// Objective-C runtime core: selectors, class registry, instances, run-time class construction, native classes, calls into guest code.
#include <algorithm>
#include <cstring>
#include <format>
#include <functional>

#include "cider/libsystem.hpp"
#include "cider/log.hpp"
#include "cider/objc.hpp"
#include "../libsystem/internal.hpp"

extern "C" {
extern uint64_t cider_objc_lookup_thunk;
extern uint64_t cider_objc_lookup_super_thunk;
void cider_objc_nil_return();
}

namespace cider::objc {
namespace {
Runtime* g_runtime = nullptr;
constexpr size_t kArenaSize = 4u << 20;
constexpr uint64_t kMaxInstanceSize = 256ull << 20;

// host functions behind the dispatch stubs (positional thunks)
uint64_t host_lookup(uint64_t self, uint64_t sel) { return g_runtime ? g_runtime->lookup_for_message(self, sel) : 0; }
uint64_t host_lookup_super(uint64_t super, uint64_t sel, uint64_t mode) { return g_runtime ? g_runtime->lookup_for_super(super, sel, static_cast<int>(mode)) : 0; }
uint64_t host_forward_trap(uint64_t self, uint64_t sel);
}  // namespace

Runtime* Runtime::current() { return g_runtime; }

Runtime::Runtime(libsystem::RuntimeContext& ctx) : ctx_(ctx) {
    g_runtime = this;
    auto region = memory::Region::reserve(0, kArenaSize, false);
    if (region.ok() && region.value().commit(0, kArenaSize).ok()) arena_ = std::move(region.value());
    else failed_ = true;
    std::vector<abi::ThunkSpec> specs = {{reinterpret_cast<void*>(&host_lookup), false},
                                         {reinterpret_cast<void*>(&host_lookup_super), false},
                                         {reinterpret_cast<void*>(&host_forward_trap), false}};
    auto t = abi::ThunkTable::build_specs(specs);
    if (!t.ok()) { failed_ = true; return; }
    core_table_ = std::move(t.value());
    lookup_thunk_ = core_table_.address(0);
    lookup_super_thunk_ = core_table_.address(1);
    forward_imp_ = core_table_.address(2);
    nil_imp_ = reinterpret_cast<Addr>(&cider_objc_nil_return);
    cider_objc_lookup_thunk = lookup_thunk_;
    cider_objc_lookup_super_thunk = lookup_super_thunk_;
}

Runtime::~Runtime() {
    cider_objc_lookup_thunk = 0;
    cider_objc_lookup_super_thunk = 0;
    if (g_runtime == this) g_runtime = nullptr;
}

// ---- guest memory checks ---------------------------------------------------------------------------------------------------------
bool Runtime::readable(Addr a, size_t n) const {
    if (a == 0 || n == 0 || a + n < a) return false;
    Addr p = a;
    const Addr end = a + n;
    while (p < end) {
        const auto info = memory::query_address(reinterpret_cast<const void*>(p));
        if (info.state != memory::PageInfo::State::Committed || info.prot == memory::Prot::None) return false;
        const Addr next = (p & ~Addr{0xfff}) + 0x1000;
        p = next;
    }
    return true;
}

bool Runtime::read_cstring(Addr a, std::string& out, size_t max) const {
    out.clear();
    while (out.size() <= max) {
        if (!readable(a + out.size(), 1)) return false;
        const char c = *reinterpret_cast<const char*>(a + out.size());
        if (c == 0) return true;
        out.push_back(c);
    }
    return false;
}

bool Runtime::write_u32(Addr a, uint32_t v) {
    if (!readable(a, 4)) return false;
    const auto info = memory::query_address(reinterpret_cast<const void*>(a));
    if (!info.writable) return false;
    std::memcpy(reinterpret_cast<void*>(a), &v, 4);
    return true;
}

Addr Runtime::arena_alloc(size_t size, size_t align) {
    const size_t start = (arena_used_ + align - 1) & ~(align - 1);
    if (!arena_.valid() || start + size > kArenaSize) { failed_ = true; return 0; }
    arena_used_ = start + size;
    return reinterpret_cast<Addr>(arena_.base()) + start;
}

// ---- selectors ---------------------------------------------------------------------------------------------------------------------
Addr Runtime::sel(std::string_view name) {
    auto it = sels_.find(name);
    if (it != sels_.end()) return it->second;
    sel_storage_.emplace_back(name);
    const std::string& s = sel_storage_.back();
    const Addr a = reinterpret_cast<Addr>(s.c_str());       // deque never moves its elements: the pointer is the unique SEL
    sels_.emplace(std::string_view(s), a);
    sel_names_.emplace(a, std::string_view(s));
    return a;
}

std::string_view Runtime::sel_name(Addr s) const {
    auto it = sel_names_.find(s);
    return it == sel_names_.end() ? std::string_view() : it->second;
}

Addr Runtime::protocol_handle(const std::string& name) {
    auto p = protocols_.find(name);
    if (p == protocols_.end()) return 0;
    if (!p->second.handle) {
        p->second.handle = arena_alloc(16, 8);
        if (p->second.handle) proto_handles_[p->second.handle] = name;
    }
    return p->second.handle;
}
const std::string* Runtime::protocol_name_of(Addr handle) const {
    auto it = proto_handles_.find(handle);
    return it == proto_handles_.end() ? nullptr : &it->second;
}
bool Runtime::conforms_to(Class* c, const std::string& protocol) const {
    std::function<bool(const std::string&, int)> has = [&](const std::string& n, int depth) {
        if (n == protocol) return true;
        if (depth > 8) return false;
        auto it = protocols_.find(n);
        if (it == protocols_.end()) return false;
        for (const auto& parent : it->second.parents)
            if (has(parent, depth + 1)) return true;
        return false;
    };
    for (Class* k = c; k; k = k->super)
        for (const auto& n : k->protocols)
            if (has(n, 0)) return true;
    return false;
}

// ---- classes -------------------------------------------------------------------------------------------------------------------------
Class* Runtime::class_named(std::string_view name) const {
    auto it = by_name_.find(std::string(name));
    return it == by_name_.end() ? nullptr : it->second;
}
Class* Runtime::class_at(Addr cls) const {
    auto it = by_addr_.find(cls);
    return it == by_addr_.end() ? nullptr : it->second;
}
Class* Runtime::class_of_object(Addr obj) const {
    if (!readable(obj, 8)) return nullptr;
    return class_at(*reinterpret_cast<const Addr*>(obj));
}

Class* Runtime::make_class(Addr cls_addr, bool meta) {
    auto c = std::make_unique<Class>();
    c->addr = cls_addr;
    c->is_meta = meta;
    Class* raw = c.get();
    classes_.push_back(std::move(c));
    by_addr_[cls_addr] = raw;
    return raw;
}

void Runtime::rebuild_index(Class* c) {
    c->by_sel.clear();
    for (size_t i = 0; i < c->methods.size(); ++i) c->by_sel[c->methods[i].sel] = i;   // later entries (categories) win
}

void Runtime::flush_caches() {
    for (auto& c : classes_) c->cache.clear();
}

bool Runtime::is_kind_of(Class* c, Class* ancestor) const {
    for (Class* k = c; k; k = k->super)
        if (k == ancestor) return true;
    return false;
}

Method* Runtime::find_method(Class* c, Addr s) {
    for (Class* k = c; k; k = k->super) {
        auto it = k->by_sel.find(s);
        if (it != k->by_sel.end()) return &k->methods[it->second];
    }
    return nullptr;
}

Addr Runtime::find_imp(Class* c, Addr s) {
    auto it = c->cache.find(s);
    if (it != c->cache.end()) return it->second;
    Method* m = find_method(c, s);
    const Addr imp = m ? m->imp : 0;
    c->cache[s] = imp;
    if (imp && log::enabled(log::Level::Info) && traced_ < 64) {          // first dispatch of each (class, selector): visible at the default run log level
        ++traced_;
        log::info(log::Category::Objc, "Message dispatched: {}[{} {}]", c->is_meta ? '+' : '-', c->is_meta ? c->instance_class->name : c->name, sel_name(s));
    }
    return imp;
}

Addr Runtime::method_handle(Class*, Method& m) {
    if (m.handle) return m.handle;
    const Addr h = arena_alloc(24, 8);
    if (!h) return 0;
    uint64_t rec[3] = {m.sel, 0, m.imp};
    std::memcpy(reinterpret_cast<void*>(h), rec, sizeof rec);
    m.handle = h;
    method_handles_[h] = &m;
    return h;
}
Method* Runtime::method_from_handle(Addr h) {
    auto it = method_handles_.find(h);
    return it == method_handles_.end() ? nullptr : it->second;
}
Addr Runtime::ivar_handle(Class* c, Ivar& v) {
    if (v.handle) return v.handle;
    const Addr h = arena_alloc(32, 8);
    if (!h) return 0;
    v.handle = h;
    ivar_handles_[h] = {c, static_cast<size_t>(&v - c->ivars.data())};
    return h;
}
Ivar* Runtime::ivar_from_handle(Addr h) {
    auto it = ivar_handles_.find(h);
    if (it == ivar_handles_.end()) return nullptr;
    return &it->second.first->ivars[it->second.second];
}

// ---- instances --------------------------------------------------------------------------------------------------------------------
Addr Runtime::create_instance(Class* c, size_t extra) {
    if (!c || c->is_meta || !c->realized) return 0;
    const uint64_t size = static_cast<uint64_t>(c->instance_size) + extra;
    if (size < 8 || size > kMaxInstanceSize) return 0;
    void* p = libsystem::detail::guest_malloc(size);
    if (!p) return 0;
    std::memset(p, 0, static_cast<size_t>(size));
    const Addr obj = reinterpret_cast<Addr>(p);
    *reinterpret_cast<Addr*>(p) = c->addr;
    live_[obj] = ObjectState{};
    return obj;
}

ObjectState* Runtime::object_state(Addr obj) {
    auto it = live_.find(obj);
    return it == live_.end() ? nullptr : &it->second;
}

void Runtime::dispose_instance(Addr obj) {
    auto it = live_.find(obj);
    if (it == live_.end()) return;
    clear_weak_refs_to(obj);
    live_.erase(it);
    libsystem::detail::guest_free(reinterpret_cast<void*>(obj));
}

// ---- run-time class construction ---------------------------------------------------------------------------------------------------
Class* Runtime::allocate_class_pair(Class* super, const std::string& name, size_t extra_bytes) {
    if (name.empty() || by_name_.count(name)) return nullptr;
    if (super && (super->is_meta || !super->realized)) return nullptr;
    const Addr cls_addr = arena_alloc(40, 8), meta_addr = arena_alloc(40, 8), ro = arena_alloc(72, 8), mro = arena_alloc(72, 8);
    if (!cls_addr || !meta_addr || !ro || !mro) return nullptr;
    Class* cls = make_class(cls_addr, false);
    Class* meta = make_class(meta_addr, true);
    cls->name = meta->name = name;
    cls->meta = meta;
    meta->instance_class = cls;
    cls->super = super;
    cls->root = super == nullptr;
    meta->super = super ? super->meta : cls;
    meta->root = false;
    cls->instance_start = cls->instance_size = static_cast<uint32_t>(super ? super->instance_size : 0);
    meta->instance_size = 40;
    cls->instance_size += static_cast<uint32_t>(extra_bytes);
    cls->realized = meta->realized = true;
    cls->under_construction = true;
    cls->ro = ro;
    meta->ro = mro;
    // the guest-visible class_t: isa, superclass, cache, vtable, data
    Class* rm = super ? super : cls;
    while (rm->super) rm = rm->super;
    uint64_t ct[5] = {meta_addr, super ? super->addr : 0, dynamic_symbol("__objc_empty_cache").value_or(0), 0, ro};
    uint64_t mt[5] = {rm->meta->addr, super ? super->meta->addr : cls_addr, ct[2], 0, mro};
    std::memcpy(reinterpret_cast<void*>(cls_addr), ct, sizeof ct);
    std::memcpy(reinterpret_cast<void*>(meta_addr), mt, sizeof mt);
    cls->raw_super = ct[1];
    return cls;
}

bool Runtime::register_class_pair(Class* cls) {
    if (!cls || !cls->under_construction || by_name_.count(cls->name)) return false;
    cls->under_construction = false;
    cls->registered = cls->meta->registered = true;
    by_name_[cls->name] = cls;
    rebuild_index(cls);
    rebuild_index(cls->meta);
    return true;
}

bool Runtime::dispose_class_pair(Class* cls) {
    if (!cls || cls->is_meta) return false;
    for (const auto& kv : live_)
        if (class_of_object(kv.first) == cls) return false;       // instances still exist: refuse, like the real runtime's UB made explicit
    for (auto& other : classes_)
        if (other->super == cls) return false;
    by_name_.erase(cls->name);
    by_addr_.erase(cls->addr);
    by_addr_.erase(cls->meta->addr);
    cls->name.clear();
    cls->meta->name.clear();
    cls->registered = cls->meta->registered = false;
    cls->realized = cls->meta->realized = false;
    flush_caches();
    return true;
}

bool Runtime::add_method(Class* c, Addr s, const std::string& types, Addr imp, bool replace, Addr* old_imp) {
    if (!c || !s || !imp) return false;
    auto it = c->by_sel.find(s);
    const bool own = it != c->by_sel.end();
    if (own && !replace) return false;
    if (old_imp) *old_imp = own ? c->methods[it->second].imp : 0;
    if (own) {
        c->methods[it->second].imp = imp;
        c->methods[it->second].types = types;
    } else {
        Method m;
        m.name = std::string(sel_name(s));
        m.sel = s;
        m.types = types;
        m.imp = imp;
        c->methods.push_back(std::move(m));
        c->by_sel[s] = c->methods.size() - 1;
    }
    flush_caches();
    return true;
}

// ---- native classes ----------------------------------------------------------------------------------------------------------------------
Class* Runtime::declare_native_class(const std::string& name, const std::string& super_name, uint32_t instance_size) {
    Class* super = super_name.empty() ? nullptr : class_named(super_name);
    if (!super_name.empty() && !super) { failed_ = true; return nullptr; }
    Class* cls = allocate_class_pair(super, name, 0);
    if (!cls) { failed_ = true; return nullptr; }
    cls->instance_size = instance_size;
    cls->native = cls->meta->native = true;
    native_owner_of_[name] = native_owner_;
    register_class_pair(cls);
    return cls;
}

void Runtime::add_native_methods(Class* cls, bool class_methods, const std::vector<NativeMethod>& methods) {
    Class* target = class_methods ? cls->meta : cls;
    for (const auto& nm : methods) {
        Method m;
        m.name = nm.selector;
        m.sel = sel(nm.selector);
        m.types = nm.types;
        m.imp = 0;
        target->methods.push_back(std::move(m));
        native_.push_back({{cls, class_methods, target->methods.size() - 1}, nm});
    }
    rebuild_index(target);
}

bool Runtime::build_native_thunks(std::string& error) {
    std::vector<abi::ThunkSpec> specs;
    for (auto& e : native_) specs.push_back({e.second.fn, e.second.raw});
    auto t = abi::ThunkTable::build_specs(specs);
    if (!t.ok()) { error = t.error().message; return false; }
    native_table_ = std::move(t.value());
    for (size_t i = 0; i < native_.size(); ++i) {
        Class* target = native_[i].first.class_method ? native_[i].first.cls->meta : native_[i].first.cls;
        target->methods[native_[i].first.index].imp = native_table_.address(i);
    }
    flush_caches();
    return true;
}

std::pair<Addr, Addr> Runtime::thunk_range() const {
    if (native_.empty()) return {0, 0};
    return {native_table_.address(0), native_table_.address(0) + native_table_.size() * abi::ThunkTable::kStride};
}

// ---- symbols visible to the dynamic linker ------------------------------------------------------------------------------------------
std::optional<uint64_t> Runtime::dynamic_symbol(std::string_view name) {
    if (name == "__objc_empty_cache" || name == "_objc_empty_cache") {
        static Addr empty = 0;
        if (!empty || !arena_.valid()) empty = arena_alloc(32, 16);
        return empty;
    }
    constexpr std::string_view kClass = "_OBJC_CLASS_$_", kMeta = "_OBJC_METACLASS_$_";
    if (name.substr(0, kClass.size()) == kClass) {
        Class* c = class_named(name.substr(kClass.size()));
        if (c && c->native) return c->addr;
    } else if (name.substr(0, kMeta.size()) == kMeta) {
        Class* c = class_named(name.substr(kMeta.size()));
        if (c && c->native) return c->meta->addr;
    } else if (name == "___CFConstantStringClassReference") {
        Class* c = class_named("NSConstantString");
        if (c) return c->addr;
    }
    for (auto& p : providers_)
        if (auto v = p(name)) return v;
    return std::nullopt;
}

// ---- calling guest / native code ---------------------------------------------------------------------------------------------------
uint64_t Runtime::call_imp(Addr imp, std::initializer_list<uint64_t> args) {
    abi::GuestCallArgs a;
    for (uint64_t v : args) a.push_int(v);
    abi::CallOutcome o;
    if (abi::cider_current_call) {
        o = abi::call_guest_nested(imp, a);
    } else if (libsystem::ThreadState* ts = libsystem::GuestThreads::current(); ts && !ts->is_main && ts->stack) {
        o = abi::call_guest_args(imp, *ts->stack, ts->stack->top() - 64, a);          // host-implemented thread (dispatch worker): its own stack
    } else if (top_stack_) {
        o = abi::call_guest_args(imp, *top_stack_, top_sp_, a);
    } else {
        libsystem::detail::trap("objc: no guest stack available for a call");
        return 0;
    }
    if (o.refused) { libsystem::detail::trap("objc: call refused: " + o.refusal); return 0; }
    return o.value;
}

uint64_t Runtime::send(Addr self, Addr s, std::initializer_list<uint64_t> args) {
    if (!self) return 0;
    Class* c = class_of_object(self);
    if (!c) { libsystem::detail::trap(std::format("objc: message to an invalid object {:#x}", self)); return 0; }
    const Addr imp = find_imp(c, s);
    if (!imp) {
        libsystem::detail::trap(std::format("objc: {}[{} {}]: unrecognized selector sent to {} {:#x}", c->is_meta ? '+' : '-', c->is_meta ? c->instance_class->name : c->name,
                                            sel_name(s), c->is_meta ? "class" : "instance", self));
        return 0;
    }
    ++messages_sent;
    std::vector<uint64_t> all = {self, s};
    all.insert(all.end(), args.begin(), args.end());
    abi::GuestCallArgs a;
    for (uint64_t v : all) a.push_int(v);
    abi::CallOutcome o;
    if (abi::cider_current_call) o = abi::call_guest_nested(imp, a);
    else if (top_stack_) o = abi::call_guest_args(imp, *top_stack_, top_sp_, a);
    else { libsystem::detail::trap("objc: no guest stack available for a message"); return 0; }
    if (o.refused) { libsystem::detail::trap("objc: message refused: " + o.refusal); return 0; }
    return o.value;
}

uint64_t Runtime::send(Addr self, std::string_view selector, std::initializer_list<uint64_t> args) { return send(self, sel(selector), args); }

namespace {
uint64_t host_forward_trap(uint64_t self, uint64_t sel) {
    Runtime* rt = Runtime::current();
    std::string what = "unrecognized selector";
    if (rt) {
        Class* c = rt->class_of_object(self);
        if (c)
            what = std::format("{}[{} {}]: unrecognized selector sent to {} {:#x}", c->is_meta ? '+' : '-', c->is_meta ? c->instance_class->name : c->name, rt->sel_name(sel),
                               c->is_meta ? "class" : "instance", self);
    }
    libsystem::detail::trap("objc: " + what);
    return 0;
}
}  // namespace

}  // namespace cider::objc

namespace cider::objc {
std::vector<std::string> Runtime::native_class_names(const std::string& owner) const {
    std::vector<std::string> out;
    for (const auto& c : classes_) {
        if (!c->native || c->is_meta) continue;
        auto it = native_owner_of_.find(c->name);
        if (owner.empty() || (it != native_owner_of_.end() && it->second == owner)) out.push_back(c->name);
    }
    return out;
}
}  // namespace cider::objc
