// TAPI text stub reader (orvyn::sdk): the v4 subset, multi-document files, multi-line lists, x86_64 filtering, limits and hostile input.
#include <random>

#include "orvyn/sdk.hpp"
#include "test_framework.hpp"

using namespace orvyn;

namespace {
const char* kSample = R"(--- !tapi-tbd
tbd-version:     4
targets:         [ x86_64-macos, arm64e-macos,
                   x86_64-maccatalyst ]
install-name:    '/System/Library/Frameworks/Foo.framework/Versions/A/Foo'
current-version: 1
reexported-libraries:
  - targets:         [ x86_64-macos ]
    libraries:       [ '/usr/lib/libbar.dylib' ]
exports:
  - targets:         [ arm64e-macos ]
    symbols:         [ _only_arm ]
  - targets:         [ x86_64-macos, arm64e-macos ]
    symbols:         [ _a, _b,
                       '$ld$weird', _c ]
    weak-symbols:    [ _w ]
    objc-classes:    [ NSFoo, NSBar ]
    objc-ivars:      [ NSFoo._x ]
...
--- !tapi-tbd
tbd-version:     4
targets:         [ arm64e-macos ]
install-name:    '/usr/lib/libarm.dylib'
exports:
  - targets:         [ arm64e-macos ]
    symbols:         [ _arm ]
...
)";
}

ORVYN_TEST(tbd_v4_documents_symbols_classes_and_targets_are_read) {
    auto r = sdk::parse_tbd(kSample);
    CHECK(r.error.empty());
    CHECK_EQ(r.libraries.size(), size_t{2});
    const auto& l = r.libraries[0];
    CHECK_EQ(l.install_name, std::string("/System/Library/Frameworks/Foo.framework/Versions/A/Foo"));
    CHECK(l.has_x86_64);
    CHECK_EQ(l.targets.size(), size_t{3});
    CHECK(l.symbols.count("_a") && l.symbols.count("_b") && l.symbols.count("$ld$weird") && l.symbols.count("_c"));
    CHECK(!l.symbols.count("_only_arm"));                       // entry without x86_64-macos is ignored
    CHECK(l.weak_symbols.count("_w"));
    CHECK(l.objc_classes.count("NSFoo") && l.objc_classes.count("NSBar"));
    CHECK(l.objc_ivars.count("NSFoo._x"));
    CHECK_EQ(l.reexported_libraries.size(), size_t{1});
    CHECK(!r.libraries[1].has_x86_64 && r.libraries[1].symbols.empty());
}

ORVYN_TEST(library_families_follow_install_names) {
    CHECK_EQ(sdk::library_family("/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit"), std::string("AppKit"));
    CHECK_EQ(sdk::library_family("/usr/lib/libobjc.A.dylib"), std::string("libobjc"));
    CHECK_EQ(sdk::library_family("/usr/lib/system/libdispatch.dylib"), std::string("libdispatch"));
}

ORVYN_TEST(malformed_tbd_input_is_an_error_not_a_crash) {
    CHECK(!sdk::parse_tbd("").error.empty());
    CHECK(!sdk::parse_tbd("not yaml at all\n").error.empty());
    CHECK(!sdk::parse_tbd("--- !tapi-tbd\ntargets: [ x86_64-macos\n").error.empty());     // unterminated list, no install name
    auto r = sdk::parse_tbd("--- !tapi-tbd\ninstall-name: 'x'\ntargets: [ x86_64-macos ]\nexports:\n  - targets: [ x86_64-macos ]\n    symbols: [ a, 'b\n...\n");
    (void)r;                                                    // must simply return
    std::string big(70u << 20, 'a');
    CHECK(!sdk::parse_tbd(big).error.empty());                  // over the input limit
}

ORVYN_TEST(random_mutations_of_a_tbd_never_crash) {
    std::mt19937 rng(7);
    std::string base = kSample;
    for (int i = 0; i < 20000; ++i) {
        std::string s = base;
        for (int k = 0; k < 1 + static_cast<int>(rng() % 6); ++k) {
            size_t at = rng() % s.size();
            switch (rng() % 3) { case 0: s[at] = static_cast<char>(rng()); break; case 1: s.erase(at, 1 + rng() % 8); break; default: s.insert(at, 1, "[]',:-# \n"[rng() % 9]); }
            if (s.empty()) s = "x";
        }
        (void)sdk::parse_tbd(s);
    }
}
