#pragma once
// The one run loop of a ORVYN process (CFRunLoop / NSRunLoop / NSApplication.run share it). See docs/EVENT-LOOP.md.
//
// One iteration: fire due timers, perform signaled sources, run the ready main-queue (libdispatch) blocks, let the GUI backend pump its
// window messages, then, if nothing happened, sleep (releasing the cooperative guest lock) until the next timer, a new main-queue block,
// a source signal, a window message or the timeout. Only the main guest thread runs the loop; other threads wake it with wake().
#include <cstdint>
#include <functional>
#include <string_view>

namespace orvyn::objc { class Runtime; }

namespace orvyn::runloop {

// Hooks installed by the GUI backend while it is active.
struct GuiHooks {
    std::function<int()> pump;                         // dispatches pending window messages without blocking; returns how many it handled
    std::function<void(void* wake_event, int64_t ms)> wait;   // blocks up to ms (-1: forever) until window input or `wake_event` is signaled
    std::function<bool()> keep_alive;                  // true while the application wants the loop to continue (windows exist / app running)
};

enum class Result { Finished = 1, Stopped = 2, TimedOut = 3, HandledSource = 4 };   // values of kCFRunLoopRun*

void install(objc::Runtime& rt);                       // classes for sources/timers, wake event, CF constants
void shutdown();
void wake();                                           // callable from any guest thread
void set_gui_hooks(GuiHooks hooks);
void clear_gui_hooks();
Result run(double timeout_seconds, bool return_after_source_handled);   // timeout < 0: no limit
void stop();                                           // makes the innermost run() return after the current iteration
bool running();

// Timers owned by the host (NSTimer, NSApplication): fire `callback` on the main thread after `delay_ms`, repeating every `interval_ms` (0: once).
// Returns an id for cancel_timer.
uint64_t add_host_timer(int64_t delay_ms, int64_t interval_ms, std::function<void()> callback);
void cancel_timer(uint64_t id);

}  // namespace orvyn::runloop
