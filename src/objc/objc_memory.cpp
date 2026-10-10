// Objective-C object lifetime: retain counts, deallocation, autorelease pools, weak references and the ARC entry points clang emits.
//
// Model (reference counting only, no garbage collection):
//   * every object created by class_createInstance starts with retain count 1 and lives in `live_` until it is disposed;
//   * objc_release of the last reference sends -dealloc (guest or native code), then makes sure the memory is released;
//   * retain/release of an address that is not a live object is accepted only for objects that sit inside a loaded image (constant
//     strings and other statically allocated objects are immortal); anything else is a use-after-free/over-release and ends the guest
//     with a precise message instead of corrupting memory;
//   * autorelease pools are per guest thread; weak references are registered per location and zeroed when the object deallocates.
#include <algorithm>
#include <cstring>
#include <format>

#include "cider/guest_threads.hpp"
#include "cider/libsystem.hpp"
#include "cider/log.hpp"
#include "cider/objc.hpp"
#include "../libsystem/internal.hpp"

namespace cider::objc {
namespace {
constexpr Addr kPoolTokenBase = 0x7e0000000000ull;

std::vector<std::vector<Addr>>& pools_of_thread() {
    static std::vector<std::vector<Addr>> fallback;
    if (libsystem::ThreadState* ts = libsystem::GuestThreads::current()) return ts->pools;
    return fallback;
}
}  // namespace

// ---- immortal objects --------------------------------------------------------------------------------------------------------------
bool Runtime::is_static_object(Addr obj) const {
    for (const auto& r : image_ranges_)
        if (obj >= r.first && obj < r.second) return true;
    // objects that live inside the runtime's own arena (class objects, constant data)
    const Addr lo = reinterpret_cast<Addr>(arena_.base());
    return arena_.valid() && obj >= lo && obj < lo + arena_.size();
}

Addr Runtime::retain(Addr obj) {
    if (!obj) return 0;
    auto it = live_.find(obj);
    if (it != live_.end()) { ++it->second.rc; return obj; }
    if (is_static_object(obj)) return obj;                          // constant strings, class objects: immortal
    libsystem::detail::trap(std::format("objc: retain of {:#x}, which is not a live object (use after free or invalid pointer)", obj));
    return 0;
}

void Runtime::destruct_instance(Addr obj) {
    Class* c = class_of_object(obj);
    if (!c) return;
    // .cxx_destruct (emitted by ARC for classes with strong/weak ivars) destroys the ivars of ONE class: run it for each class of the chain
    const Addr dtor = sel(".cxx_destruct");
    for (Class* k = c; k; k = k->super) {
        if (!(k->ro_flags & kRoHasCxxStructors)) continue;
        auto it = k->by_sel.find(dtor);
        if (it != k->by_sel.end()) call_imp(k->methods[it->second].imp, {obj, dtor});
    }
}

void Runtime::release(Addr obj) {
    if (!obj) return;
    auto it = live_.find(obj);
    if (it == live_.end()) {
        if (is_static_object(obj)) return;
        libsystem::detail::trap(std::format("objc: release of {:#x}, which is not a live object (over-release, double free or invalid pointer)", obj));
        return;
    }
    if (it->second.rc > 1) { --it->second.rc; return; }
    if (it->second.deallocating) return;                            // release during dealloc (e.g. by a weak load): ignore
    it->second.deallocating = true;
    Class* c = class_of_object(obj);
    const Addr dealloc = sel("dealloc");
    const Addr imp = c ? find_imp(c, dealloc) : 0;
    if (imp) call_imp(imp, {obj, dealloc});
    // The deallocation must have freed the object (NSObject's dealloc, or object_dispose in a root class). If a root class forgot to,
    // free it here so nothing leaks, and say so.
    if (live_.count(obj)) {
        if (imp) log::warn(log::Category::Objc, "-dealloc of {} did not free the object: freeing it", c ? c->name : "?");
        destruct_instance(obj);
        dispose_instance(obj);
    }
}

// ---- autorelease pools ---------------------------------------------------------------------------------------------------------------
Addr Runtime::autorelease(Addr obj) {
    if (!obj) return 0;
    auto& pools = pools_of_thread();
    if (pools.empty()) {
        log::warn(log::Category::Objc, "object {:#x} autoreleased with no autorelease pool in place: leaking it", obj);
        return obj;
    }
    if (!live_.count(obj) && !is_static_object(obj)) {
        libsystem::detail::trap(std::format("objc: autorelease of {:#x}, which is not a live object", obj));
        return obj;
    }
    pools.back().push_back(obj);
    return obj;
}

Addr Runtime::pool_push() {
    auto& pools = pools_of_thread();
    pools.emplace_back();
    return kPoolTokenBase + pools.size();
}

void Runtime::pool_pop(Addr token) {
    auto& pools = pools_of_thread();
    if (token <= kPoolTokenBase || token - kPoolTokenBase > pools.size()) {
        libsystem::detail::trap(std::format("objc: autorelease pool pop with an invalid token {:#x}", token));
        return;
    }
    const size_t depth = static_cast<size_t>(token - kPoolTokenBase);      // 1-based index of the pool being popped
    while (pools.size() >= depth) {
        // objects released while draining may autorelease others into this very pool: drain until it stays empty
        while (!pools.back().empty()) {
            std::vector<Addr> batch;
            batch.swap(pools.back());
            for (auto it = batch.rbegin(); it != batch.rend(); ++it) release(*it);
        }
        pools.pop_back();
    }
}

void Runtime::drain_all_pools() {
    auto& pools = pools_of_thread();
    if (!pools.empty()) pool_pop(kPoolTokenBase + 1);
}

// ---- weak references ---------------------------------------------------------------------------------------------------------------------
void Runtime::remove_weak_location(Addr* location) {
    auto it = weak_by_location_.find(location);
    if (it == weak_by_location_.end()) return;
    auto& vec = weak_by_object_[it->second];
    vec.erase(std::remove(vec.begin(), vec.end(), location), vec.end());
    if (vec.empty()) weak_by_object_.erase(it->second);
    weak_by_location_.erase(it);
}

bool Runtime::store_weak(Addr* location, Addr obj) {
    remove_weak_location(location);
    if (!obj) { *location = 0; return true; }
    auto it = live_.find(obj);
    if (it == live_.end()) {
        if (is_static_object(obj)) { *location = obj; return true; }      // immortal: nothing to track
        *location = 0;
        return false;
    }
    if (it->second.deallocating) { *location = 0; return false; }
    it->second.weak_referenced = true;
    *location = obj;
    weak_by_object_[obj].push_back(location);
    weak_by_location_[location] = obj;
    return true;
}

Addr Runtime::load_weak(Addr* location) {
    const Addr obj = *location;
    if (!obj) return 0;
    auto it = live_.find(obj);
    if (it == live_.end()) return is_static_object(obj) ? obj : 0;
    if (it->second.deallocating) return 0;
    return obj;
}

void Runtime::clear_weak_refs_to(Addr obj) {
    auto it = weak_by_object_.find(obj);
    if (it == weak_by_object_.end()) return;
    for (Addr* loc : it->second) {
        weak_by_location_.erase(loc);
        if (readable(reinterpret_cast<Addr>(loc), 8)) *loc = 0;
    }
    weak_by_object_.erase(it);
}

// ---- ARC and runtime entry points -------------------------------------------------------------------------------------------------------
namespace {
Runtime* RT() { return Runtime::current(); }
template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

Addr w_objc_retain(Addr o) { return RT() ? RT()->retain(o) : 0; }
void w_objc_release(Addr o) { if (RT()) RT()->release(o); }
Addr w_objc_autorelease(Addr o) { return RT() ? RT()->autorelease(o) : 0; }
Addr w_objc_retainAutorelease(Addr o) { return RT() ? RT()->autorelease(RT()->retain(o)) : 0; }
Addr w_objc_retainAutoreleaseReturnValue(Addr o) { return w_objc_retainAutorelease(o); }
Addr w_objc_autoreleaseReturnValue(Addr o) { return w_objc_autorelease(o); }               // no TLS fast path: a plain autorelease, balanced by the caller's retain
Addr w_objc_retainAutoreleasedReturnValue(Addr o) { return w_objc_retain(o); }
Addr w_objc_unsafeClaimAutoreleasedReturnValue(Addr o) { return o; }
Addr w_objc_autoreleasePoolPush() { return RT() ? RT()->pool_push() : 0; }
void w_objc_autoreleasePoolPop(Addr token) { if (RT()) RT()->pool_pop(token); }
void w_objc_storeStrong(Addr* location, Addr value) {
    Runtime* rt = RT();
    if (!rt || !location) return;
    const Addr old = *location;
    if (old == value) return;
    rt->retain(value);
    *location = value;
    rt->release(old);
}
Addr w_objc_storeWeak(Addr* location, Addr value) { RT()->store_weak(location, value); return *location; }
Addr w_objc_initWeak(Addr* location, Addr value) { *location = 0; RT()->store_weak(location, value); return *location; }
void w_objc_destroyWeak(Addr* location) { RT()->remove_weak_location(location); }
Addr w_objc_loadWeakRetained(Addr* location) { Addr o = RT()->load_weak(location); return o ? RT()->retain(o) : 0; }
Addr w_objc_loadWeak(Addr* location) { Addr o = RT()->load_weak(location); return o ? RT()->autorelease(RT()->retain(o)) : 0; }
void w_objc_copyWeak(Addr* to, Addr* from) { Addr o = RT()->load_weak(from); *to = 0; RT()->store_weak(to, o); }
void w_objc_moveWeak(Addr* to, Addr* from) {
    Addr o = RT()->load_weak(from);
    RT()->remove_weak_location(from);
    *from = 0;
    *to = 0;
    RT()->store_weak(to, o);
}

// +alloc/+new/... fast paths emitted by clang for macOS >= 10.14.4: they must behave exactly like the corresponding messages
Addr w_objc_alloc(Addr cls) { return cls ? RT()->send(cls, "alloc") : 0; }
Addr w_objc_allocWithZone(Addr cls, Addr zone) { return cls ? RT()->send(cls, "allocWithZone:", {zone}) : 0; }
Addr w_objc_alloc_init(Addr cls) {
    if (!cls) return 0;
    const Addr o = RT()->send(cls, "alloc");
    return o ? RT()->send(o, "init") : 0;
}
Addr w_objc_opt_new(Addr cls) { return cls ? RT()->send(cls, "new") : 0; }
Addr w_objc_opt_class(Addr obj) { return obj ? RT()->send(obj, "class") : 0; }
int w_objc_opt_isKindOfClass(Addr obj, Addr cls) { return obj ? static_cast<int>(RT()->send(obj, "isKindOfClass:", {cls}) & 0xff) : 0; }
int w_objc_opt_respondsToSelector(Addr obj, Addr s) { return obj ? static_cast<int>(RT()->send(obj, "respondsToSelector:", {s}) & 0xff) : 0; }
void w_objc_enumerationMutation(Addr) { libsystem::detail::trap("objc: collection was mutated while being enumerated"); }

// property accessors the compiler calls for atomic / copy object properties: (self, _cmd, offset[, newValue, atomic, copy])
Addr w_objc_getProperty(Addr self, Addr, int64_t offset, int) {
    Runtime* rt = RT();
    if (!rt->readable(self + offset, 8)) return 0;
    const Addr v = *reinterpret_cast<Addr*>(self + offset);
    return v ? rt->autorelease(rt->retain(v)) : 0;
}
void set_property(Addr self, int64_t offset, Addr value, bool copy) {
    Runtime* rt = RT();
    if (!rt->readable(self + offset, 8)) return;
    Addr* slot = reinterpret_cast<Addr*>(self + offset);
    Addr nv = value;
    if (value) nv = copy ? rt->send(value, "copyWithZone:", {0}) : rt->retain(value);
    const Addr old = *slot;
    *slot = nv;
    rt->release(old);
}
void w_objc_setProperty(Addr self, Addr, int64_t offset, Addr value, int, int copy) { set_property(self, offset, value, copy != 0); }
void w_objc_setProperty_atomic(Addr self, Addr, Addr value, int64_t offset) { set_property(self, offset, value, false); }
void w_objc_setProperty_nonatomic(Addr self, Addr, Addr value, int64_t offset) { set_property(self, offset, value, false); }
void w_objc_setProperty_atomic_copy(Addr self, Addr, Addr value, int64_t offset) { set_property(self, offset, value, true); }
void w_objc_setProperty_nonatomic_copy(Addr self, Addr, Addr value, int64_t offset) { set_property(self, offset, value, true); }
Addr w_objc_destructInstance(Addr obj) { if (RT() && RT()->is_live_object(obj)) RT()->destruct_instance(obj); return obj; }
}  // namespace

void add_objc_memory_symbols(std::vector<libsystem::HostSymbol>& t) {
    using libsystem::HostSymbol;
    using libsystem::Lib;
    auto fn = [&](const char* name, void* f) { t.push_back(HostSymbol{name, f, false, false, false, Lib::Objc}); };
    fn("_objc_retain", addr(w_objc_retain));
    fn("_objc_release", addr(w_objc_release));
    fn("_objc_autorelease", addr(w_objc_autorelease));
    fn("_objc_retainAutorelease", addr(w_objc_retainAutorelease));
    fn("_objc_retainAutoreleaseReturnValue", addr(w_objc_retainAutoreleaseReturnValue));
    fn("_objc_autoreleaseReturnValue", addr(w_objc_autoreleaseReturnValue));
    fn("_objc_retainAutoreleasedReturnValue", addr(w_objc_retainAutoreleasedReturnValue));
    fn("_objc_unsafeClaimAutoreleasedReturnValue", addr(w_objc_unsafeClaimAutoreleasedReturnValue));
    fn("_objc_autoreleasePoolPush", addr(w_objc_autoreleasePoolPush));
    fn("_objc_autoreleasePoolPop", addr(w_objc_autoreleasePoolPop));
    fn("_objc_storeStrong", addr(w_objc_storeStrong));
    fn("_objc_storeWeak", addr(w_objc_storeWeak));
    fn("_objc_initWeak", addr(w_objc_initWeak));
    fn("_objc_destroyWeak", addr(w_objc_destroyWeak));
    fn("_objc_loadWeak", addr(w_objc_loadWeak));
    fn("_objc_loadWeakRetained", addr(w_objc_loadWeakRetained));
    fn("_objc_copyWeak", addr(w_objc_copyWeak));
    fn("_objc_moveWeak", addr(w_objc_moveWeak));
    fn("_objc_alloc", addr(w_objc_alloc));
    fn("_objc_allocWithZone", addr(w_objc_allocWithZone));
    fn("_objc_alloc_init", addr(w_objc_alloc_init));
    fn("_objc_opt_new", addr(w_objc_opt_new));
    fn("_objc_opt_class", addr(w_objc_opt_class));
    fn("_objc_opt_isKindOfClass", addr(w_objc_opt_isKindOfClass));
    fn("_objc_opt_respondsToSelector", addr(w_objc_opt_respondsToSelector));
    fn("_objc_enumerationMutation", addr(w_objc_enumerationMutation));
    fn("_objc_getProperty", addr(w_objc_getProperty));
    fn("_objc_setProperty", addr(w_objc_setProperty));
    fn("_objc_setProperty_atomic", addr(w_objc_setProperty_atomic));
    fn("_objc_setProperty_nonatomic", addr(w_objc_setProperty_nonatomic));
    fn("_objc_setProperty_atomic_copy", addr(w_objc_setProperty_atomic_copy));
    fn("_objc_setProperty_nonatomic_copy", addr(w_objc_setProperty_nonatomic_copy));
    fn("_objc_destructInstance", addr(w_objc_destructInstance));
}

}  // namespace cider::objc
