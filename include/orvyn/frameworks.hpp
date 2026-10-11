#pragma once
// Which Apple libraries ORVYN recognises, and how far each one goes. "Recognised" is the weakest claim: a binary may name the
// library in LC_LOAD_DYLIB and the linker proceeds to symbol binding. It says nothing about what the library exports or runs.
//
//   Implemented  host-written subset (libSystem, libobjc, Foundation, CoreFoundation, CoreGraphics, AppKit): the symbols listed in
//                implemented_symbols() exist and run; every other import is refused at load ("unresolved symbol").
//   Umbrella     re-exports other recognised libraries and implements nothing itself (Cocoa). A symbol bound to it is looked up in
//                the libraries it re-exports.
//   StubOnly     recognised so that linking can continue, but NOT implemented. Only the symbols in stub_symbols() resolve (to a
//                thunk that stops the guest with a precise diagnostic when called); every other import is refused at load. Data
//                symbols can never be stubbed: a made-up value would be indistinguishable from a real one.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "orvyn/libsystem.hpp"

namespace orvyn::libsystem {

enum class FrameworkStatus : uint8_t { Implemented, Umbrella, StubOnly };
const char* to_string(FrameworkStatus s) noexcept;

struct FrameworkInfo {
    const char* name;                       // "Cocoa"
    const char* install_name;               // canonical install name, e.g. /System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa
    const char* alternate_install_name;     // the un-versioned path some binaries use, or ""
    FrameworkStatus status;
    Lib lib;                                // group tag used by the symbol tables
    std::vector<Lib> reexports;             // libraries whose symbols this one re-exports (transitively resolved by lib_visible)
    const char* note;
};

const std::vector<FrameworkInfo>& known_frameworks();
// nullptr when `install_name` (either spelling) is not a recognised library.
const FrameworkInfo* find_framework(std::string_view install_name);
const FrameworkInfo* framework_of(Lib lib);

// True when a symbol exported by `have` can be reached through a binding to `requested`: same library, a library `requested`
// re-exports (transitively), or - for the four implemented Foundation-family subsets - each other (they share one host runtime).
bool lib_visible(Lib requested, Lib have);

// A symbol that ORVYN can bind without implementing: a function of a StubOnly library, or - in libobjc / libSystem - one of the few
// exception-unwinding entry points (objc_begin_catch, _Unwind_Resume...) a program that merely CONTAINS @try/@catch imports. `signature` is the C prototype from Apple's public header
// (documentation and diagnostics only: the stub never reads its arguments and never returns).
struct StubSymbol {
    const char* name;        // Mach-O name, leading underscore
    Lib lib;                 // exporting library
    const char* signature;
    const char* explanation = "";   // when set, the diagnostic says this instead of the generic "not implemented" text
};
const std::vector<StubSymbol>& stub_symbols();
const StubSymbol* find_stub(std::string_view mach_o_symbol);
inline constexpr size_t kMaxStubs = 96;

}  // namespace orvyn::libsystem
