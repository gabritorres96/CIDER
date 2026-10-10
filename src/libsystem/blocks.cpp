// Apple Blocks ABI subset (CIDER 0.5.1): _Block_copy, _Block_release, _Block_object_assign/_dispose and the three concrete block classes.
//
// A block literal emitted by clang is { isa, flags, reserved, invoke, descriptor, captures... }. The runtime never calls `invoke` for the
// guest (compiled code does); it only copies stack blocks to the heap, keeps heap blocks' reference counts, and runs the copy/dispose
// helpers the compiler generated for captured objects, blocks and __block variables. Helpers are guest code, entered through nested
// callbacks. Not implemented: garbage-collected blocks (BLOCK_IS_GC), _Block_use_RR2, block layout strings (never interpreted),
// Block_size, C++ constructors in blocks (BLOCK_HAS_CTOR is just a copy helper here).
#include <cstring>
#include <format>
#include <unordered_set>

#include "cider/objc.hpp"
#include "internal.hpp"

namespace cider::libsystem::detail {
namespace {

constexpr int32_t kRefcountMask = 0xfffe, kNeedsFree = 1 << 24, kHasCopyDispose = 1 << 25, kIsGlobal = 1 << 28, kDeallocating = 0x0001;
constexpr int kFieldObject = 3, kFieldBlock = 7, kFieldByref = 8, kFieldWeak = 16, kFieldCaller = 128;
constexpr int32_t kByrefNeedsFree = 1 << 24, kByrefHasCopyDispose = 1 << 25;

struct BlockLiteral {
    uint64_t isa;
    int32_t flags;
    int32_t reserved;
    uint64_t invoke;
    uint64_t descriptor;
};
struct BlockDescriptor {
    uint64_t reserved;
    uint64_t size;
    // if (flags & kHasCopyDispose): uint64_t copy, dispose;  then if (flags & kHasSignature): signature
};
struct Byref {
    uint64_t isa;
    Byref* forwarding;
    int32_t flags;
    uint32_t size;
    uint64_t keep, destroy;                 // only when kByrefHasCopyDispose; otherwise the variable starts at keep's offset
};

uint64_t g_stack_isa = 0, g_global_isa = 0, g_malloc_isa = 0;
std::unordered_set<uint64_t> g_heap_blocks, g_heap_byrefs;       // blocks/byrefs allocated here: the only ones that may be freed

bool ok(uint64_t addr, size_t n) {
    RuntimeContext* c = context();
    return c && c->objc && c->objc->readable(addr, n);
}
void call(uint64_t fn, uint64_t a, uint64_t b) {
    RuntimeContext* c = context();
    if (c && c->objc) c->objc->call_imp(fn, {a, b});
}
void call1(uint64_t fn, uint64_t a) {
    RuntimeContext* c = context();
    if (c && c->objc) c->objc->call_imp(fn, {a});
}

bool copy_dispose_of(const BlockLiteral* b, uint64_t& copy, uint64_t& dispose) {
    copy = dispose = 0;
    if (!(b->flags & kHasCopyDispose)) return true;
    if (!ok(b->descriptor, sizeof(BlockDescriptor) + 16)) return false;
    const auto* d = reinterpret_cast<const uint64_t*>(b->descriptor);
    copy = d[2];
    dispose = d[3];
    return true;
}

// ---- _Block_copy / _Block_release ------------------------------------------------------------------------------------------------------
uint64_t w_block_copy(uint64_t arg) {
    if (!arg) return 0;
    if (!ok(arg, sizeof(BlockLiteral))) { trap(std::format("_Block_copy: {:#x} is not readable memory", arg)); return 0; }
    auto* b = reinterpret_cast<BlockLiteral*>(arg);
    if (g_heap_blocks.count(arg)) {                                   // heap block: one more reference
        if ((b->flags & kRefcountMask) != kRefcountMask) b->flags += 2;
        return arg;
    }
    if (b->flags & kIsGlobal) return arg;                             // global block: immortal
    if (!ok(b->descriptor, sizeof(BlockDescriptor))) { trap("_Block_copy: block with an unreadable descriptor"); return 0; }
    const auto* d = reinterpret_cast<const BlockDescriptor*>(b->descriptor);
    if (d->size < sizeof(BlockLiteral) || d->size > (16u << 20)) { trap(std::format("_Block_copy: invalid block size {}", d->size)); return 0; }
    uint64_t copy_fn, dispose_fn;
    if (!copy_dispose_of(b, copy_fn, dispose_fn)) { trap("_Block_copy: unreadable copy/dispose helpers"); return 0; }
    if (!ok(arg, d->size)) { trap("_Block_copy: block body is not readable"); return 0; }
    auto* h = static_cast<BlockLiteral*>(guest_malloc(d->size));
    if (!h) { fail_errno(kENOMEM); return 0; }
    std::memcpy(h, b, d->size);
    h->flags = (h->flags & ~(kRefcountMask | kDeallocating)) | kNeedsFree | 2;
    h->isa = g_malloc_isa;
    g_heap_blocks.insert(reinterpret_cast<uint64_t>(h));
    if (copy_fn) call(copy_fn, reinterpret_cast<uint64_t>(h), arg);       // retains captured objects, copies captured blocks and __block variables
    return reinterpret_cast<uint64_t>(h);
}

void w_block_release(uint64_t arg) {
    if (!arg) return;
    if (!g_heap_blocks.count(arg)) {
        if (ok(arg, sizeof(BlockLiteral)) && (reinterpret_cast<BlockLiteral*>(arg)->flags & kIsGlobal)) return;
        if (ok(arg, 8) && *reinterpret_cast<const uint64_t*>(arg) == g_stack_isa) return;     // stack block: nothing to release
        trap(std::format("_Block_release of {:#x}, which is not a heap block (double release or invalid pointer)", arg));
        return;
    }
    auto* b = reinterpret_cast<BlockLiteral*>(arg);
    const int32_t count = b->flags & kRefcountMask;
    if (count == kRefcountMask) return;                               // latched
    if (count > 2) { b->flags -= 2; return; }
    b->flags = (b->flags & ~kRefcountMask) | kDeallocating;
    uint64_t copy_fn, dispose_fn;
    if (copy_dispose_of(b, copy_fn, dispose_fn) && dispose_fn) call1(dispose_fn, arg);
    g_heap_blocks.erase(arg);
    b->isa = 0;                                                        // a stale pointer is no longer a block
    guest_free(b);
}

// ---- __block variables ---------------------------------------------------------------------------------------------------------------
Byref* byref_copy(Byref* src) {
    if (!ok(reinterpret_cast<uint64_t>(src), sizeof(Byref))) { trap("_Block_object_assign: unreadable __block variable"); return nullptr; }
    if (g_heap_byrefs.count(reinterpret_cast<uint64_t>(src->forwarding))) {         // already on the heap
        if ((src->forwarding->flags & kRefcountMask) != kRefcountMask) src->forwarding->flags += 2;
        return src->forwarding;
    }
    if (src->forwarding != src) { trap("_Block_object_assign: corrupt __block variable (forwarding pointer)"); return nullptr; }
    if (src->size < 24 || src->size > (16u << 20)) { trap(std::format("_Block_object_assign: invalid __block size {}", src->size)); return nullptr; }
    auto* copy = static_cast<Byref*>(guest_malloc(src->size));
    if (!copy) { fail_errno(kENOMEM); return nullptr; }
    std::memcpy(copy, src, src->size);
    copy->flags = (src->flags & ~kRefcountMask) | kByrefNeedsFree | 4;     // two references: the copying block and the frame that still disposes its own
    copy->forwarding = copy;
    src->forwarding = copy;                                           // the stack original now forwards to the heap copy
    g_heap_byrefs.insert(reinterpret_cast<uint64_t>(copy));
    if (src->flags & kByrefHasCopyDispose) call(src->keep, reinterpret_cast<uint64_t>(copy), reinterpret_cast<uint64_t>(src));
    return copy;
}
void byref_release(Byref* byref) {
    if (!byref) return;
    Byref* f = byref->forwarding;
    if (!g_heap_byrefs.count(reinterpret_cast<uint64_t>(f))) return;  // never copied: still on the stack
    const int32_t count = f->flags & kRefcountMask;
    if (count == kRefcountMask) return;
    if (count > 2) { f->flags -= 2; return; }
    if (f->flags & kByrefHasCopyDispose) call1(f->destroy, reinterpret_cast<uint64_t>(f));
    g_heap_byrefs.erase(reinterpret_cast<uint64_t>(f));
    guest_free(f);
}

// void _Block_object_assign(void* dest, const void* object, int flags)
void w_block_object_assign(uint64_t dest, uint64_t object, int flags) {
    if (!ok(dest, 8)) { trap("_Block_object_assign: destination is not writable memory"); return; }
    auto* slot = reinterpret_cast<uint64_t*>(dest);
    if (flags & kFieldByref) {
        *slot = reinterpret_cast<uint64_t>(byref_copy(reinterpret_cast<Byref*>(object)));
    } else if ((flags & (kFieldBlock | kFieldCaller)) == kFieldBlock) {
        *slot = w_block_copy(object);
    } else if ((flags & (kFieldObject | kFieldCaller)) == kFieldObject && !(flags & kFieldWeak)) {
        RuntimeContext* c = context();
        if (c && c->objc && object) c->objc->retain(object);
        *slot = object;
    } else {
        *slot = object;                                                // weak fields and BYREF_CALLER assignments do not retain
    }
}
// void _Block_object_dispose(const void* object, int flags)
void w_block_object_dispose(uint64_t object, int flags) {
    if (flags & kFieldByref) byref_release(reinterpret_cast<Byref*>(object));
    else if ((flags & (kFieldBlock | kFieldCaller)) == kFieldBlock) w_block_release(object);
    else if ((flags & (kFieldObject | kFieldCaller)) == kFieldObject && !(flags & kFieldWeak)) {
        RuntimeContext* c = context();
        if (c && c->objc && object) c->objc->release(object);
    }
}

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

bool block_is_object(uint64_t obj) {
    if (!g_malloc_isa || !ok(obj, 8)) return false;
    const uint64_t isa = *reinterpret_cast<const uint64_t*>(obj);
    return isa == g_malloc_isa || isa == g_global_isa || isa == g_stack_isa;
}
uint64_t block_retain(uint64_t obj) {
    const uint64_t isa = *reinterpret_cast<const uint64_t*>(obj);
    if (isa == g_global_isa) return obj;
    return w_block_copy(obj);                                         // stack: copy to the heap; heap: +1
}
void block_release(uint64_t obj) { w_block_release(obj); }
namespace {
uint64_t m_copy(uint64_t self, uint64_t) { return w_block_copy(self); }
uint64_t m_retain(uint64_t self, uint64_t) { return block_retain(self); }
void m_release(uint64_t self, uint64_t) { w_block_release(self); }
uint64_t m_autorelease(uint64_t self, uint64_t) {
    RuntimeContext* c = context();
    return c && c->objc ? c->objc->autorelease(self) : self;
}
}  // namespace

void install_block_classes(objc::Runtime& rt) {
    rt.set_native_owner("libSystem");
    objc::Class* base = rt.declare_native_class("NSBlock", "NSObject", 8);
    objc::Class* st = rt.declare_native_class("__NSStackBlock__", "NSBlock", 8);
    objc::Class* gl = rt.declare_native_class("__NSGlobalBlock__", "NSBlock", 8);
    objc::Class* ma = rt.declare_native_class("__NSMallocBlock__", "NSBlock", 8);
    if (!base || !st || !gl || !ma) return;
    rt.add_native_methods(base, false, {{"copy", "@16@0:8", addr(m_copy), false},
                                        {"retain", "@16@0:8", addr(m_retain), false},
                                        {"release", "Vv16@0:8", addr(m_release), false},
                                        {"autorelease", "@16@0:8", addr(m_autorelease), false}});
    g_stack_isa = st->addr;
    g_global_isa = gl->addr;
    g_malloc_isa = ma->addr;
    g_heap_blocks.clear();
    g_heap_byrefs.clear();
}

void add_block_symbols(std::vector<HostSymbol>& t) {
    auto fn = [&](const char* name, void* f) { t.push_back(HostSymbol{name, f, false, false, false, Lib::System}); };
    fn("__Block_copy", addr(w_block_copy));
    fn("__Block_release", addr(w_block_release));
    fn("__Block_object_assign", addr(w_block_object_assign));
    fn("__Block_object_dispose", addr(w_block_object_dispose));
}

}  // namespace cider::libsystem::detail
