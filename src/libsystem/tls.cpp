// Thread-local storage for the Mach-O x86_64 TLV protocol, WITHOUT touching Windows' own TLS machinery.
//
// What clang emits on Darwin x86_64 for `__thread int x;`:
//     movq  _x@TLVP(%rip), %rdi        ; address of the variable's DESCRIPTOR {thunk, key, offset}
//     callq *(%rdi)                    ; call descriptor->thunk(descriptor) -> rax = address of this thread's x
// The guest never reads %gs itself. On macOS, dyld replaces `thunk` (bound to _tlv_bootstrap) by libdyld's
// tlv_get_addr, which does use %gs. ORVYN binds the thunk to the function below instead: it keeps one lazily created
// block per image (copy of the image's __thread_data template + zeroed __thread_bss) in the guest heap and returns
// block + descriptor->offset. The Windows TEB, %gs, TlsAlloc slots and the host's thread_local data are never read,
// written or replaced. There is exactly one guest thread, so "per thread" and "per process" coincide.
#include <cstring>

#include "orvyn/guest_threads.hpp"
#include "orvyn/libsystem.hpp"
#include "internal.hpp"

namespace orvyn::libsystem {
namespace {

void* w_tlv_get_addr(const uint64_t* descriptor) {
    RuntimeContext* c = context();
    const uint64_t d = reinterpret_cast<uint64_t>(descriptor);
    if (c) {
        for (size_t image = 0; image < c->tls_images.size(); ++image) {
            TlsImage& t = c->tls_images[image];
            if (d < t.lo || d >= t.hi) continue;
            const uint64_t offset = descriptor[2];
            if (offset >= t.tmpl_size) {
                detail::trap("tlv_get_addr: descriptor offset is outside the thread-local template");
                return nullptr;
            }
            // One block per image PER GUEST THREAD: each thread starts from the template (see guest_threads.hpp).
            ThreadState* me = GuestThreads::current();
            uint8_t** slot = &t.block;
            if (me) {
                if (me->tls_blocks.size() <= image) me->tls_blocks.resize(c->tls_images.size(), nullptr);
                slot = &me->tls_blocks[image];
            }
            if (!*slot) {
                const uint64_t align = t.align < 16 ? 16 : t.align;
                uint8_t* raw = static_cast<uint8_t*>(detail::guest_malloc(t.tmpl_size + align));
                if (!raw) { detail::trap("tlv_get_addr: out of memory for thread-local storage"); return nullptr; }
                uint8_t* aligned = reinterpret_cast<uint8_t*>((reinterpret_cast<uint64_t>(raw) + align - 1) & ~(align - 1));
                std::memset(aligned, 0, t.tmpl_size);
                std::memcpy(aligned, reinterpret_cast<const void*>(t.tmpl), t.init_size);   // __thread_data; the rest (__thread_bss) stays zero
                *slot = aligned;
            }
            return *slot + offset;
        }
    }
    detail::trap("tlv_get_addr: descriptor does not belong to a loaded image with thread-local storage");
    return nullptr;
}

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

namespace detail {
void add_tls_symbols(std::vector<HostSymbol>& t) {
    t.push_back({"__tlv_bootstrap", addr(w_tlv_get_addr), false, true});
    t.push_back({"_tlv_get_addr", addr(w_tlv_get_addr), false, true});
}
}  // namespace detail

}  // namespace orvyn::libsystem
