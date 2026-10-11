// Darwin pthreads on top of ORVYN's cooperative guest threads (guest_threads.hpp). pthread_t is an opaque number; pthread_mutex_t,
// pthread_cond_t, pthread_once_t, pthread_attr_t only keep a signature plus a small index in the guest's own storage, laid out like
// Darwin's (mutex 64 bytes, cond 48, attr 64, mutexattr/once 16): the real state lives in host tables. Nothing here is a Windows
// object, and Windows' own objects are never handed to the guest.
#include <chrono>
#include <cstring>

#include "orvyn/guest_threads.hpp"
#include "orvyn/libsystem.hpp"
#include "internal.hpp"

namespace orvyn::libsystem {
namespace {

using detail::kEINVAL;
constexpr int kEPERMv = 1, kESRCH = 3, kEDEADLKv = 11, kEBUSYv = 16, kETIMEDOUT = 60, kEAGAINv = 35, kENOMEMv = 12;

// Darwin signatures (<pthread/pthread.h> / <sys/_pthread/_pthread_types.h>)
constexpr uint64_t kMutexInit = 0x32AAABA7, kMutexReady = 0x4D555458, kCondInit = 0x3CB0B1BB, kCondReady = 0x434F4E44,
                   kOnceInit = 0x30B1BCBA, kAttrReady = 0x54484441, kMutexAttrReady = 0x4D415454, kDestroyed = 0xDEADDEAD;
constexpr size_t kMaxSyncObjects = 4096;

struct Raw { uint64_t sig; uint64_t id; };

GuestThreads* T() {
    RuntimeContext* c = context();
    return c ? c->threads.get() : nullptr;
}

// ---- mutex lookup / lazy initialisation -------------------------------------------------------------------------------------
MutexObject* mutex_of(GuestThreads& t, uint8_t* storage, bool create, int& err) {
    auto* raw = reinterpret_cast<Raw*>(storage);
    if (!storage) { err = kEINVAL; return nullptr; }
    if (raw->sig == kMutexInit && create) {                       // statically initialised: PTHREAD_MUTEX_INITIALIZER
        for (size_t i = 0; i < t.mutexes.size(); ++i)
            if (!t.mutexes[i].in_use) { t.mutexes[i] = MutexObject{true, 0, 0, 0}; raw->sig = kMutexReady; raw->id = i + 1; return &t.mutexes[i]; }
        if (t.mutexes.size() >= kMaxSyncObjects) { err = kENOMEMv; return nullptr; }
        t.mutexes.push_back(MutexObject{true, 0, 0, 0});
        raw->sig = kMutexReady;
        raw->id = t.mutexes.size();
        return &t.mutexes.back();
    }
    if (raw->sig != kMutexReady || raw->id == 0 || raw->id > t.mutexes.size() || !t.mutexes[raw->id - 1].in_use) { err = kEINVAL; return nullptr; }
    return &t.mutexes[raw->id - 1];
}

CondObject* cond_of(GuestThreads& t, uint8_t* storage, bool create, int& err) {
    auto* raw = reinterpret_cast<Raw*>(storage);
    if (!storage) { err = kEINVAL; return nullptr; }
    if (raw->sig == kCondInit && create) {
        for (size_t i = 0; i < t.conds.size(); ++i)
            if (!t.conds[i].in_use) { t.conds[i] = CondObject{true, 0, 0}; raw->sig = kCondReady; raw->id = i + 1; return &t.conds[i]; }
        if (t.conds.size() >= kMaxSyncObjects) { err = kENOMEMv; return nullptr; }
        t.conds.push_back(CondObject{true, 0, 0});
        raw->sig = kCondReady;
        raw->id = t.conds.size();
        return &t.conds.back();
    }
    if (raw->sig != kCondReady || raw->id == 0 || raw->id > t.conds.size() || !t.conds[raw->id - 1].in_use) { err = kEINVAL; return nullptr; }
    return &t.conds[raw->id - 1];
}

// ---- threads ------------------------------------------------------------------------------------------------------------------
int w_pthread_create(uint64_t* thread, const uint8_t* attr, uint64_t start, uint64_t arg) {
    GuestThreads* t = T();
    if (!t || !thread) return kEINVAL;
    if (!is_valid_callback(start)) return kEINVAL;
    size_t stack = 0;
    bool detached = false;
    if (attr) {
        uint64_t sig, ss, ds;
        std::memcpy(&sig, attr, 8); std::memcpy(&ss, attr + 8, 8); std::memcpy(&ds, attr + 16, 8);
        if (sig != kAttrReady) return kEINVAL;
        stack = static_cast<size_t>(ss);
        detached = ds == 2;                                        // PTHREAD_CREATE_DETACHED on Darwin is 2
    }
    uint64_t handle = 0;
    const int rc = t->create(start, arg, stack, handle);
    if (rc) return rc;
    if (detached) t->detach(handle);
    *thread = handle;
    return 0;
}
int w_pthread_join(uint64_t thread, uint64_t* retval) { GuestThreads* t = T(); return t ? t->join(thread, retval) : kEINVAL; }
int w_pthread_detach(uint64_t thread) { GuestThreads* t = T(); return t ? t->detach(thread) : kEINVAL; }
uint64_t w_pthread_self() { GuestThreads* t = T(); return t ? GuestThreads::handle_of(t->current_id()) : 0; }
int w_pthread_equal(uint64_t a, uint64_t b) { return a == b ? 1 : 0; }
void w_pthread_exit(uint64_t retval) {
    GuestThreads* t = T();
    if (!t) return;
    ThreadState* me = GuestThreads::current();
    if (me && me->is_main) { detail::trap("pthread_exit on the main thread is not supported"); return; }
    t->exit_thread(retval);
}
int w_sched_yield() { if (GuestThreads* t = T()) t->yield(); return 0; }

// attributes (stack size and detach state only)
int w_attr_init(uint8_t* attr) {
    if (!attr) return kEINVAL;
    const uint64_t sig = kAttrReady, ss = GuestThreads::kDefaultStack, ds = 1;   // PTHREAD_CREATE_JOINABLE = 1
    std::memset(attr, 0, 64);
    std::memcpy(attr, &sig, 8); std::memcpy(attr + 8, &ss, 8); std::memcpy(attr + 16, &ds, 8);
    return 0;
}
int w_attr_destroy(uint8_t* attr) { if (!attr) return kEINVAL; uint64_t d = kDestroyed; std::memcpy(attr, &d, 8); return 0; }
int w_attr_setstacksize(uint8_t* attr, uint64_t size) {
    uint64_t sig;
    if (!attr) return kEINVAL;
    std::memcpy(&sig, attr, 8);
    if (sig != kAttrReady || size < 16384) return kEINVAL;
    std::memcpy(attr + 8, &size, 8);
    return 0;
}
int w_attr_getstacksize(const uint8_t* attr, uint64_t* size) {
    uint64_t sig;
    if (!attr || !size) return kEINVAL;
    std::memcpy(&sig, attr, 8);
    if (sig != kAttrReady) return kEINVAL;
    std::memcpy(size, attr + 8, 8);
    return 0;
}
int w_attr_setdetachstate(uint8_t* attr, int state) {
    uint64_t sig;
    if (!attr || (state != 1 && state != 2)) return kEINVAL;
    std::memcpy(&sig, attr, 8);
    if (sig != kAttrReady) return kEINVAL;
    const uint64_t s = static_cast<uint64_t>(state);
    std::memcpy(attr + 16, &s, 8);
    return 0;
}

// ---- mutexes --------------------------------------------------------------------------------------------------------------------
int w_mutexattr_init(uint8_t* a) { if (!a) return kEINVAL; std::memset(a, 0, 16); const uint64_t s = kMutexAttrReady; std::memcpy(a, &s, 8); return 0; }
int w_mutexattr_destroy(uint8_t* a) { if (!a) return kEINVAL; const uint64_t d = kDestroyed; std::memcpy(a, &d, 8); return 0; }
int w_mutexattr_settype(uint8_t* a, int type) {
    uint64_t sig;
    if (!a || type < 0 || type > 2) return kEINVAL;                // NORMAL 0, ERRORCHECK 1, RECURSIVE 2 on Darwin
    std::memcpy(&sig, a, 8);
    if (sig != kMutexAttrReady) return kEINVAL;
    const uint64_t t = static_cast<uint64_t>(type);
    std::memcpy(a + 8, &t, 8);
    return 0;
}
int w_mutexattr_gettype(const uint8_t* a, int* type) {
    uint64_t sig, t;
    if (!a || !type) return kEINVAL;
    std::memcpy(&sig, a, 8);
    if (sig != kMutexAttrReady) return kEINVAL;
    std::memcpy(&t, a + 8, 8);
    *type = static_cast<int>(t);
    return 0;
}

int w_mutex_init(uint8_t* m, const uint8_t* attr) {
    GuestThreads* t = T();
    if (!t || !m) return kEINVAL;
    int type = 0;
    if (attr) { if (w_mutexattr_gettype(attr, &type)) return kEINVAL; }
    auto* raw = reinterpret_cast<Raw*>(m);
    raw->sig = kMutexInit;                                          // (re)initialise whatever was there
    int err = 0;
    MutexObject* o = mutex_of(*t, m, true, err);
    if (!o) return err;
    o->type = type;
    return 0;
}
int w_mutex_destroy(uint8_t* m) {
    GuestThreads* t = T();
    int err = 0;
    if (!t) return kEINVAL;
    MutexObject* o = mutex_of(*t, m, false, err);
    if (!o) return err;
    if (o->owner) return kEBUSYv;
    *o = MutexObject{};
    reinterpret_cast<Raw*>(m)->sig = kDestroyed;
    return 0;
}
int mutex_lock(GuestThreads& t, uint8_t* m, bool block) {
    int err = 0;
    MutexObject* o = mutex_of(t, m, true, err);
    if (!o) return err;
    const uint64_t me = t.current_id();
    auto* raw = reinterpret_cast<Raw*>(m);
    const uint64_t index = raw->id - 1;
    if (o->owner == me) {
        if (o->type == 2) { ++o->count; return 0; }                // recursive
        return block ? kEDEADLKv : kEBUSYv;                        // error-checking / default: relocking would deadlock forever
    }
    if (o->owner != 0) {
        if (!block) return kEBUSYv;
        if (!t.wait_until([&] { return t.mutexes[index].owner == 0; })) return 0;   // process exiting
    }
    o = &t.mutexes[index];                                         // the vector may have grown while we waited
    o->owner = me;
    o->count = 1;
    return 0;
}
int w_mutex_lock(uint8_t* m) { GuestThreads* t = T(); return t ? mutex_lock(*t, m, true) : kEINVAL; }
int w_mutex_trylock(uint8_t* m) { GuestThreads* t = T(); return t ? mutex_lock(*t, m, false) : kEINVAL; }
int w_mutex_unlock(uint8_t* m) {
    GuestThreads* t = T();
    int err = 0;
    if (!t) return kEINVAL;
    MutexObject* o = mutex_of(*t, m, false, err);
    if (!o) return err;
    if (o->owner != t->current_id()) return kEPERMv;
    if (--o->count > 0) return 0;
    o->owner = 0;
    o->count = 0;
    t->notify();
    return 0;
}

// ---- condition variables ---------------------------------------------------------------------------------------------------------
int w_cond_init(uint8_t* c, const void*) {
    GuestThreads* t = T();
    if (!t || !c) return kEINVAL;
    reinterpret_cast<Raw*>(c)->sig = kCondInit;
    int err = 0;
    return cond_of(*t, c, true, err) ? 0 : err;
}
int w_cond_destroy(uint8_t* c) {
    GuestThreads* t = T();
    int err = 0;
    if (!t) return kEINVAL;
    CondObject* o = cond_of(*t, c, false, err);
    if (!o) return err;
    if (o->waiters) return kEBUSYv;
    *o = CondObject{};
    reinterpret_cast<Raw*>(c)->sig = kDestroyed;
    return 0;
}
int cond_wait(uint8_t* c, uint8_t* m, int64_t timeout_ms) {
    GuestThreads* t = T();
    int err = 0;
    if (!t) return kEINVAL;
    CondObject* co = cond_of(*t, c, true, err);
    if (!co) return err;
    MutexObject* mo = mutex_of(*t, m, false, err);
    if (!mo) return err;
    if (mo->owner != t->current_id()) return kEPERMv;
    const uint64_t cidx = reinterpret_cast<Raw*>(c)->id - 1, midx = reinterpret_cast<Raw*>(m)->id - 1;
    const int saved = mo->count;
    ++co->waiters;
    mo->owner = 0;                                                  // release the mutex fully while waiting
    mo->count = 0;
    t->notify();
    const bool woken = t->wait_until([&] { return t->conds[cidx].wake > 0; }, timeout_ms);
    CondObject& after = t->conds[cidx];
    if (woken) --after.wake;
    --after.waiters;
    if (!t->wait_until([&] { return t->mutexes[midx].owner == 0; })) return 0;   // exiting
    t->mutexes[midx].owner = t->current_id();
    t->mutexes[midx].count = saved;
    return woken ? 0 : kETIMEDOUT;
}
int w_cond_wait(uint8_t* c, uint8_t* m) { return cond_wait(c, m, -1); }
int w_cond_timedwait(uint8_t* c, uint8_t* m, const int64_t* abstime) {   // struct timespec {sec, nsec}, CLOCK_REALTIME based
    if (!abstime) return kEINVAL;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t target = abstime[0] * 1000 + abstime[1] / 1000000;
    return cond_wait(c, m, target > now ? target - now : 0);
}
int w_cond_signal(uint8_t* c) {
    GuestThreads* t = T();
    int err = 0;
    if (!t) return kEINVAL;
    CondObject* o = cond_of(*t, c, true, err);
    if (!o) return err;
    if (o->waiters > o->wake) ++o->wake;
    t->notify();
    return 0;
}
int w_cond_broadcast(uint8_t* c) {
    GuestThreads* t = T();
    int err = 0;
    if (!t) return kEINVAL;
    CondObject* o = cond_of(*t, c, true, err);
    if (!o) return err;
    o->wake = o->waiters;
    t->notify();
    return 0;
}

// ---- pthread_once and thread-specific data ------------------------------------------------------------------------------------------
int w_pthread_once(uint8_t* once, uint64_t fn) {
    GuestThreads* t = T();
    if (!t || !once || !is_valid_callback(fn)) return kEINVAL;
    uint64_t sig;
    std::memcpy(&sig, once, 8);
    if (sig != kOnceInit) return kEINVAL;
    uint8_t& state = once[8];                                       // 0 not run, 1 running, 2 done
    if (state == 2) return 0;
    if (state == 1) { t->wait_until([&] { return once[8] == 2; }); return 0; }
    state = 1;
    abi::CallOutcome o = abi::call_guest_nested(fn, abi::GuestCallArgs{});
    if (o.refused) { detail::trap("pthread_once: init routine refused: " + o.refusal); return kEINVAL; }
    once[8] = 2;
    t->notify();
    return 0;
}

uint32_t g_next_key = 1;
int w_key_create(uint64_t* key, uint64_t /*destructor: never run, documented*/) {
    if (!key || g_next_key >= 1024) return kEAGAINv;
    *key = g_next_key++;
    return 0;
}
int w_key_delete(uint64_t) { return 0; }
uint64_t w_getspecific(uint64_t key) {
    ThreadState* ts = GuestThreads::current();
    if (!ts) return 0;
    auto it = ts->specific.find(static_cast<uint32_t>(key));
    return it == ts->specific.end() ? 0 : it->second;
}
int w_setspecific(uint64_t key, uint64_t value) {
    ThreadState* ts = GuestThreads::current();
    if (!ts || key == 0 || key >= g_next_key) return kEINVAL;
    ts->specific[static_cast<uint32_t>(key)] = value;
    return 0;
}

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

namespace detail {
void add_pthread_symbols(std::vector<HostSymbol>& t) {
    t.push_back({"_pthread_create", addr(w_pthread_create)});
    t.push_back({"_pthread_join", addr(w_pthread_join)});
    t.push_back({"_pthread_detach", addr(w_pthread_detach)});
    t.push_back({"_pthread_self", addr(w_pthread_self)});
    t.push_back({"_pthread_equal", addr(w_pthread_equal)});
    t.push_back({"_pthread_exit", addr(w_pthread_exit)});
    t.push_back({"_sched_yield", addr(w_sched_yield)});
    t.push_back({"_pthread_attr_init", addr(w_attr_init)});
    t.push_back({"_pthread_attr_destroy", addr(w_attr_destroy)});
    t.push_back({"_pthread_attr_setstacksize", addr(w_attr_setstacksize)});
    t.push_back({"_pthread_attr_getstacksize", addr(w_attr_getstacksize)});
    t.push_back({"_pthread_attr_setdetachstate", addr(w_attr_setdetachstate)});
    t.push_back({"_pthread_mutexattr_init", addr(w_mutexattr_init)});
    t.push_back({"_pthread_mutexattr_destroy", addr(w_mutexattr_destroy)});
    t.push_back({"_pthread_mutexattr_settype", addr(w_mutexattr_settype)});
    t.push_back({"_pthread_mutexattr_gettype", addr(w_mutexattr_gettype)});
    t.push_back({"_pthread_mutex_init", addr(w_mutex_init)});
    t.push_back({"_pthread_mutex_destroy", addr(w_mutex_destroy)});
    t.push_back({"_pthread_mutex_lock", addr(w_mutex_lock)});
    t.push_back({"_pthread_mutex_trylock", addr(w_mutex_trylock)});
    t.push_back({"_pthread_mutex_unlock", addr(w_mutex_unlock)});
    t.push_back({"_pthread_cond_init", addr(w_cond_init)});
    t.push_back({"_pthread_cond_destroy", addr(w_cond_destroy)});
    t.push_back({"_pthread_cond_wait", addr(w_cond_wait)});
    t.push_back({"_pthread_cond_timedwait", addr(w_cond_timedwait)});
    t.push_back({"_pthread_cond_signal", addr(w_cond_signal)});
    t.push_back({"_pthread_cond_broadcast", addr(w_cond_broadcast)});
    t.push_back({"_pthread_once", addr(w_pthread_once)});
    t.push_back({"_pthread_key_create", addr(w_key_create)});
    t.push_back({"_pthread_key_delete", addr(w_key_delete)});
    t.push_back({"_pthread_getspecific", addr(w_getspecific)});
    t.push_back({"_pthread_setspecific", addr(w_setspecific)});
}
}  // namespace detail

}  // namespace orvyn::libsystem
