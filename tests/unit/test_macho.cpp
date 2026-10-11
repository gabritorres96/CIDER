#include <algorithm>

#include "orvyn/macho.hpp"
#include "orvyn/util.hpp"
#include "macho_builder.hpp"
#include "test_framework.hpp"

using namespace orvyn;
using namespace orvyn::testing;

static macho::Image parse_ok(const std::vector<uint8_t>& b, bool symbols = false) {
    macho::ParseOptions o;
    o.parse_symbols = symbols;
    auto r = macho::parse_image(ByteView(b), o);
    if (!r.ok()) { report_failure(__FILE__, __LINE__, "parse_image failed: " + r.error().message); throw AbortTest{}; }
    return r.value();
}

ORVYN_TEST(identify_formats) {
    auto b = build_macho({});
    CHECK(macho::identify(ByteView(b.bytes)) == macho::Format::MachO64);
    std::vector<uint8_t> junk{1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(macho::identify(ByteView(junk)) == macho::Format::Unknown);
    CHECK(macho::identify(ByteView()) == macho::Format::Unknown);
    std::vector<uint8_t> m32{0xce, 0xfa, 0xed, 0xfe};
    CHECK(macho::identify(ByteView(m32)) == macho::Format::MachO32);
}

ORVYN_TEST(parse_executable_header) {
    auto img = parse_ok(build_macho({}).bytes);
    CHECK_EQ(img.cputype, macho::kCpuX86_64);
    CHECK_EQ(macho::cpu_name(img.cputype, img.cpusubtype), std::string("x86_64"));
    CHECK_EQ(img.filetype, macho::kFileExecute);
    CHECK((img.flags & macho::kFlagPie) != 0);
    CHECK_EQ(std::string(macho::filetype_name(img.filetype)), std::string("executable"));
    auto names = macho::flag_names(img.flags);
    CHECK(std::find(names.begin(), names.end(), "PIE") != names.end());
}

ORVYN_TEST(parse_segments_and_sections) {
    auto img = parse_ok(build_macho({}).bytes);
    REQUIRE_EQ(img.segments.size(), size_t{3});
    CHECK_EQ(img.segments[0].name, std::string("__PAGEZERO"));
    CHECK_EQ(img.segments[0].vmsize, uint64_t{0x100000000});
    const auto* text = img.find_segment("__TEXT");
    REQUIRE(text != nullptr);
    CHECK_EQ(text->vmaddr, kTextAddr);
    CHECK_EQ(text->initprot, 5);
    REQUIRE_EQ(text->sections.size(), size_t{1});
    CHECK_EQ(text->sections[0].name, std::string("__text"));
    CHECK_EQ(text->sections[0].size, uint64_t{6});
    CHECK(img.find_segment("__LINKEDIT") != nullptr);
}

ORVYN_TEST(parse_entry_point_lc_main) {
    auto img = parse_ok(build_macho({}).bytes);
    REQUIRE(img.entry.has_value());
    CHECK(img.entry->kind == macho::EntryPoint::Kind::Main);
    CHECK_EQ(img.entry->file_offset, uint64_t{0xf00});
    CHECK_EQ(img.entry->vmaddr, kTextAddr + 0xf00);
    CHECK_EQ(img.entry->segment, std::string("__TEXT"));
}

ORVYN_TEST(parse_entry_point_unixthread) {
    MachoSpec s;
    s.main = false;
    s.unixthread = true;
    auto img = parse_ok(build_macho(s).bytes);
    REQUIRE(img.entry.has_value());
    CHECK(img.entry->kind == macho::EntryPoint::Kind::UnixThread);
    CHECK_EQ(img.entry->vmaddr, kTextAddr + 0xf00);
}

ORVYN_TEST(no_entry_point_is_warning_for_executable) {
    MachoSpec s;
    s.main = false;
    auto img = parse_ok(build_macho(s).bytes);
    CHECK(!img.entry.has_value());
    CHECK(!img.warnings.empty());
}

ORVYN_TEST(parse_dependencies_all_kinds) {
    MachoSpec s;
    s.dylibs = {"/usr/lib/libSystem.B.dylib", "@rpath/libfoo.dylib"};
    s.weak_dylibs = {"@executable_path/../Frameworks/Weak.dylib"};
    s.reexport_dylibs = {"@loader_path/re.dylib"};
    s.rpaths = {"@executable_path/../Frameworks", "@loader_path/lib"};
    auto img = parse_ok(build_macho(s).bytes);
    REQUIRE_EQ(img.dylibs.size(), size_t{4});
    CHECK_EQ(img.dylibs[0].path, std::string("/usr/lib/libSystem.B.dylib"));
    CHECK(img.dylibs[0].kind == macho::DylibKind::Load);
    CHECK_EQ(img.dylibs[0].ordinal, 1u);
    CHECK(img.dylibs[2].kind == macho::DylibKind::Weak);
    CHECK(img.dylibs[3].kind == macho::DylibKind::Reexport);
    CHECK_EQ(img.dylibs[1].current_version, 0x00010000u);
    REQUIRE_EQ(img.rpaths.size(), size_t{2});
    CHECK_EQ(img.rpaths[1], std::string("@loader_path/lib"));
    CHECK_EQ(img.dylinker, std::string("/usr/lib/dyld"));
}

ORVYN_TEST(parse_dylib_id) {
    MachoSpec s;
    s.filetype = 6;
    s.main = false;
    s.id_dylib = "@rpath/libexample.dylib";
    auto img = parse_ok(build_macho(s).bytes);
    REQUIRE(img.id_dylib.has_value());
    CHECK_EQ(img.id_dylib->path, std::string("@rpath/libexample.dylib"));
    CHECK_EQ(img.id_dylib->ordinal, 0u);
    CHECK(img.dylibs.size() == 1);  // LC_ID_DYLIB is not a dependency
}

ORVYN_TEST(parse_build_version_and_platform) {
    auto img = parse_ok(build_macho({}).bytes);
    REQUIRE_EQ(img.build_versions.size(), size_t{1});
    CHECK_EQ(img.build_versions[0].platform, 1u);
    CHECK_EQ(std::string(macho::platform_name(1)), std::string("macOS"));
    CHECK_EQ(version_string(img.build_versions[0].minos), std::string("10.15"));
    CHECK_EQ(version_string(img.build_versions[0].sdk), std::string("12.0"));
}
