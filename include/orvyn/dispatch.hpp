#pragma once
// Grand Central Dispatch subset (see docs/LIBDISPATCH.md). The main queue is drained by the main guest thread only.
#include <cstdint>
#include <optional>
#include <string_view>

namespace orvyn::objc { class Runtime; }

namespace orvyn::dispatch {

void install(objc::Runtime& rt);                          // declares the OS_dispatch_* classes and creates the main/global queues
std::optional<uint64_t> data_symbol(std::string_view mach_o_name);   // __dispatch_main_q, __dispatch_queue_attr_concurrent
void shutdown();                                          // after the guest threads stopped: drops pending work and worker references

// For run loops (NSApplication.run, CFRunLoop, dispatch_main): only valid on the main guest thread.
bool run_main_queue_once();                               // runs one ready main-queue block; false when none
bool main_queue_pending();                                // a block is ready now
int64_t ms_until_next_main_timer();                       // dispatch_after on the main queue: ms until due, -1 when none

void set_main_wake(void (*wake)());                       // called (from any guest thread) when work is added to the main queue

}  // namespace orvyn::dispatch
