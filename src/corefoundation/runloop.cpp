// The process run loop (see include/orvyn/runloop.hpp and docs/EVENT-LOOP.md).
#include "orvyn/runloop.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "orvyn/dispatch.hpp"
#include "orvyn/libsystem.hpp"
#include "orvyn/log.hpp"
#include "orvyn/objc.hpp"
#include "../libsystem/internal.hpp"
#include "runloop_internal.hpp"

namespace orvyn::runloop {
namespace {

using libsystem::GuestThreads;

struct HostTimer {
    uint64_t id = 0;
    int64_t due_ns = 0;
    int64_t interval_ns = 0;
    std::function<void()> fire;
    bool alive = true;
};

std::vector<HostTimer> g_timers;
std::vector<cf::Source*> g_sources;                 // sources added to the run loop
GuiHooks g_hooks;
bool g_has_hooks = false;
HANDLE g_wake = nullptr;
int g_depth = 0;                                     // nested run() calls
int g_stop_requests = 0;
uint64_t g_next_timer_id = 1;
objc::Runtime* g_rt = nullptr;

int64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
GuestThreads* threads() {
    libsystem::RuntimeContext* c = libsystem::context();
    return c ? c->threads.get() : nullptr;
}
bool dying() {
    GuestThreads* t = threads();
    libsystem::RuntimeContext* c = libsystem::context();
    return !t || t->exit_requested() || (c && !c->fault.empty());
}

bool fire_due_timers() {
    bool any = false;
    const int64_t now = now_ns();
    // callbacks may add or cancel timers: iterate over ids
    std::vector<uint64_t> due;
    for (auto& t : g_timers) if (t.alive && t.due_ns <= now) due.push_back(t.id);
    for (uint64_t id : due) {
        auto it = std::find_if(g_timers.begin(), g_timers.end(), [&](const HostTimer& t) { return t.id == id; });
        if (it == g_timers.end() || !it->alive) continue;
        std::function<void()> fn = it->fire;
        if (it->interval_ns > 0) {
            it->due_ns = std::max(it->due_ns + it->interval_ns, now);           // repeating: skip missed ticks, like CFRunLoopTimer
        } else {
            it->alive = false;
        }
        any = true;
        fn();
        if (dying()) break;
    }
    g_timers.erase(std::remove_if(g_timers.begin(), g_timers.end(), [](const HostTimer& t) { return !t.alive; }), g_timers.end());
    return any;
}

bool perform_sources() {
    bool any = false;
    std::vector<cf::Source*> list = g_sources;
    std::stable_sort(list.begin(), list.end(), [](cf::Source* a, cf::Source* b) { return a->order < b->order; });
    for (cf::Source* s : list) {
        if (!s->valid || !s->signaled) continue;
        s->signaled = false;
        any = true;
        cf::perform_source(*s);
        if (dying()) break;
    }
    return any;
}

int64_t ms_until_next_timer() {
    int64_t soonest = -1;
    const int64_t now = now_ns();
    for (auto& t : g_timers) {
        if (!t.alive) continue;
        const int64_t ms = t.due_ns <= now ? 0 : (t.due_ns - now) / 1'000'000 + 1;
        if (soonest < 0 || ms < soonest) soonest = ms;
    }
    const int64_t d = dispatch::ms_until_next_main_timer();
    if (d >= 0 && (soonest < 0 || d < soonest)) soonest = d;
    return soonest;
}

void wait_for_work(int64_t ms) {
    auto sleep_fn = [&] {
        if (g_has_hooks && g_hooks.wait) g_hooks.wait(g_wake, ms);
        else WaitForSingleObject(g_wake, ms < 0 ? INFINITE : static_cast<DWORD>(ms));
    };
    GuestThreads* t = threads();
    if (t) t->blocking(sleep_fn);                       // other guest threads run meanwhile; an exit request unwinds us afterwards
    else sleep_fn();
}

}  // namespace

namespace cf {
std::vector<Source*>& added_sources() { return g_sources; }
}  // namespace cf

void install(objc::Runtime& rt) {
    g_rt = &rt;
    if (!g_wake) g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_timers.clear();
    g_sources.clear();
    g_depth = g_stop_requests = 0;
    g_has_hooks = false;
    g_hooks = GuiHooks{};
    dispatch::set_main_wake(&wake);
}
void shutdown() {
    g_timers.clear();
    g_sources.clear();
    clear_gui_hooks();
    dispatch::set_main_wake(nullptr);
    if (g_wake) { CloseHandle(g_wake); g_wake = nullptr; }
}
void wake() {
    if (g_wake) SetEvent(g_wake);
}
void set_gui_hooks(GuiHooks hooks) {
    g_hooks = std::move(hooks);
    g_has_hooks = true;
}
void clear_gui_hooks() {
    g_hooks = GuiHooks{};
    g_has_hooks = false;
}
bool running() { return g_depth > 0; }
void stop() {
    if (g_depth > 0) ++g_stop_requests;
    wake();
}

uint64_t add_host_timer(int64_t delay_ms, int64_t interval_ms, std::function<void()> callback) {
    HostTimer t;
    t.id = g_next_timer_id++;
    t.due_ns = now_ns() + std::max<int64_t>(delay_ms, 0) * 1'000'000;
    t.interval_ns = std::max<int64_t>(interval_ms, 0) * 1'000'000;
    t.fire = std::move(callback);
    g_timers.push_back(std::move(t));
    wake();
    return g_timers.back().id;
}
void cancel_timer(uint64_t id) {
    for (auto& t : g_timers) if (t.id == id) t.alive = false;
}

Result run(double timeout_seconds, bool return_after_source_handled) {
    libsystem::RuntimeContext* c = libsystem::context();
    GuestThreads* th = threads();
    if (!c || !th || th->current_id() != 1) { libsystem::detail::trap("the run loop can only run on the main thread"); return Result::Finished; }
    ++g_depth;
    const int64_t deadline = timeout_seconds < 0 ? -1 : now_ns() + static_cast<int64_t>(timeout_seconds * 1e9);
    struct Leave { ~Leave() { --g_depth; } } leave;
    const int stops_at_entry = g_stop_requests;
    for (;;) {
        if (dying()) return Result::Stopped;
        if (g_stop_requests > stops_at_entry) { g_stop_requests = stops_at_entry; return Result::Stopped; }
        bool handled = false, source_handled = false;
        handled |= fire_due_timers();
        if (dying()) return Result::Stopped;
        source_handled = perform_sources();
        handled |= source_handled;
        while (dispatch::run_main_queue_once()) { handled = true; if (dying() || g_stop_requests > stops_at_entry) break; }
        if (g_has_hooks && g_hooks.pump) handled |= g_hooks.pump() > 0;
        if (dying()) return Result::Stopped;
        if (g_stop_requests > stops_at_entry) { g_stop_requests = stops_at_entry; return Result::Stopped; }
        if (source_handled && return_after_source_handled) return Result::HandledSource;

        // nothing left that could ever wake us: CFRunLoopRun returns at once when there are no sources, timers or input
        const bool alive = !g_timers.empty() || !g_sources.empty() || dispatch::main_queue_pending() || dispatch::ms_until_next_main_timer() >= 0 ||
                           (g_has_hooks && g_hooks.keep_alive && g_hooks.keep_alive()) || deadline >= 0;
        if (!alive) return Result::Finished;
        if (handled) continue;                                   // look again before sleeping: handlers may have queued more work

        int64_t wait_ms = ms_until_next_timer();
        if (deadline >= 0) {
            const int64_t left = deadline <= now_ns() ? 0 : (deadline - now_ns()) / 1'000'000 + 1;
            if (wait_ms < 0 || left < wait_ms) wait_ms = left;
            if (left == 0) return Result::TimedOut;
        }
        if (wait_ms < 0 || wait_ms > 20) wait_ms = 20;           // short slices: exit requests and other-thread notifications are noticed promptly
        wait_for_work(wait_ms);
    }
}

}  // namespace orvyn::runloop
