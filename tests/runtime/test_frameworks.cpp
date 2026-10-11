// ORVYN 0.6 framework registry: which Apple libraries are recognised, what the Cocoa umbrella resolves to, and the honest stubs of
// recognised-but-unimplemented frameworks. "Recognised" never means "implemented": these tests pin the difference.
#include <algorithm>
#include <set>

#include "orvyn/frameworks.hpp"
#include "orvyn/guest.hpp"
#include "orvyn/libsystem.hpp"
#include "orvyn/linker.hpp"
#include "guest_code.hpp"
#include "test_framework.hpp"

using namespace orvyn;
using namespace orvyn::libsystem;
using namespace orvyn::testing;
namespace fs = std::filesystem;

namespace {

const fs::path kObjc = fs::path(ORVYN_OBJC_FIXTURES_DIR);
fs::path bin(const std::string& n) { return kObjc / "bin" / n / n; }

struct Run {
    guest::Outcome r;
    std::string out, err;
};
Run run(const fs::path& exe, std::vector<std::string> args = {}) {
    Run run;
    guest::GuestConfig cfg;
    cfg.args = {path_to_utf8(exe)};
    cfg.args.insert(cfg.args.end(), args.begin(), args.end());
    cfg.image_path = exe;
    cfg.sink = [&](int fd, const char* d, size_t n) { (fd == 2 ? run.err : run.out).append(d, n); };
    auto b = read_file(exe, 32u << 20);
    if (!b.ok()) { report_failure(__FILE__, __LINE__, "fixture missing: " + path_to_utf8(exe)); throw AbortTest{}; }
    run.r = guest::run_in_process(ByteView(b.value()), cfg);
    return run;
}

}  // namespace

ORVYN_TEST(registry_recognises_exactly_the_documented_libraries) {
    std::set<std::string> names;
    for (const auto& f : known_frameworks()) {
        CHECK(names.insert(f.name).second);
        CHECK(find_framework(f.install_name) == &f);
        if (f.alternate_install_name[0]) CHECK(find_framework(f.alternate_install_name) == &f);
    }
    for (const char* n : {"libSystem", "libobjc", "Foundation", "CoreFoundation", "CoreGraphics", "AppKit", "Cocoa", "ApplicationServices", "Carbon", "IOKit", "CoreServices", "Security", "ServiceManagement"})
        CHECK(names.count(n) == 1);
    CHECK_EQ(names.size(), size_t{13});
    // not recognised: anything else, including look-alikes
    for (const char* n : {"/System/Library/Frameworks/CoreData.framework/Versions/A/CoreData", "/System/Library/Frameworks/SwiftUI.framework/Versions/A/SwiftUI",
                          "/usr/lib/libc++.1.dylib", "/usr/lib/swift/libswiftCore.dylib", "Cocoa", "/System/Library/Frameworks/Cocoa.framework/Versions/B/Cocoa", ""})
        CHECK(find_framework(n) == nullptr);
    CHECK(find_framework("/System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa")->status == FrameworkStatus::Umbrella);
    CHECK(find_framework("/System/Library/Frameworks/Foundation.framework/Foundation")->status == FrameworkStatus::Implemented);
    for (const char* n : {"ApplicationServices", "Carbon", "IOKit", "CoreServices", "Security", "ServiceManagement"})
        for (const auto& f : known_frameworks())
            if (std::string(f.name) == n) CHECK(f.status == FrameworkStatus::StubOnly);
}

ORVYN_TEST(re_export_visibility_follows_the_real_umbrellas) {
    CHECK(lib_visible(Lib::Cocoa, Lib::AppKit));
    CHECK(lib_visible(Lib::Cocoa, Lib::Foundation));
    CHECK(lib_visible(Lib::Cocoa, Lib::CoreFoundation));       // Foundation re-exports it
    CHECK(!lib_visible(Lib::Cocoa, Lib::System));              // Cocoa does not re-export libSystem
    CHECK(!lib_visible(Lib::Cocoa, Lib::IOKit));
    CHECK(lib_visible(Lib::ApplicationServices, Lib::CoreGraphics));
    CHECK(lib_visible(Lib::Carbon, Lib::ApplicationServices));     // Carbon -> ApplicationServices
    CHECK(lib_visible(Lib::Carbon, Lib::CoreGraphics));            // transitive: ... -> CoreGraphics
    CHECK(lib_visible(Lib::Carbon, Lib::CoreServices));
    CHECK(!lib_visible(Lib::ApplicationServices, Lib::Carbon));    // never upwards
    CHECK(!lib_visible(Lib::ApplicationServices, Lib::CoreServices));
    CHECK(!lib_visible(Lib::IOKit, Lib::Security));
    CHECK(!lib_visible(Lib::System, Lib::Foundation));
    CHECK(!lib_visible(Lib::Foundation, Lib::System));
    CHECK(lib_visible(Lib::AppKit, Lib::Foundation));          // unchanged behaviour of the four implemented subsets
}

ORVYN_TEST(stubs_are_never_counted_as_implemented_behaviour) {
    std::set<std::string> names;
    CHECK(!stub_symbols().empty());
    CHECK(stub_symbols().size() <= kMaxStubs);
    for (const auto& s : stub_symbols()) {
        CHECK(names.insert(s.name).second);
        CHECK(s.name[0] == '_');
        CHECK(std::string(s.signature).find(s.name + 1) != std::string::npos);      // the prototype names the function
        const FrameworkInfo* f = framework_of(s.lib);
        REQUIRE(f != nullptr);
        CHECK(f->status == FrameworkStatus::StubOnly);                                // only unimplemented frameworks have stubs
        CHECK(!is_implemented(s.name));                                               // the `implemented` registry never contains a stub
        for (const auto& h : implemented_symbols()) CHECK(std::string(h.name) != s.name);
    }
    // the stubs are served by the provider but flagged
    auto p = take(LibSystemProvider::create());
    for (const auto& s : stub_symbols()) {
        CHECK(p->is_stub(s.name));
        auto r = p->lookup(framework_of(s.lib)->install_name, s.name);
        REQUIRE(r.has_value());
        CHECK(r->stub);
        CHECK(r->is_function);
        CHECK(r->address != 0);
    }
    CHECK(!p->is_stub("_puts"));
    auto puts = p->lookup(kInstallName, "_puts");
    REQUIRE(puts.has_value());
    CHECK(!puts->stub);
}

ORVYN_TEST(the_provider_resolves_only_what_a_library_really_re_exports) {
    auto p = take(LibSystemProvider::create());
    const char* ApplicationServices = "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices";
    const char* Carbon = "/System/Library/Frameworks/Carbon.framework/Versions/A/Carbon";
    const char* IOKit = "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit";
    const char* Cocoa = "/System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa";
    CHECK(p->provides_library(Cocoa));
    CHECK(p->provides_library(ApplicationServices));
    CHECK(!p->provides_library("/System/Library/Frameworks/CoreData.framework/Versions/A/CoreData"));
    CHECK(p->lookup(ApplicationServices, "_AXIsProcessTrusted").has_value());
    CHECK(p->lookup(Carbon, "_AXIsProcessTrusted").has_value());                       // Carbon re-exports ApplicationServices
    CHECK(p->lookup(Carbon, "_UCKeyTranslate").has_value());                           // ... and CoreServices
    CHECK(!p->lookup(ApplicationServices, "_UCKeyTranslate").has_value());             // ApplicationServices does not
    CHECK(!p->lookup(IOKit, "_AXIsProcessTrusted").has_value());
    CHECK(!p->lookup(kInstallName, "_AXIsProcessTrusted").has_value());                // a stub is not a libSystem symbol
    // unknown functions and DATA symbols of recognised libraries are refused, never stubbed
    CHECK(!p->lookup(ApplicationServices, "_AXUIElementPostKeyboardEvent").has_value());
    CHECK(!p->lookup(Carbon, "_kTISPropertyLocalizedName").has_value());
    // the umbrella forwards to what is really implemented and nothing else
    auto nslog = p->lookup(Cocoa, "_NSLog");
    REQUIRE(nslog.has_value());
    CHECK(!nslog->stub);
    CHECK(!p->lookup(Cocoa, "_AXIsProcessTrusted").has_value());
    CHECK(!p->lookup(Cocoa, "_puts").has_value());
}

ORVYN_TEST(a_program_linked_against_the_cocoa_umbrella_runs) {
    Run r = run(bin("fw_cocoa"));
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 42);
    CHECK(r.err.find("cocoa result: 42") != std::string::npos);
}

ORVYN_TEST(stubs_let_an_image_load_and_start_but_stop_the_guest_when_called) {
    Run ok = run(bin("fw_stub"));                         // lazy imports are bound to stubs, never called
    REQUIRE(ok.r.state == guest::State::Exited);
    CHECK_EQ(ok.r.exit_code, 42);
    const struct { const char* arg; const char* fn; const char* framework; } calls[] = {
        {"a", "AXIsProcessTrusted", "ApplicationServices"}, {"k", "LMGetKbdType", "Carbon"}, {"t", "TISCopyCurrentKeyboardInputSource", "Carbon"}};
    for (auto& c : calls) {
        Run r = run(bin("fw_stub"), {c.arg});
        if (r.r.state != guest::State::Faulted) { report_failure(__FILE__, __LINE__, std::string(c.fn) + ": state " + guest::to_string(r.r.state)); continue; }
        CHECK(r.r.message.find(c.fn) != std::string::npos);
        CHECK(r.r.message.find(c.framework) != std::string::npos);
        CHECK(r.r.message.find("not implemented in ORVYN") != std::string::npos);
        CHECK(r.r.message.find("Public prototype") != std::string::npos);
        CHECK(r.out.empty());
    }
}

ORVYN_TEST(the_link_records_which_imports_are_stubs) {
    auto provider = take(LibSystemProvider::create());
    auto b = read_file(bin("fw_stub"), 1 << 20);
    REQUIRE(b.ok());
    linker::LinkOptions lo;
    lo.executable_path = bin("fw_stub");
    linker::LinkError err;
    auto prog = linker::Program::link(ByteView(b.value()), lo, *provider, err);
    REQUIRE(prog != nullptr);
    std::set<std::string> got;
    for (const auto& s : prog->stub_bindings()) {
        got.insert(s.symbol);
        CHECK(!s.framework.empty());
        CHECK(!s.signature.empty());
    }
    CHECK(got == (std::set<std::string>{"_AXIsProcessTrusted", "_LMGetKbdType", "_TISCopyCurrentKeyboardInputSource"}));
    // an image that imports nothing from those libraries records none
    auto exe = read_file(fs::path(ORVYN_EXEC_FIXTURES_DIR) / "bin" / "hello", 1 << 20);
    REQUIRE(exe.ok());
    lo.executable_path = fs::path(ORVYN_EXEC_FIXTURES_DIR) / "bin" / "hello";
    auto prog2 = linker::Program::link(ByteView(exe.value()), lo, *provider, err);
    REQUIRE(prog2 != nullptr);
    CHECK(prog2->stub_bindings().empty());
}

ORVYN_TEST(unknown_functions_and_data_of_unimplemented_frameworks_are_refused_at_load) {
    for (const char* name : {"fw_stub_unknown_function", "fw_stub_data"}) {
        Run r = run(bin(name));
        if (r.r.state != guest::State::LoadFailed) { report_failure(__FILE__, __LINE__, std::string(name) + ": state " + guest::to_string(r.r.state)); continue; }
        CHECK(r.r.message.find("unresolved symbol") != std::string::npos);
        CHECK(r.out.empty());
    }
    Run f = run(bin("fw_stub_unknown_function"));
    CHECK(f.r.message.find("AXUIElementPostKeyboardEvent") != std::string::npos);
    CHECK(f.r.message.find("ApplicationServices") != std::string::npos);
    Run d = run(bin("fw_stub_data"));
    CHECK(d.r.message.find("kTISPropertyLocalizedName") != std::string::npos);
}
