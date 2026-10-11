#pragma once
#include <cstdint>
#include <vector>

#include "orvyn/libsystem.hpp"

namespace orvyn::corefoundation {
void add_cf_symbols(std::vector<libsystem::HostSymbol>& table);   // _CFRetain, _CFString*, _CFArray*, _CFDictionary*, _CFRunLoop*...
void install(objc::Runtime& rt);                                     // run loop classes, constants (kCFTypeArrayCallBacks, kCFRunLoopDefaultMode...)
void rt_call(uint64_t fn, uint64_t arg);                             // calls a guest function with one argument through the Objective-C runtime
}  // namespace orvyn::corefoundation
