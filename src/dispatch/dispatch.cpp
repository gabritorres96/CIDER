// Grand Central Dispatch subset (CIDER 0.5.2).
//
// Model, in one paragraph: guest threads are cooperative (one runs at a time, switching only at blocking points, see THREADING.md). Queue
// state is therefore only touched while holding the guest lock and needs no mutex. The *main queue* is drained by the main guest thread
// only, from dispatch_main() or from the GUI run loop (dispatch::run_main_queue_once): blocks submitted to it never run on another
// thread, so GUI callbacks stay on the thread that owns the windows. Every other queue gets a host-implemented guest thread (a "worker",
// created on the first asynchronous submission) that runs its blocks one at a time in FIFO order. dispatch_sync runs the block on the
// *calling* thread once the queue is idle (as libdispatch does) and traps on the self-deadlocks libdispatch also rejects.
//
// Concurrent queues execute their blocks serially (a valid schedule); barriers are ordinary submissions; dispatch_apply runs inline.
// Not implemented: sources, I/O channels, data objects, target queues, dispatch_workloop, QoS overrides, dispatch_suspend/resume.
#include <algorithm>
#include <chrono>
#include <deque>
#include <format>
#include <memory>
#include <unordered_map>

#include "cider/dispatch.hpp"
#include "cider/log.hpp"
#include "cider/objc.hpp"
#include "../libsystem/internal.hpp"

namespace cider::dispatch {
namespace {

using objc::Addr;
using libsystem::GuestThreads;
using libsystem::RuntimeContext;
using libsystem::context;
using libsystem::detail::trap;

constexpr uint64_t kForever = ~0ull;

uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Group;
struct Item {
    Addr block = 0;                 // retained (copied) block, or 0
    Addr fn = 0, ctx = 0;           // dispatch_*_f form
    uint64_t due_ns = 0;            // 0: ready
    Addr group = 0;                 // group object to leave when the item completed
    bool* done = nullptr;           // dispatch_sync from another thread onto the main queue: completion flag of the waiting caller
    size_t apply_index = SIZE_MAX;  // dispatch_apply iteration (SIZE_MAX: none)
};
struct Queue {
    std::string label;
    bool main = false, global = false, concurrent = false;
    std::deque<Item> ready;
    std::vector<Item> timers;       // dispatch_after items, not yet due
    uint64_t owner = 0;             // guest thread id currently running a block of this queue
    bool worker = false;            // a worker thread exists (it holds one reference on the queue object)
    Addr obj = 0;
};
struct Group {
    int64_t count = 0;
    struct Notify { Addr queue; Item item; };
    std::vector<Notify> notifies;
};
struct Semaphore { int64_t value = 0; };
struct Obj {
    enum class Kind { Queue, Group, Semaphore } kind;
    Queue q;
    Group g;
    Semaphore s;
};

objc::Runtime* g_rt = nullptr;
void (*g_main_wake)() = nullptr;
std::unordered_map<Addr, std::unique_ptr<Obj>> g_objs;
Addr g_main_queue = 0, g_attr_concurrent = 0;
Addr g_global[4] = {};
objc::Class *g_cls_queue = nullptr, *g_cls_main = nullptr, *g_cls_global = nullptr, *g_cls_group = nullptr, *g_cls_sema = nullptr;

Obj* obj_of(Addr a) {
    auto it = g_objs.find(a);
    return it == g_objs.end() ? nullptr : it->second.get();
}
Queue* queue_of(Addr a) {
    Obj* o = obj_of(a);
    return o && o->kind == Obj::Kind::Queue ? &o->q : nullptr;
}
GuestThreads* threads() {
    RuntimeContext* c = context();
    return c ? c->threads.get() : nullptr;
}
uint64_t me() {
    GuestThreads* t = threads();
    return t ? t->current_id() : 1;
}
bool on_main_thread() { return me() == 1; }
bool dying() {
    GuestThreads* t = threads();
    RuntimeContext* c = context();
    return !t || t->exit_requested() || (c && !c->fault.empty());
}

Addr make_object(objc::Class* cls, Obj::Kind kind, bool immortal) {
    if (!g_rt || !cls) return 0;
    Addr a = immortal ? g_rt->arena_alloc(16, 16) : g_rt->create_instance(cls, 0);
    if (!a) return 0;
    if (immortal) { reinterpret_cast<Addr*>(a)[0] = cls->addr; reinterpret_cast<Addr*>(a)[1] = 0; }
    auto o = std::make_unique<Obj>();
    o->kind = kind;
    o->q.obj = a;
    g_objs[a] = std::move(o);
    return a;
}

// ---- running items -----------------------------------------------------------------------------------------------------------------
void leave_group(Addr group);

void run_item(Queue& q, Item it) {
    GuestThreads* th = threads();
    const uint64_t prev_owner = q.owner;
    q.owner = me();
    const Addr token = g_rt->pool_push();
    if (it.block) {
        const uint64_t invoke = g_rt->readable(it.block, 24) ? *reinterpret_cast<const uint64_t*>(it.block + 16) : 0;
        if (!libsystem::is_valid_callback(invoke)) trap("dispatch: the block's invoke function is not executable guest code");
        else if (it.apply_index != SIZE_MAX) g_rt->call_imp(invoke, {it.block, it.apply_index});
        else g_rt->call_imp(invoke, {it.block});
    } else if (it.fn) {
        if (!libsystem::is_valid_callback(it.fn)) trap("dispatch: the work function is not executable guest code");
        else g_rt->call_imp(it.fn, {it.ctx});
    }
    g_rt->pool_pop(token);
    if (it.block) libsystem::detail::block_release(it.block);
    q.owner = prev_owner;
    if (it.done) *it.done = true;
    if (it.group) leave_group(it.group);
    if (th) th->notify();
}

void promote_timers(Queue& q) {
    if (q.timers.empty()) return;
    const uint64_t now = now_ns();
    std::stable_sort(q.timers.begin(), q.timers.end(), [](const Item& a, const Item& b) { return a.due_ns < b.due_ns; });
    while (!q.timers.empty() && q.timers.front().due_ns <= now) {
        Item it = q.timers.front();
        q.timers.erase(q.timers.begin());
        it.due_ns = 0;
        q.ready.push_back(it);
    }
}
bool has_ready(Queue& q) {
    promote_timers(q);
    return !q.ready.empty();
}
int64_t ms_until_timer(Queue& q) {
    if (q.timers.empty()) return -1;
    uint64_t soonest = ~0ull;
    for (auto& t : q.timers) soonest = std::min(soonest, t.due_ns);
    const uint64_t now = now_ns();
    return soonest <= now ? 0 : static_cast<int64_t>((soonest - now) / 1'000'000 + 1);
}

void enqueue(Addr queue_obj, Item it) {
    Queue* q = queue_of(queue_obj);
    if (!q) { trap(std::format("dispatch: {:#x} is not a dispatch queue", queue_obj)); return; }
    if (queue_obj && !q->global && !q->main) g_rt->retain(queue_obj);          // a pending item keeps its queue alive
    if (it.due_ns) q->timers.push_back(it); else q->ready.push_back(it);
    if (GuestThreads* th = threads()) th->notify();
    if (q->main) { if (g_main_wake) g_main_wake(); return; }                                       // drained by the main thread
    if (!q->worker) {
        GuestThreads* th = threads();
        if (!th) { trap("dispatch: no guest thread scheduler"); return; }
        q->worker = true;
        g_rt->retain(queue_obj);                               // the worker's own reference (released when the worker stops)
        const Addr obj = queue_obj;
        uint64_t handle = 0;
        const int rc = th->create_native([obj] {
            GuestThreads* t = threads();
            for (;;) {
                Queue* wq = queue_of(obj);
                if (!wq) return;
                auto* st = g_rt->object_state(obj);
                const bool idle_and_orphaned = st && st->rc == 1 && wq->ready.empty() && wq->timers.empty() && wq->owner == 0 && !wq->global;
                if (idle_and_orphaned) {                       // only the worker references the queue: retire
                    wq->worker = false;
                    g_rt->release(obj);                        // deallocates the queue (on this thread)
                    return;
                }
                if (dying()) return;
                if (!has_ready(*wq) || wq->owner != 0) {
                    const int64_t timeout = ms_until_timer(*wq);
                    t->wait_until([&] {
                        Queue* x = queue_of(obj);
                        auto* s2 = g_rt->object_state(obj);
                        if (!x || dying()) return true;
                        if (s2 && s2->rc == 1 && x->ready.empty() && x->timers.empty() && x->owner == 0 && !x->global) return true;
                        return x->owner == 0 && has_ready(*x);
                    }, timeout >= 0 && timeout < 50 ? timeout : 50);
                    continue;
                }
                Item next = wq->ready.front();
                wq->ready.pop_front();
                run_item(*wq, next);
                Queue* after = queue_of(obj);
                if (after && !after->global && !after->main) g_rt->release(obj);       // the reference taken when the item was enqueued
            }
        }, handle);
        if (rc != 0) { q->worker = false; g_rt->release(queue_obj); trap("dispatch: cannot create a worker thread (thread limit)"); }
    }
}

// ---- groups ---------------------------------------------------------------------------------------------------------------------------
void leave_group(Addr group) {
    Obj* o = obj_of(group);
    if (!o || o->kind != Obj::Kind::Group) return;
    if (o->g.count <= 0) { trap("dispatch_group_leave: unbalanced call (the group's count would become negative)"); return; }
    if (--o->g.count == 0) {
        auto notifies = std::move(o->g.notifies);
        o->g.notifies.clear();
        for (auto& n : notifies) {
            enqueue(n.queue, n.item);
            g_rt->release(n.queue);
        }
    }
    if (GuestThreads* th = threads()) th->notify();
    g_rt->release(group);                                       // matches the retain taken by enter
}

// ---- guest-visible functions ------------------------------------------------------------------------------------------------------------
Item item_for_block(Addr block) {
    Item it;
    it.block = libsystem::detail::block_retain(block);          // GCD copies the block
    return it;
}
bool check_block(Addr block, const char* who) {
    if (!block || !g_rt->readable(block, 24)) { trap(std::format("{}: the block is NULL or unreadable", who)); return false; }
    return true;
}

void w_dispatch_async(Addr queue, Addr block) {
    if (!check_block(block, "dispatch_async")) return;
    enqueue(queue, item_for_block(block));
}
void w_dispatch_async_f(Addr queue, Addr ctx, Addr fn) {
    Item it;
    it.fn = fn;
    it.ctx = ctx;
    enqueue(queue, it);
}
void w_dispatch_barrier_async(Addr queue, Addr block) { w_dispatch_async(queue, block); }
void w_dispatch_barrier_async_f(Addr queue, Addr ctx, Addr fn) { w_dispatch_async_f(queue, ctx, fn); }

void sync_run(Addr queue_obj, Item it, bool is_block) {
    Queue* q = queue_of(queue_obj);
    GuestThreads* th = threads();
    if (!q || !th) { trap(std::format("dispatch_sync: {:#x} is not a dispatch queue", queue_obj)); return; }
    if (q->main) {
        if (on_main_thread()) { trap("dispatch_sync onto the main queue from the main thread would deadlock (BUG IN CLIENT)"); return; }
        bool done = false;
        it.done = &done;
        q->ready.push_back(it);
        th->notify();
        if (g_main_wake) g_main_wake();
        th->wait_until([&] { return done || dying(); });
        if (!done) { for (Item& pending : q->ready) if (pending.done == &done) pending.done = nullptr; }     // the caller leaves: do not write into its frame
        return;
    }
    if (q->owner == me()) { trap(std::format("dispatch_sync onto queue '{}' from a block already running on it would deadlock (BUG IN CLIENT)", q->label)); return; }
    g_rt->retain(queue_obj);
    th->wait_until([&] { Queue* x = queue_of(queue_obj); return !x || dying() || (x->owner == 0 && !has_ready(*x)); });
    if (!dying()) { if (Queue* x = queue_of(queue_obj)) run_item(*x, it); }
    else if (is_block && it.block) libsystem::detail::block_release(it.block);
    g_rt->release(queue_obj);
}
void w_dispatch_sync(Addr queue, Addr block) {
    if (!check_block(block, "dispatch_sync")) return;
    sync_run(queue, item_for_block(block), true);
}
void w_dispatch_sync_f(Addr queue, Addr ctx, Addr fn) {
    Item it;
    it.fn = fn;
    it.ctx = ctx;
    sync_run(queue, it, false);
}
void w_dispatch_barrier_sync(Addr queue, Addr block) { w_dispatch_sync(queue, block); }

uint64_t w_dispatch_time(uint64_t when, int64_t delta) {
    if (when == kForever) return kForever;
    const uint64_t base = when ? when : now_ns();
    const int64_t r = static_cast<int64_t>(base) + delta;
    return r <= 0 ? 1 : static_cast<uint64_t>(r);
}
void w_dispatch_after(uint64_t when, Addr queue, Addr block) {
    if (!check_block(block, "dispatch_after")) return;
    if (when == kForever) return;                              // never fires
    Item it = item_for_block(block);
    it.due_ns = when ? when : 1;
    enqueue(queue, it);
}
void w_dispatch_after_f(uint64_t when, Addr queue, Addr ctx, Addr fn) {
    if (when == kForever) return;
    Item it;
    it.fn = fn;
    it.ctx = ctx;
    it.due_ns = when ? when : 1;
    enqueue(queue, it);
}

// intptr_t dispatch_once_t: 0 = never run, 1 = running, ~0 = done (the header's inline fast path compares with ~0)
void run_once(Addr pred, Addr block, Addr fn, Addr ctx) {
    if (!g_rt->readable(pred, 8)) { trap("dispatch_once: the predicate is not writable memory"); return; }
    auto* p = reinterpret_cast<int64_t*>(pred);
    GuestThreads* th = threads();
    if (*p == -1) return;
    if (*p == 1) {
        if (th) th->wait_until([&] { return *p == -1 || dying(); });
        return;
    }
    *p = 1;
    if (block) {
        if (check_block(block, "dispatch_once")) {
            const uint64_t invoke = *reinterpret_cast<const uint64_t*>(block + 16);
            if (!libsystem::is_valid_callback(invoke)) trap("dispatch_once: the block's invoke function is not executable guest code");
            else g_rt->call_imp(invoke, {block});
        }
    } else if (fn) {
        if (!libsystem::is_valid_callback(fn)) trap("dispatch_once_f: the function is not executable guest code");
        else g_rt->call_imp(fn, {ctx});
    }
    *p = -1;
    if (th) th->notify();
}
void w_dispatch_once(Addr pred, Addr block) { run_once(pred, block, 0, 0); }
void w_dispatch_once_f(Addr pred, Addr ctx, Addr fn) { run_once(pred, 0, fn, ctx); }

void w_dispatch_main() {
    GuestThreads* th = threads();
    if (!th || !on_main_thread()) { trap("dispatch_main must be called on the main thread"); return; }
    Queue* mq = queue_of(g_main_queue);
    while (!dying()) {
        if (run_main_queue_once()) continue;
        th->wait_until([&] { return dying() || has_ready(*mq); }, std::min<int64_t>(ms_until_timer(*mq) < 0 ? 50 : ms_until_timer(*mq), 50));
    }
}

Addr w_dispatch_get_global_queue(int64_t identifier, uint64_t flags) {
    (void)flags;
    int slot = 1;                                               // default
    if (identifier == 2 || identifier == 0x21) slot = 0;       // HIGH / QOS_CLASS_USER_INTERACTIVE, USER_INITIATED(0x19) below
    else if (identifier == 0x19) slot = 0;
    else if (identifier == -2 || identifier == 0x11) slot = 2;  // LOW / UTILITY
    else if (identifier == -32768 || identifier == 0x09) slot = 3;   // BACKGROUND
    return g_global[slot];
}
Addr w_dispatch_queue_create(const char* label, Addr attr) {
    Addr a = make_object(g_cls_queue, Obj::Kind::Queue, false);
    if (!a) return 0;
    Queue& q = obj_of(a)->q;
    q.label = label ? label : "";
    q.concurrent = attr && attr == g_attr_concurrent;
    return a;
}
Addr w_dispatch_queue_attr_make_with_qos_class(Addr attr, int, int) { return attr; }
const char* w_dispatch_queue_get_label(Addr queue) {
    static std::string label;
    Queue* q = queue_of(queue);
    if (!queue) {                                               // DISPATCH_CURRENT_QUEUE_LABEL: the queue running this thread's block
        for (auto& kv : g_objs) if (kv.second->kind == Obj::Kind::Queue && kv.second->q.owner == me()) { label = kv.second->q.label; return label.c_str(); }
        return "";
    }
    if (!q) return "";
    label = q->label;
    return label.c_str();
}
void w_dispatch_retain(Addr o) { if (o) g_rt->retain(o); }
void w_dispatch_release(Addr o) { if (o) g_rt->release(o); }

void w_dispatch_apply(uint64_t n, Addr queue, Addr block) {
    if (!check_block(block, "dispatch_apply")) return;
    Queue* q = queue_of(queue);
    if (!q) { trap("dispatch_apply: not a dispatch queue"); return; }
    for (uint64_t i = 0; i < n && !dying(); ++i) {
        Item it = item_for_block(block);
        it.apply_index = i;
        Queue tmp;                                              // iterations run inline, in order, on the calling thread
        run_item(tmp, it);
    }
}

// ---- groups and semaphores ---------------------------------------------------------------------------------------------------------------
Addr w_dispatch_group_create() { return make_object(g_cls_group, Obj::Kind::Group, false); }
void w_dispatch_group_enter(Addr group) {
    Obj* o = obj_of(group);
    if (!o || o->kind != Obj::Kind::Group) { trap("dispatch_group_enter: not a dispatch group"); return; }
    ++o->g.count;
    g_rt->retain(group);
}
void w_dispatch_group_leave(Addr group) { leave_group(group); }
void w_dispatch_group_async(Addr group, Addr queue, Addr block) {
    Obj* o = obj_of(group);
    if (!o || o->kind != Obj::Kind::Group) { trap("dispatch_group_async: not a dispatch group"); return; }
    if (!check_block(block, "dispatch_group_async")) return;
    ++o->g.count;
    g_rt->retain(group);
    Item it = item_for_block(block);
    it.group = group;
    enqueue(queue, it);
}
void w_dispatch_group_notify(Addr group, Addr queue, Addr block) {
    Obj* o = obj_of(group);
    if (!o || o->kind != Obj::Kind::Group) { trap("dispatch_group_notify: not a dispatch group"); return; }
    if (!check_block(block, "dispatch_group_notify")) return;
    Item it = item_for_block(block);
    if (o->g.count == 0) { enqueue(queue, it); return; }
    g_rt->retain(queue);
    o->g.notifies.push_back({queue, it});
}
int64_t timeout_ms(uint64_t timeout) {
    if (timeout == kForever) return -1;
    const uint64_t now = now_ns();
    return timeout <= now ? 0 : static_cast<int64_t>((timeout - now) / 1'000'000 + 1);
}
int64_t w_dispatch_group_wait(Addr group, uint64_t timeout) {
    Obj* o = obj_of(group);
    GuestThreads* th = threads();
    if (!o || o->kind != Obj::Kind::Group || !th) { trap("dispatch_group_wait: not a dispatch group"); return 1; }
    const bool ok = th->wait_until([&] { Obj* x = obj_of(group); return !x || x->g.count == 0 || dying(); }, timeout_ms(timeout));
    return ok && obj_of(group) && obj_of(group)->g.count == 0 ? 0 : 1;
}
Addr w_dispatch_semaphore_create(int64_t value) {
    if (value < 0) return 0;
    Addr a = make_object(g_cls_sema, Obj::Kind::Semaphore, false);
    if (a) obj_of(a)->s.value = value;
    return a;
}
int64_t w_dispatch_semaphore_signal(Addr sem) {
    Obj* o = obj_of(sem);
    if (!o || o->kind != Obj::Kind::Semaphore) { trap("dispatch_semaphore_signal: not a semaphore"); return 0; }
    ++o->s.value;
    if (GuestThreads* th = threads()) th->notify();
    return 0;
}
int64_t w_dispatch_semaphore_wait(Addr sem, uint64_t timeout) {
    Obj* o = obj_of(sem);
    GuestThreads* th = threads();
    if (!o || o->kind != Obj::Kind::Semaphore || !th) { trap("dispatch_semaphore_wait: not a semaphore"); return 1; }
    if (o->s.value > 0) { --o->s.value; return 0; }
    const bool ok = th->wait_until([&] { Obj* x = obj_of(sem); return !x || x->s.value > 0 || dying(); }, timeout_ms(timeout));
    Obj* x = obj_of(sem);
    if (ok && x && x->s.value > 0) { --x->s.value; return 0; }
    return 1;                                                    // timed out
}

// ---- native classes -----------------------------------------------------------------------------------------------------------------------
uint64_t m_dealloc(Addr self, Addr) {
    auto it = g_objs.find(self);
    if (it != g_objs.end()) {
        std::unique_ptr<Obj> o = std::move(it->second);
        g_objs.erase(it);
        for (Item& i : o->q.ready) if (i.block) libsystem::detail::block_release(i.block);
        for (Item& i : o->q.timers) if (i.block) libsystem::detail::block_release(i.block);
        for (auto& n : o->g.notifies) if (n.item.block) libsystem::detail::block_release(n.item.block);
    }
    g_rt->destruct_instance(self);
    g_rt->dispose_instance(self);
    return 0;
}

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

// ---- public API -----------------------------------------------------------------------------------------------------------------------------
bool run_main_queue_once() {
    GuestThreads* th = threads();
    Queue* mq = queue_of(g_main_queue);
    if (!mq || !th || !on_main_thread() || mq->owner != 0 || !has_ready(*mq)) return false;
    Item it = mq->ready.front();
    mq->ready.pop_front();
    run_item(*mq, it);
    return true;
}
bool main_queue_pending() {
    Queue* mq = queue_of(g_main_queue);
    return mq && has_ready(*mq);
}
int64_t ms_until_next_main_timer() {
    Queue* mq = queue_of(g_main_queue);
    return mq ? ms_until_timer(*mq) : -1;
}

void install(objc::Runtime& rt) {
    g_rt = &rt;
    g_objs.clear();
    rt.set_native_owner("libSystem");
    objc::Class* base = rt.declare_native_class("OS_dispatch_object", "NSObject", 16);
    g_cls_queue = rt.declare_native_class("OS_dispatch_queue", "OS_dispatch_object", 16);
    g_cls_main = rt.declare_native_class("OS_dispatch_queue_main", "OS_dispatch_queue", 16);
    g_cls_global = rt.declare_native_class("OS_dispatch_queue_global", "OS_dispatch_queue", 16);
    g_cls_group = rt.declare_native_class("OS_dispatch_group", "OS_dispatch_object", 16);
    g_cls_sema = rt.declare_native_class("OS_dispatch_semaphore", "OS_dispatch_object", 16);
    if (!base || !g_cls_sema) return;
    rt.add_native_methods(base, false, {{"dealloc", "v16@0:8", addr(m_dealloc), false}});
    g_main_queue = make_object(g_cls_main, Obj::Kind::Queue, true);
    obj_of(g_main_queue)->q.main = true;
    obj_of(g_main_queue)->q.label = "com.apple.main-thread";
    static const char* names[4] = {"com.apple.root.user-interactive-qos", "com.apple.root.default-qos", "com.apple.root.utility-qos", "com.apple.root.background-qos"};
    for (int i = 0; i < 4; ++i) {
        g_global[i] = make_object(g_cls_global, Obj::Kind::Queue, true);
        Queue& q = obj_of(g_global[i])->q;
        q.global = true;
        q.concurrent = true;
        q.label = names[i];
    }
    g_attr_concurrent = rt.arena_alloc(16, 16);
}

std::optional<uint64_t> data_symbol(std::string_view name) {
    if (name == "__dispatch_main_q") return g_main_queue ? std::optional<uint64_t>(g_main_queue) : std::nullopt;
    if (name == "__dispatch_queue_attr_concurrent") return g_attr_concurrent ? std::optional<uint64_t>(g_attr_concurrent) : std::nullopt;
    return std::nullopt;
}

void set_main_wake(void (*wake)()) { g_main_wake = wake; }

void shutdown() {
    if (!g_rt) return;
    // release the references the workers still hold (they are stopped by now), then drop pending items
    std::vector<Addr> workers;
    for (auto& kv : g_objs)
        if (kv.second->kind == Obj::Kind::Queue && kv.second->q.worker && !kv.second->q.global) workers.push_back(kv.first);
    for (Addr a : workers) {
        if (Queue* q = queue_of(a)) {
            q->worker = false;
            for (Item& i : q->ready) { if (i.block) libsystem::detail::block_release(i.block); if (!q->global && !q->main) g_rt->release(a); }
            for (Item& i : q->timers) { if (i.block) libsystem::detail::block_release(i.block); if (!q->global && !q->main) g_rt->release(a); }
            q->ready.clear();
            q->timers.clear();
        }
        if (obj_of(a)) g_rt->release(a);
    }
    if (Queue* mq = queue_of(g_main_queue)) {
        for (Item& i : mq->ready) if (i.block) libsystem::detail::block_release(i.block);
        for (Item& i : mq->timers) if (i.block) libsystem::detail::block_release(i.block);
        mq->ready.clear();
        mq->timers.clear();
    }
}

}  // namespace cider::dispatch

namespace cider::libsystem::detail {
void add_dispatch_symbols(std::vector<HostSymbol>& t) {
    using namespace cider::dispatch;
    auto fn = [&](const char* name, void* f) { t.push_back(HostSymbol{name, f, false, false, false, Lib::System}); };
    fn("_dispatch_async", addr(w_dispatch_async));
    fn("_dispatch_async_f", addr(w_dispatch_async_f));
    fn("_dispatch_sync", addr(w_dispatch_sync));
    fn("_dispatch_sync_f", addr(w_dispatch_sync_f));
    fn("_dispatch_barrier_async", addr(w_dispatch_barrier_async));
    fn("_dispatch_barrier_async_f", addr(w_dispatch_barrier_async_f));
    fn("_dispatch_barrier_sync", addr(w_dispatch_barrier_sync));
    fn("_dispatch_after", addr(w_dispatch_after));
    fn("_dispatch_after_f", addr(w_dispatch_after_f));
    fn("_dispatch_time", addr(w_dispatch_time));
    fn("_dispatch_once", addr(w_dispatch_once));
    fn("_dispatch_once_f", addr(w_dispatch_once_f));
    fn("_dispatch_main", addr(w_dispatch_main));
    fn("_dispatch_get_global_queue", addr(w_dispatch_get_global_queue));
    fn("_dispatch_queue_create", addr(w_dispatch_queue_create));
    fn("_dispatch_queue_attr_make_with_qos_class", addr(w_dispatch_queue_attr_make_with_qos_class));
    fn("_dispatch_queue_get_label", addr(w_dispatch_queue_get_label));
    fn("_dispatch_retain", addr(w_dispatch_retain));
    fn("_dispatch_release", addr(w_dispatch_release));
    fn("_dispatch_apply", addr(w_dispatch_apply));
    fn("_dispatch_group_create", addr(w_dispatch_group_create));
    fn("_dispatch_group_enter", addr(w_dispatch_group_enter));
    fn("_dispatch_group_leave", addr(w_dispatch_group_leave));
    fn("_dispatch_group_async", addr(w_dispatch_group_async));
    fn("_dispatch_group_notify", addr(w_dispatch_group_notify));
    fn("_dispatch_group_wait", addr(w_dispatch_group_wait));
    fn("_dispatch_semaphore_create", addr(w_dispatch_semaphore_create));
    fn("_dispatch_semaphore_signal", addr(w_dispatch_semaphore_signal));
    fn("_dispatch_semaphore_wait", addr(w_dispatch_semaphore_wait));
}
}  // namespace cider::libsystem::detail
