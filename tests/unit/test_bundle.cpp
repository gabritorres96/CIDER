#include <algorithm>

#include "cider/analysis.hpp"
#include "cider/bundle.hpp"
#include "cider/dyld.hpp"
#include "cider/util.hpp"
#include "macho_builder.hpp"
#include "test_framework.hpp"

using namespace cider;
using namespace cider::testing;
namespace fs = std::filesystem;

CIDER_TEST(bundle_basic_metadata) {
    TempDir t;
    auto app = make_app(t, "Example.app", build_macho({}).bytes);
    CHECK(bundle::looks_like_bundle(app));
    auto r = bundle::inspect(app);
    REQUIRE(r.ok());
    auto& b = r.value();
    CHECK_EQ(b.executable_name, std::string("Example"));
    CHECK_EQ(b.identifier, std::string("com.example.app"));
    CHECK_EQ(b.name, std::string("Example"));
    CHECK_EQ(b.version, std::string("42"));
    CHECK_EQ(b.short_version, std::string("1.2.3"));
    CHECK_EQ(b.min_system_version, std::string("10.15"));
    CHECK(b.has_contents && b.has_info_plist && b.has_macos_dir && b.has_resources);
    CHECK(!b.has_frameworks);
    CHECK(b.executable_found);
    CHECK(b.issues.empty());
}

CIDER_TEST(bundle_binary_info_plist) {
    TempDir t;
    // the fixture writer only supports keys shorter than 15 characters
    t.write("B.app/Contents/Info.plist", binary_plist({{"CFBundleName", "Bin"}}));
    auto r = bundle::inspect(t.path() / "B.app");
    REQUIRE(r.ok());
    CHECK_EQ(r.value().name, std::string("Bin"));
    CHECK(r.value().executable_name.empty());
    CHECK(!r.value().issues.empty());  // CFBundleExecutable missing
}

CIDER_TEST(bundle_missing_pieces_reported) {
    TempDir t;
    fs::create_directories(t.path() / "Empty.app");
    auto r0 = bundle::inspect(t.path() / "Empty.app");
    REQUIRE(r0.ok());
    CHECK(!r0.value().has_contents);
    CHECK(!r0.value().issues.empty());

    fs::create_directories(t.path() / "NoPlist.app/Contents/MacOS");
    auto r1 = bundle::inspect(t.path() / "NoPlist.app");
    REQUIRE(r1.ok());
    CHECK(!r1.value().has_info_plist);

    t.write("NoExe.app/Contents/Info.plist", xml_plist({{"CFBundleExecutable", "Missing"}}));
    auto r2 = bundle::inspect(t.path() / "NoExe.app");
    REQUIRE(r2.ok());
    CHECK(!r2.value().executable_found);

    CHECK_ERR(bundle::inspect(t.path() / "does-not-exist.app"), ErrorCode::NotFound);
}

CIDER_TEST(bundle_rejects_path_traversal_in_executable_name) {
    TempDir t;
    t.write("Evil.app/Contents/Info.plist", xml_plist({{"CFBundleExecutable", "../../outside"}}));
    t.write("outside", std::string("x"));
    auto r = bundle::inspect(t.path() / "Evil.app");
    REQUIRE(r.ok());
    CHECK(!r.value().executable_found);
    CHECK(r.value().executable_path.empty());
    CHECK(!r.value().issues.empty());
    for (const char* bad : {"a/b", "a\\b", "..", ".", "C:evil"}) {
        TempDir t2;
        t2.write("E.app/Contents/Info.plist", xml_plist({{"CFBundleExecutable", bad}}));
        auto rr = bundle::inspect(t2.path() / "E.app");
        REQUIRE(rr.ok());
        CHECK(rr.value().executable_path.empty());
    }
}

CIDER_TEST(bundle_ios_requirements_flagged) {
    TempDir t;
    t.write("I.app/Contents/Info.plist",
            "<plist><dict><key>CFBundleExecutable</key><string>I</string><key>LSRequiresIPhoneOS</key><true/></dict></plist>");
    t.write("I.app/Contents/MacOS/I", build_macho({}).bytes);
    auto r = bundle::inspect(t.path() / "I.app");
    REQUIRE(r.ok());
    CHECK(std::any_of(r.value().issues.begin(), r.value().issues.end(), [](auto& s) { return s.find("iOS") != std::string::npos; }));
}

CIDER_TEST(bundle_frameworks_listed_with_architectures) {
    TempDir t;
    auto app = make_app(t, "F.app", build_macho({}).bytes);
    MachoSpec lib;
    lib.filetype = 6;
    lib.main = false;
    lib.id_dylib = "@rpath/Foo.framework/Versions/A/Foo";
    t.write("F.app/Contents/Frameworks/Foo.framework/Versions/A/Foo", build_macho(lib).bytes);
    t.write("F.app/Contents/Frameworks/libbar.dylib", build_macho(lib).bytes);
    t.write("F.app/Contents/Frameworks/notes.txt", std::string("ignored"));
    auto r = bundle::inspect(app);
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().frameworks.size(), size_t{2});
    CHECK_EQ(r.value().frameworks[0].name, std::string("Foo.framework"));
    CHECK(r.value().frameworks[0].binary.has_value());
    REQUIRE_EQ(r.value().frameworks[0].architectures.size(), size_t{1});
    CHECK_EQ(r.value().frameworks[0].architectures[0], std::string("x86_64"));
    CHECK_EQ(r.value().frameworks[1].name, std::string("libbar.dylib"));
}

// ---- dyld path expansion ---------------------------------------------------------------
CIDER_TEST(dyld_system_and_anchor_classification) {
    CHECK(dyld::is_system_path("/usr/lib/libSystem.B.dylib"));
    CHECK(dyld::is_system_path("/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation"));
    CHECK(!dyld::is_system_path("/usr/local/lib/libfoo.dylib"));
    CHECK(!dyld::is_system_path("@rpath/libusr/lib/x"));
    CHECK(dyld::anchor_of("@rpath/x") == dyld::Anchor::Rpath);
    CHECK(dyld::anchor_of("@executable_path/x") == dyld::Anchor::ExecutablePath);
    CHECK(dyld::anchor_of("@loader_path/x") == dyld::Anchor::LoaderPath);
    CHECK(dyld::anchor_of("@rpathology/x") == dyld::Anchor::None);
    CHECK(dyld::anchor_of("/abs") == dyld::Anchor::None);
}

CIDER_TEST(dyld_candidate_expansion) {
    dyld::PathContext c;
    c.executable_dir = fs::path("/app/Contents/MacOS");
    c.loader_dir = fs::path("/app/Contents/Frameworks/Foo.framework/Versions/A");
    c.rpaths = {"@executable_path/../Frameworks", "@loader_path/lib", "/opt/lib", "@rpath/bad"};
    auto a = dyld::candidates("@executable_path/../Resources/x.dylib", c);
    REQUIRE_EQ(a.size(), size_t{1});
    CHECK_EQ(path_to_utf8(a[0]), std::string(fs::path("/app/Contents/Resources/x.dylib").lexically_normal().generic_string()));
    auto l = dyld::candidates("@loader_path/y.dylib", c);
    REQUIRE_EQ(l.size(), size_t{1});
    CHECK(path_to_utf8(l[0]).find("Foo.framework/Versions/A/y.dylib") != std::string::npos);
    auto r = dyld::candidates("@rpath/libz.dylib", c);
    REQUIRE_EQ(r.size(), size_t{3});  // the nested "@rpath/bad" entry is invalid and skipped
    CHECK(path_to_utf8(r[0]).find("Contents/Frameworks/libz.dylib") != std::string::npos);
    CHECK(path_to_utf8(r[1]).find("Versions/A/lib/libz.dylib") != std::string::npos);
    CHECK(dyld::candidates("relative.dylib", c).empty());
    CHECK(dyld::candidates("@rpath/x", dyld::PathContext{}).empty());
}

// ---- dependency analysis ---------------------------------------------------------------
static bundle::Info must_inspect(const fs::path& p) {
    auto r = bundle::inspect(p);
    if (!r.ok()) throw AbortTest{};
    return r.value();
}

CIDER_TEST(dependencies_resolved_inside_bundle_transitively) {
    TempDir t;
    MachoSpec exe;
    exe.dylibs = {"/usr/lib/libSystem.B.dylib", "@rpath/libfoo.dylib", "@rpath/libmissing.dylib", "/usr/local/lib/libext.dylib"};
    exe.weak_dylibs = {"@rpath/libweak.dylib"};
    exe.rpaths = {"@executable_path/../Frameworks"};
    auto app = make_app(t, "D.app", build_macho(exe).bytes);
    MachoSpec foo;
    foo.filetype = 6;
    foo.main = false;
    foo.id_dylib = "@rpath/libfoo.dylib";
    foo.dylibs = {"/usr/lib/libSystem.B.dylib", "@loader_path/libbar.dylib"};
    t.write("D.app/Contents/Frameworks/libfoo.dylib", build_macho(foo).bytes);
    MachoSpec bar = foo;
    bar.id_dylib = "@loader_path/libbar.dylib";
    bar.dylibs = {"/usr/lib/libSystem.B.dylib"};
    t.write("D.app/Contents/Frameworks/libbar.dylib", build_macho(bar).bytes);

    auto info = must_inspect(app);
    auto f = macho::parse_file(ByteView(read_file(info.executable_path, 1 << 26).value()));
    REQUIRE(f.ok());
    // keep the buffer alive: parse_file copies everything it needs into owned structures
    const auto* slice = analysis::select_slice(f.value());
    REQUIRE(slice != nullptr);
    auto rep = analysis::analyze_dependencies(*slice->image, info.executable_path, info.root);

    auto find = [&](const std::string& name, bool direct) -> const analysis::Dependency* {
        for (auto& d : rep.deps)
            if (d.install_name == name && d.direct == direct) return &d;
        return nullptr;
    };
    auto* foo_dep = find("@rpath/libfoo.dylib", true);
    REQUIRE(foo_dep != nullptr);
    CHECK(foo_dep->cls == analysis::DepClass::Bundled);
    CHECK(foo_dep->resolved.find("Frameworks/libfoo.dylib") != std::string::npos);
    auto* bar_dep = find("@loader_path/libbar.dylib", false);  // found through libfoo (transitive)
    REQUIRE(bar_dep != nullptr);
    CHECK(bar_dep->cls == analysis::DepClass::Bundled);
    CHECK_EQ(bar_dep->required_by, std::string("libfoo.dylib"));
    CHECK(find("@rpath/libmissing.dylib", true)->cls == analysis::DepClass::Missing);
    CHECK(find("@rpath/libweak.dylib", true)->cls == analysis::DepClass::Missing);
    CHECK(find("/usr/local/lib/libext.dylib", true)->cls == analysis::DepClass::External);
    CHECK(find("/usr/lib/libSystem.B.dylib", true)->cls == analysis::DepClass::System);
    CHECK_EQ(rep.images_scanned, size_t{3});

    auto compat = analysis::assess(f.value(), &rep, &info);
    CHECK(compat.status == analysis::CompatStatus::PendingDependencies);
    CHECK(!compat.execution_confirmed);
}

CIDER_TEST(dependency_resolving_outside_bundle_is_flagged) {
    TempDir t;
    MachoSpec exe;
    exe.dylibs = {"@executable_path/../../../escape.dylib"};
    auto app = make_app(t, "O.app", build_macho(exe).bytes);
    MachoSpec lib;
    lib.filetype = 6; lib.main = false; lib.id_dylib = "x"; lib.dylibs = {};
    t.write("escape.dylib", build_macho(lib).bytes);  // sits next to O.app, outside it
    auto info = must_inspect(app);
    auto bytes = read_file(info.executable_path, 1 << 26).value();
    auto f = macho::parse_file(ByteView(bytes)).value();
    auto rep = analysis::analyze_dependencies(*f.slices[0].image, info.executable_path, info.root);
    REQUIRE_EQ(rep.deps.size(), size_t{1});
    CHECK(rep.deps[0].cls == analysis::DepClass::OutsideBundle);
}

CIDER_TEST(dependency_cycle_terminates) {
    TempDir t;
    MachoSpec exe;
    exe.dylibs = {"@executable_path/a.dylib"};
    auto app = make_app(t, "C.app", build_macho(exe).bytes);
    MachoSpec a;
    a.filetype = 6; a.main = false; a.id_dylib = "a";
    a.dylibs = {"@loader_path/b.dylib"};
    MachoSpec b = a;
    b.dylibs = {"@loader_path/a.dylib"};
    t.write("C.app/Contents/MacOS/a.dylib", build_macho(a).bytes);
    t.write("C.app/Contents/MacOS/b.dylib", build_macho(b).bytes);
    auto info = must_inspect(app);
    auto bytes = read_file(info.executable_path, 1 << 26).value();
    auto f = macho::parse_file(ByteView(bytes)).value();
    auto rep = analysis::analyze_dependencies(*f.slices[0].image, info.executable_path, info.root);
    CHECK_EQ(rep.images_scanned, size_t{3});
    CHECK_EQ(rep.deps.size(), size_t{3});
}

// ---- compatibility policy -----------------------------------------------------------------
static macho::File parsed(const std::vector<uint8_t>& b) {
    auto r = macho::parse_file(ByteView(b));
    if (!r.ok()) throw AbortTest{};
    return r.value();
}

CIDER_TEST(compat_analysis_compatible_never_claims_execution) {
    auto f = parsed(build_macho({}).bytes);
    auto c = analysis::assess(f, nullptr, nullptr);
    CHECK(c.status == analysis::CompatStatus::AnalysisCompatible);
    CHECK(!c.execution_confirmed);
    CHECK(c.status != analysis::CompatStatus::Executable);
}

CIDER_TEST(compat_arm64_only_is_not_compatible) {
    MachoSpec s;
    s.cputype = macho::kCpuArm64;
    s.cpusubtype = 0;
    auto c = analysis::assess(parsed(build_macho(s).bytes), nullptr, nullptr);
    CHECK(c.status == analysis::CompatStatus::NotCompatible);
}

CIDER_TEST(compat_ios_and_encrypted_are_not_compatible) {
    MachoSpec s;
    s.platform = 2;
    CHECK(analysis::assess(parsed(build_macho(s).bytes), nullptr, nullptr).status == analysis::CompatStatus::NotCompatible);
    MachoSpec e;
    e.encrypted = true;
    CHECK(analysis::assess(parsed(build_macho(e).bytes), nullptr, nullptr).status == analysis::CompatStatus::NotCompatible);
}

CIDER_TEST(compat_fat_with_x86_64_slice_is_analysable) {
    MachoSpec a;
    a.cputype = macho::kCpuArm64;
    a.cpusubtype = 0;
    auto fat = build_fat({{macho::kCpuX86_64, 3, build_macho({}).bytes}, {macho::kCpuArm64, 0, build_macho(a).bytes}});
    CHECK(analysis::assess(parsed(fat), nullptr, nullptr).status == analysis::CompatStatus::AnalysisCompatible);
}
