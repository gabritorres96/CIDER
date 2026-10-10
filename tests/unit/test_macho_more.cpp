#include <algorithm>

#include "cider/macho.hpp"
#include "macho_builder.hpp"
#include "test_framework.hpp"

using namespace cider;
using namespace cider::testing;

static macho::Image parse_ok(const std::vector<uint8_t>& b, bool symbols = false) {
    macho::ParseOptions o;
    o.parse_symbols = symbols;
    auto r = macho::parse_image(ByteView(b), o);
    if (!r.ok()) { report_failure(__FILE__, __LINE__, "parse_image failed: " + r.error().message); throw AbortTest{}; }
    return r.value();
}

CIDER_TEST(legacy_version_min_command) {
    MachoSpec s;
    s.build_version = false;
    s.legacy_version_min = true;
    auto img = parse_ok(build_macho(s).bytes);
    REQUIRE_EQ(img.build_versions.size(), size_t{1});
    CHECK(img.build_versions[0].legacy);
    CHECK_EQ(img.build_versions[0].platform, 1u);
}

CIDER_TEST(parse_uuid) {
    auto img = parse_ok(build_macho({}).bytes);
    REQUIRE(img.uuid.has_value());
    CHECK_EQ(*img.uuid, std::string("10111213-1415-1617-1819-1A1B1C1D1E1F"));
}

CIDER_TEST(parse_symbols_when_requested) {
    MachoSpec s;
    s.defined_symbols = {"_main", "_helper"};
    s.undefined_symbols = {"_puts", "_exit"};
    auto plain = parse_ok(build_macho(s).bytes, false);
    CHECK(plain.symbols.empty());
    REQUIRE(plain.symtab.has_value());
    CHECK_EQ(plain.symtab->nsyms, 4u);
    auto img = parse_ok(build_macho(s).bytes, true);
    REQUIRE_EQ(img.symbols.size(), size_t{4});
    CHECK_EQ(img.symbols[0].name, std::string("_main"));
    CHECK(!img.symbols[0].undefined);
    CHECK_EQ(img.symbols[0].value, kTextAddr + 0xf00);
    CHECK_EQ(img.symbols[2].name, std::string("_puts"));
    CHECK(img.symbols[2].undefined);
    CHECK_EQ(int{img.symbols[2].library_ordinal}, 1);
    REQUIRE(img.dysymtab.has_value());
    CHECK_EQ(img.dysymtab->nundefsym, 2u);
}

CIDER_TEST(parse_dyld_info) {
    auto img = parse_ok(build_macho({}).bytes);
    REQUIRE(img.dyld_info.has_value());
    CHECK(img.dyld_info->only);
    CHECK_EQ(img.dyld_info->exports.size, 8u);
    CHECK_EQ(img.dyld_info->exports.offset, 0x1000u);
}

CIDER_TEST(parse_code_signature) {
    MachoSpec s;
    s.code_signature = true;
    auto img = parse_ok(build_macho(s).bytes);
    REQUIRE(img.code_signature.has_value());
    const auto& c = *img.code_signature;
    CHECK(c.blob_parsed);
    CHECK(c.has_code_directory);
    CHECK(c.adhoc);
    CHECK(c.hardened_runtime);
    CHECK_EQ(c.identifier, std::string("com.example.fixture"));
    CHECK_EQ(c.team_id, std::string("TEAM123456"));
    CHECK_EQ(c.code_slots, 1u);
    CHECK(img.warnings.empty());
}

CIDER_TEST(garbage_code_signature_is_warning_not_error) {
    MachoSpec s;
    s.code_signature = true;
    auto b = build_macho(s);
    uint32_t off = static_cast<uint32_t>(b.bytes[b.cmd["code_signature"] + 8]) |
                   (static_cast<uint32_t>(b.bytes[b.cmd["code_signature"] + 9]) << 8);
    b.bytes[off] = 0x00;  // break SuperBlob magic
    auto img = parse_ok(b.bytes);
    REQUIRE(img.code_signature.has_value());
    CHECK(!img.code_signature->blob_parsed);
    CHECK(!img.warnings.empty());
}

CIDER_TEST(encrypted_image_detected) {
    MachoSpec s;
    s.encrypted = true;
    CHECK(parse_ok(build_macho(s).bytes).encrypted);
    CHECK(!parse_ok(build_macho({}).bytes).encrypted);
}

CIDER_TEST(arm64_image_parses) {
    MachoSpec s;
    s.cputype = macho::kCpuArm64;
    s.cpusubtype = 0;
    auto img = parse_ok(build_macho(s).bytes);
    CHECK_EQ(macho::cpu_name(img.cputype, img.cpusubtype), std::string("arm64"));
    CHECK_EQ(macho::cpu_name(macho::kCpuArm64, 2), std::string("arm64e"));
    CHECK_EQ(macho::cpu_name(macho::kCpuX86_64, 0x80000003), std::string("x86_64"));
}

CIDER_TEST(dylib_file_type) {
    MachoSpec s;
    s.filetype = 6;
    s.main = false;
    s.id_dylib = "@rpath/libx.dylib";
    auto img = parse_ok(build_macho(s).bytes);
    CHECK_EQ(std::string(macho::filetype_name(img.filetype)), std::string("dynamic library"));
    CHECK(!img.entry.has_value());
    CHECK(img.warnings.empty());
}

CIDER_TEST(parse_file_thin_has_one_slice) {
    auto b = build_macho({});
    auto f = macho::parse_file(ByteView(b.bytes));
    REQUIRE(f.ok());
    CHECK(!f.value().fat);
    REQUIRE_EQ(f.value().slices.size(), size_t{1});
    CHECK(f.value().slices[0].image.has_value());
    CHECK(f.value().find_slice(macho::kCpuX86_64) != nullptr);
    CHECK(f.value().find_slice(macho::kCpuArm64) == nullptr);
}

// N_UNDF symbols without N_EXT (e.g. N_PEXT, type 0x10) are undefined, like llvm-objdump/nm report them
// (CIDER 0.1 required N_EXT and classified them as defined).
CIDER_TEST(regression_non_external_undefined_symbol_is_undefined) {
    auto built = build_macho({});
    auto b = built.bytes;
    size_t puts_type = built.linkedit_off + 8 + 16 * 1 + 4;  // second nlist (_puts): n_type
    b[puts_type] = 0x10;                                      // N_UNDF | N_PEXT, not external
    auto img = parse_ok(b, true);
    REQUIRE_EQ(img.symbols.size(), size_t{2});
    CHECK_EQ(img.symbols[1].name, std::string("_puts"));
    CHECK(img.symbols[1].undefined);
    CHECK(!img.symbols[1].external);
    CHECK(!img.symbols[0].undefined);
}
