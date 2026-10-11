// Framework registry and honest stub providers (see include/orvyn/frameworks.hpp for the contract).
//
// A stub is a callable symbol of a library ORVYN does not implement. It exists so that an image whose lazy bindings name such a function
// can still be linked and started: nothing is lost as long as the function is never called. The moment the guest calls it, the run ends
// with a diagnostic naming the library, the function and its public prototype. A stub never returns, never sets errno and never hands
// back a value, because any such value would be an invented result. Data symbols and functions that are not in the table below are not
// stubbed at all: the link fails with "unresolved symbol", exactly as for any other missing import.
#include <algorithm>
#include <array>
#include <utility>

#include "internal.hpp"
#include "orvyn/frameworks.hpp"
#include "orvyn/log.hpp"

namespace orvyn::libsystem {

const char* to_string(FrameworkStatus s) noexcept {
    switch (s) {
        case FrameworkStatus::Implemented: return "implemented subset";
        case FrameworkStatus::Umbrella: return "umbrella (re-exports only)";
        case FrameworkStatus::StubOnly: return "recognised, not implemented";
    }
    return "?";
}

namespace {
constexpr const char* kFw = "/System/Library/Frameworks/";
}

const std::vector<FrameworkInfo>& known_frameworks() {
    static const std::vector<FrameworkInfo> table = {
        {"libSystem", kInstallName, "", FrameworkStatus::Implemented, Lib::System, {}, "libc/POSIX/pthread/libdispatch/Blocks subset; see DARWIN-RUNTIME.md"},
        {"libobjc", kObjcInstallName, "", FrameworkStatus::Implemented, Lib::Objc, {}, "own Apple-ABI Objective-C runtime; no @try/@catch unwinding"},
        {"Foundation", kFoundationInstallName, "/System/Library/Frameworks/Foundation.framework/Foundation", FrameworkStatus::Implemented, Lib::Foundation, {}, "host-written subset"},
        {"CoreFoundation", kCoreFoundationInstallName, "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", FrameworkStatus::Implemented, Lib::CoreFoundation, {}, "host-written subset (served by the Foundation runtime)"},
        {"CoreGraphics", kCoreGraphicsInstallName, "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", FrameworkStatus::Implemented, Lib::CoreGraphics, {}, "drawing subset (contexts, paths, colours, transforms); no CGEvent, no images"},
        {"AppKit", kAppKitInstallName, "/System/Library/Frameworks/AppKit.framework/AppKit", FrameworkStatus::Implemented, Lib::AppKit, {}, "9 classes on native Win32 windows; no menus, text system or Auto Layout"},
        {"Cocoa", "/System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa", "/System/Library/Frameworks/Cocoa.framework/Cocoa", FrameworkStatus::Umbrella, Lib::Cocoa,
         {Lib::AppKit, Lib::Foundation}, "umbrella of AppKit, Foundation and CoreData; CoreData is not recognised, so its symbols are refused"},
        {"ApplicationServices", "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices", "/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices",
         FrameworkStatus::StubOnly, Lib::ApplicationServices, {Lib::CoreGraphics}, "re-exports CoreGraphics (the implemented subset only); Accessibility (AX*) symbols are stubs"},
        {"Carbon", "/System/Library/Frameworks/Carbon.framework/Versions/A/Carbon", "/System/Library/Frameworks/Carbon.framework/Carbon",
         FrameworkStatus::StubOnly, Lib::Carbon, {Lib::ApplicationServices, Lib::CoreServices}, "input sources (TIS*) and legacy low-memory accessors are stubs"},
        {"IOKit", "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit", "/System/Library/Frameworks/IOKit.framework/IOKit", FrameworkStatus::StubOnly, Lib::IOKit, {}, "no device registry exists on Windows"},
        {"CoreServices", "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices", "/System/Library/Frameworks/CoreServices.framework/CoreServices",
         FrameworkStatus::StubOnly, Lib::CoreServices, {}, "LaunchServices, CarbonCore (UCKeyTranslate...) are stubs"},
        {"Security", "/System/Library/Frameworks/Security.framework/Versions/A/Security", "/System/Library/Frameworks/Security.framework/Security", FrameworkStatus::StubOnly, Lib::Security, {}, "no keychain; nothing is stored or authenticated"},
        {"ServiceManagement", "/System/Library/Frameworks/ServiceManagement.framework/Versions/A/ServiceManagement", "/System/Library/Frameworks/ServiceManagement.framework/ServiceManagement",
         FrameworkStatus::StubOnly, Lib::ServiceManagement, {}, "no launchd on Windows"},
    };
    (void)kFw;
    return table;
}

const FrameworkInfo* find_framework(std::string_view install_name) {
    for (const auto& f : known_frameworks())
        if (install_name == f.install_name || (f.alternate_install_name[0] && install_name == f.alternate_install_name)) return &f;
    return nullptr;
}

const FrameworkInfo* framework_of(Lib lib) {
    for (const auto& f : known_frameworks())
        if (f.lib == lib) return &f;
    return nullptr;
}

bool lib_visible(Lib requested, Lib have) {
    if (requested == have) return true;
    auto family = [](Lib l) { return l == Lib::Foundation || l == Lib::CoreFoundation || l == Lib::CoreGraphics || l == Lib::AppKit; };
    if (family(requested) && family(have)) return true;               // the four implemented subsets share one host runtime and re-export each other
    // transitive re-exports (depth bounded by the table size)
    std::vector<Lib> todo = {requested}, seen;
    while (!todo.empty() && seen.size() < known_frameworks().size()) {
        const Lib cur = todo.back();
        todo.pop_back();
        if (std::find(seen.begin(), seen.end(), cur) != seen.end()) continue;
        seen.push_back(cur);
        const FrameworkInfo* f = framework_of(cur);
        if (!f) continue;
        for (Lib r : f->reexports) {
            if (r == have || (family(r) && family(have))) return true;
            todo.push_back(r);
        }
    }
    return false;
}

// ---- the stub table -------------------------------------------------------------------------------------------------------------
// Prototypes are copied from Apple's public headers (types as spelled there). Only FUNCTIONS are listed.
const std::vector<StubSymbol>& stub_symbols() {
    static const std::vector<StubSymbol> table = {
        // ApplicationServices (HIServices / Accessibility)
        {"_AXIsProcessTrusted", Lib::ApplicationServices, "Boolean AXIsProcessTrusted(void)"},
        {"_AXIsProcessTrustedWithOptions", Lib::ApplicationServices, "Boolean AXIsProcessTrustedWithOptions(CFDictionaryRef options)"},
        {"_AXUIElementCreateSystemWide", Lib::ApplicationServices, "AXUIElementRef AXUIElementCreateSystemWide(void)"},
        {"_AXUIElementCreateApplication", Lib::ApplicationServices, "AXUIElementRef AXUIElementCreateApplication(pid_t pid)"},
        {"_AXUIElementCopyAttributeValue", Lib::ApplicationServices, "AXError AXUIElementCopyAttributeValue(AXUIElementRef element, CFStringRef attribute, CFTypeRef *value)"},
        // Carbon (HIToolbox, Text Input Sources)
        {"_LMGetKbdType", Lib::Carbon, "UInt8 LMGetKbdType(void)"},
        {"_TISCopyCurrentKeyboardInputSource", Lib::Carbon, "TISInputSourceRef TISCopyCurrentKeyboardInputSource(void)"},
        {"_TISCopyCurrentKeyboardLayoutInputSource", Lib::Carbon, "TISInputSourceRef TISCopyCurrentKeyboardLayoutInputSource(void)"},
        {"_TISCopyCurrentASCIICapableKeyboardInputSource", Lib::Carbon, "TISInputSourceRef TISCopyCurrentASCIICapableKeyboardInputSource(void)"},
        {"_TISGetInputSourceProperty", Lib::Carbon, "void *TISGetInputSourceProperty(TISInputSourceRef inputSource, CFStringRef propertyKey)"},
        // CoreServices
        {"_UCKeyTranslate", Lib::CoreServices,
         "OSStatus UCKeyTranslate(const UCKeyboardLayout *keyLayoutPtr, UInt16 virtualKeyCode, UInt16 keyAction, UInt32 modifierKeyState, UInt32 keyboardType, OptionBits keyTranslateOptions, UInt32 *deadKeyState, UniCharCount maxStringLength, UniCharCount *actualStringLength, UniChar unicodeString[])"},
        {"_LSCopyDefaultApplicationURLForURL", Lib::CoreServices, "CFURLRef LSCopyDefaultApplicationURLForURL(CFURLRef inURL, LSRolesMask inRoleMask, CFErrorRef *outError)"},
        // IOKit
        {"_IOServiceMatching", Lib::IOKit, "CFMutableDictionaryRef IOServiceMatching(const char *name)"},
        {"_IOServiceGetMatchingService", Lib::IOKit, "io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching)"},
        {"_IORegistryEntryCreateCFProperty", Lib::IOKit, "CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key, CFAllocatorRef allocator, IOOptionBits options)"},
        {"_IOObjectRelease", Lib::IOKit, "kern_return_t IOObjectRelease(io_object_t object)"},
        {"_IOPSCopyPowerSourcesInfo", Lib::IOKit, "CFTypeRef IOPSCopyPowerSourcesInfo(void)"},
        {"_IOMainPort", Lib::IOKit, "kern_return_t IOMainPort(mach_port_t bootstrapPort, mach_port_t *mainPort)"},
        // Security
        {"_SecItemCopyMatching", Lib::Security, "OSStatus SecItemCopyMatching(CFDictionaryRef query, CFTypeRef *result)"},
        {"_SecItemAdd", Lib::Security, "OSStatus SecItemAdd(CFDictionaryRef attributes, CFTypeRef *result)"},
        {"_SecItemDelete", Lib::Security, "OSStatus SecItemDelete(CFDictionaryRef query)"},
        {"_SecRandomCopyBytes", Lib::Security, "int SecRandomCopyBytes(SecRandomRef rnd, size_t count, void *bytes)"},
        {"_SecCopyErrorMessageString", Lib::Security, "CFStringRef SecCopyErrorMessageString(OSStatus status, void *reserved)"},
        // ServiceManagement
        {"_SMLoginItemSetEnabled", Lib::ServiceManagement, "Boolean SMLoginItemSetEnabled(CFStringRef identifier, Boolean enabled)"},
        {"_SMCopyAllJobDictionaries", Lib::ServiceManagement, "CFArrayRef SMCopyAllJobDictionaries(CFStringRef domain)"},
        // Exception unwinding (libobjc / libunwind in libSystem). ORVYN has no unwinder: an image that contains @try/@catch/@finally LOADS, runs
        // normally while nothing is thrown, and stops with this diagnostic if control ever reaches a handler. objc_exception_throw itself is
        // implemented (it ends the program like an uncaught exception, see nsexception.cpp); nothing can reach the functions below without an unwind.
        {"_objc_begin_catch", Lib::Objc, "void *objc_begin_catch(void *exc_buf)",
         "Objective-C exception handling: a @catch handler was entered. ORVYN has no unwinder, so exceptions can be thrown (they end the program) but never caught"},
        {"_objc_end_catch", Lib::Objc, "void objc_end_catch(void)", "Objective-C exception handling: a @catch handler was left. ORVYN has no unwinder, so no handler can have run"},
        {"_objc_terminate", Lib::Objc, "void objc_terminate(void)", "objc_terminate was called (unwinding failed or crossed a noexcept frame): ORVYN has no unwinder, so this cannot have been a real unwind"},
        {"_objc_exception_rethrow", Lib::Objc, "void objc_exception_rethrow(void)", "Objective-C exception handling: a @catch handler rethrew. ORVYN has no unwinder"},
        {"___objc_personality_v0", Lib::Objc,
         "_Unwind_Reason_Code __objc_personality_v0(int version, _Unwind_Action actions, uint64_t exceptionClass, struct _Unwind_Exception *exceptionObject, struct _Unwind_Context *context)",
         "the Objective-C personality routine was called: only an unwinder calls it, and ORVYN has none"},
        {"__Unwind_Resume", Lib::System, "void _Unwind_Resume(struct _Unwind_Exception *exception_object)",
         "a cleanup (@finally / ARC / C++ destructor landing pad) tried to resume unwinding: ORVYN has no unwinder, so no landing pad can have run"},
    };
    return table;
}

const StubSymbol* find_stub(std::string_view s) {
    for (const auto& st : stub_symbols())
        if (s == st.name) return &st;
    return nullptr;
}

namespace {
// One host function per table slot: the thunk identifies the stub by its slot, because a raw thunk carries no symbol name.
template <size_t I>
void stub_entry(abi::GuestRegs*) {
    if (I >= stub_symbols().size()) { detail::trap("internal error: a stub thunk beyond the stub table was called"); return; }
    const StubSymbol& s = stub_symbols()[I];
    const FrameworkInfo* f = framework_of(s.lib);
    if (s.explanation[0]) {
        detail::trap(std::string(f ? f->name : "?") + ": " + s.explanation + " (" + (s.name + 1) + "; the program was stopped instead of continuing with a made-up result). Public prototype: " + s.signature);
        return;
    }
    detail::trap(std::string(f ? f->name : "?") + ": " + (s.name + 1) + " is not implemented in ORVYN, so the program was stopped instead of being given an invented result. Public prototype: " +
                 s.signature);
}

template <size_t... I>
constexpr std::array<void (*)(abi::GuestRegs*), sizeof...(I)> make_entries(std::index_sequence<I...>) {
    return {&stub_entry<I>...};
}
}  // namespace

namespace detail {
const std::vector<HostSymbol>& stub_host_symbols() {
    static const std::vector<HostSymbol> table = [] {
        static_assert(kMaxStubs >= 24, "kMaxStubs must cover the stub table");
        static constexpr auto entries = make_entries(std::make_index_sequence<kMaxStubs>{});
        std::vector<HostSymbol> t;
        const auto& st = stub_symbols();
        if (st.size() > kMaxStubs) {
            log::error("stub table has {} entries but only {} thunks exist: the extra ones are not registered", st.size(), kMaxStubs);
        }
        for (size_t i = 0; i < st.size() && i < kMaxStubs; ++i)
            t.push_back(HostSymbol{st[i].name, reinterpret_cast<void*>(entries[i]), true, false, false, st[i].lib});
        return t;
    }();
    return table;
}
}  // namespace detail

}  // namespace orvyn::libsystem
