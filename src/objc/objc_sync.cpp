// @synchronized (objc_sync_enter / objc_sync_exit) and associated objects (objc_setAssociatedObject & co.).
//
// Threads: guest threads are cooperative (guest_threads.hpp) - exactly one runs at a time and control only changes hands inside a blocking
// host call. So these tables need no lock of their own; the only waiting is a thread entering a lock another thread holds, which parks it
// in GuestThreads::wait_until until the owner leaves. This is NOT parallelism: ORVYN does not claim concurrent execution (see
// docs/THREADING.md).
#include <vector>

#include "orvyn/guest_threads.hpp"
#include "orvyn/libsystem.hpp"
#include "orvyn/log.hpp"
#include "orvyn/objc.hpp"
#include "../libsystem/internal.hpp"

namespace orvyn::objc {

namespace {
constexpr int kSyncSuccess = 0, kSyncNotOwning = -1;
constexpr size_t kMaxSyncLocks = 1u << 16, kMaxAssocPerObject = 4096, kMaxAssocObjects = 1u << 20;

// Association policies (objc/runtime.h): the setter part is the low bits, the getter part is 256 (retain on read) | 512 (autorelease on read).
constexpr uint32_t kAssign = 0, kRetainNonatomic = 1, kCopyNonatomic = 3, kRetain = 0x301, kCopy = 0x303;
bool valid_policy(uint32_t p) { return p == kAssign || p == kRetainNonatomic || p == kCopyNonatomic || p == kRetain || p == kCopy; }
bool owns_value(uint32_t p) { return (p & 3) != 0; }
}  // namespace

// ---- @synchronized ------------------------------------------------------------------------------------------------------------------
int Runtime::sync_enter(Addr obj) {
    if (!obj) return kSyncSuccess;                                   // @synchronized(nil) does nothing
    libsystem::GuestThreads* gt = ctx_.threads.get();
    const uint64_t me = gt ? gt->current_id() : 1;
    auto it = sync_.find(obj);
    if (it != sync_.end() && it->second.owner == me) { ++it->second.count; return kSyncSuccess; }       // recursive
    if (it != sync_.end() && it->second.count != 0 && gt) {
        // another thread holds it: wait until the lock record is gone or free (the process exiting also ends the wait)
        if (!gt->wait_until([&] { auto k = sync_.find(obj); return k == sync_.end() || k->second.count == 0; })) return kSyncSuccess;
    }
    if (sync_.size() >= kMaxSyncLocks && !sync_.count(obj)) {
        libsystem::detail::trap("@synchronized: more than 65536 objects locked at once");
        return kSyncSuccess;
    }
    SyncLock& l = sync_[obj];
    l.owner = me;
    l.count = 1;
    return kSyncSuccess;
}

int Runtime::sync_exit(Addr obj) {
    if (!obj) return kSyncSuccess;
    libsystem::GuestThreads* gt = ctx_.threads.get();
    const uint64_t me = gt ? gt->current_id() : 1;
    auto it = sync_.find(obj);
    if (it == sync_.end() || it->second.owner != me || it->second.count == 0) return kSyncNotOwning;
    if (--it->second.count == 0) {
        sync_.erase(it);                                              // the record lives only while the lock is held
        if (gt) gt->notify();
    }
    return kSyncSuccess;
}

// ---- associated objects ---------------------------------------------------------------------------------------------------------------
void Runtime::release_association(const Association& a) {
    if (owns_value(a.policy) && a.value) release(a.value);
}

size_t Runtime::associated_count() const {
    size_t n = 0;
    for (const auto& kv : assoc_) n += kv.second.size();
    return n;
}

bool Runtime::set_associated(Addr obj, Addr key, Addr value, uint32_t policy) {
    if (!obj || !valid_policy(policy)) return false;
    Association fresh;
    if (value) {
        fresh.policy = policy;
        switch (policy & 3) {
            case 0: fresh.value = value; break;                                         // ASSIGN: stored as is, never retained
            case 1: fresh.value = retain(value); break;
            default: fresh.value = static_cast<Addr>(send(value, "copy")); break;       // copy returns an owned (+1) object
        }
        if (fresh.value == 0 && (policy & 3) == 3) return true;                          // copy returned nil: nothing to associate
    }
    auto oit = assoc_.find(obj);
    Association old;
    bool had = false;
    if (oit != assoc_.end()) {
        auto kit = oit->second.find(key);
        if (kit != oit->second.end()) { old = kit->second; had = true; if (!value) oit->second.erase(kit); }
    }
    if (value) {
        if (oit == assoc_.end()) {
            if (assoc_.size() >= kMaxAssocObjects) { release_association(fresh); libsystem::detail::trap("objc_setAssociatedObject: too many objects with associations"); return false; }
            oit = assoc_.emplace(obj, std::unordered_map<Addr, Association>{}).first;
        }
        if (!had && oit->second.size() >= kMaxAssocPerObject) { release_association(fresh); libsystem::detail::trap("objc_setAssociatedObject: too many associations on one object"); return false; }
        oit->second[key] = fresh;
    } else if (oit != assoc_.end() && oit->second.empty()) {
        assoc_.erase(oit);
    }
    if (had) release_association(old);                                                    // after the new value is installed, like Apple's
    return true;
}

Addr Runtime::get_associated(Addr obj, Addr key) {
    auto oit = assoc_.find(obj);
    if (oit == assoc_.end()) return 0;
    auto kit = oit->second.find(key);
    if (kit == oit->second.end()) return 0;
    const Association a = kit->second;
    // atomic policies (0x300) hand back a value that survives a concurrent re-association: retain + autorelease
    if ((a.policy & 0x300) && a.value) autorelease(retain(a.value));
    return a.value;
}

void Runtime::remove_associated(Addr obj) {
    auto oit = assoc_.find(obj);
    if (oit == assoc_.end()) return;
    auto taken = std::move(oit->second);                                                   // release can re-enter (dealloc of the value)
    assoc_.erase(oit);
    for (const auto& kv : taken) release_association(kv.second);
}

namespace {
Runtime* RT() { return Runtime::current(); }

int w_objc_sync_enter(Addr o) { return RT() ? RT()->sync_enter(o) : 0; }
int w_objc_sync_exit(Addr o) { return RT() ? RT()->sync_exit(o) : 0; }

void w_objc_setAssociatedObject(Addr obj, Addr key, Addr value, uint64_t policy) {
    Runtime* rt = RT();
    if (!rt) return;
    if (!obj) return;                                           // associating with nil does nothing
    if (!rt->set_associated(obj, key, value, static_cast<uint32_t>(policy)) && !(policy == 0 || policy == 1 || policy == 3 || policy == 0x301 || policy == 0x303))
        libsystem::detail::trap("objc_setAssociatedObject: invalid association policy " + std::to_string(policy));
}
Addr w_objc_getAssociatedObject(Addr obj, Addr key) { return RT() && obj ? RT()->get_associated(obj, key) : 0; }
void w_objc_removeAssociatedObjects(Addr obj) { if (RT() && obj) RT()->remove_associated(obj); }

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }
}  // namespace

void add_objc_sync_symbols(std::vector<libsystem::HostSymbol>& t) {
    auto fn = [&](const char* name, void* f) { t.push_back(libsystem::HostSymbol{name, f, false, false, false, libsystem::Lib::Objc}); };
    fn("_objc_sync_enter", addr(w_objc_sync_enter));
    fn("_objc_sync_exit", addr(w_objc_sync_exit));
    fn("_objc_setAssociatedObject", addr(w_objc_setAssociatedObject));
    fn("_objc_getAssociatedObject", addr(w_objc_getAssociatedObject));
    fn("_objc_removeAssociatedObjects", addr(w_objc_removeAssociatedObjects));
}

}  // namespace orvyn::objc
