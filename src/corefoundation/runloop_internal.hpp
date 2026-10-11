#pragma once
// Private to src/corefoundation: run loop sources shared between runloop.cpp and cf_bridge.cpp.
#include <cstdint>
#include <vector>

namespace orvyn::runloop::cf {

struct Source {
    uint64_t obj = 0;               // the guest-visible CFRunLoopSourceRef (an Objective-C object)
    int64_t order = 0;
    bool valid = true, signaled = false, added = false;
    uint64_t info = 0, retain_fn = 0, release_fn = 0, schedule_fn = 0, cancel_fn = 0, perform_fn = 0;     // CFRunLoopSourceContext (version 0)
};

void perform_source(Source& s);          // calls the guest perform callback with `info` (cf_bridge.cpp)
std::vector<Source*>& added_sources();   // sources currently added to the run loop (runloop.cpp)

}  // namespace orvyn::runloop::cf
