// Strict-validation matrix: every malformed input must be rejected with a specific error and
// must never crash. Each case doubles as a regression test for the validation it exercises.
#include "orvyn/macho.hpp"
#include "macho_builder.hpp"
#include "test_framework.hpp"

using namespace orvyn;
using namespace orvyn::testing;

static Result<macho::Image> parse(const std::vector<uint8_t>& b, bool symbols = true) {
    macho::ParseOptions o;
    o.parse_symbols = symbols;
    return macho::parse_image(ByteView(b), o);
}

ORVYN_TEST(empty_and_tiny_inputs) {
    CHECK_ERR(parse({}), ErrorCode::BadMagic);
    CHECK_ERR(parse({0xcf}), ErrorCode::BadMagic);
    CHECK_ERR(parse({0xcf, 0xfa, 0xed, 0xfe}), ErrorCode::Truncated);
    std::vector<uint8_t> hdr(31, 0);
    hdr[0] = 0xcf; hdr[1] = 0xfa; hdr[2] = 0xed; hdr[3] = 0xfe;
    CHECK_ERR(parse(hdr), ErrorCode::Truncated);
}

ORVYN_TEST(bad_magic_and_unsupported_formats) {
    std::vector<uint8_t> junk(64, 0x41);
    CHECK_ERR(parse(junk), ErrorCode::BadMagic);
    std::vector<uint8_t> m32{0xce, 0xfa, 0xed, 0xfe, 7, 0, 0, 0};
    m32.resize(64);
    CHECK_ERR(parse(m32), ErrorCode::Unsupported);
    std::vector<uint8_t> be{0xfe, 0xed, 0xfa, 0xcf};
    be.resize(64);
    CHECK_ERR(parse(be), ErrorCode::Unsupported);
}

ORVYN_TEST(unknown_architecture) {
    auto b = build_macho({}).bytes;
    patch32(b, 4, 0x01000099);
    CHECK_ERR(parse(b), ErrorCode::Unsupported);
    patch32(b, 4, 7);  // 32-bit cputype in a 64-bit header
    CHECK_ERR(parse(b), ErrorCode::Unsupported);
}

ORVYN_TEST(invalid_filetype) {
    auto b = build_macho({}).bytes;
    patch32(b, 12, 0);
    CHECK_ERR(parse(b), ErrorCode::Malformed);
    patch32(b, 12, 99);
    CHECK_ERR(parse(b), ErrorCode::Malformed);
}

ORVYN_TEST(sizeofcmds_past_end_of_file) {
    auto b = build_macho({}).bytes;
    patch32(b, 20, static_cast<uint32_t>(b.size()));
    CHECK_ERR(parse(b), ErrorCode::Truncated);
}

ORVYN_TEST(sizeofcmds_and_ncmds_limits) {
    auto b = build_macho({}).bytes;
    patch32(b, 20, 0xffffffff);
    CHECK_ERR(parse(b), ErrorCode::LimitExceeded);
    b = build_macho({}).bytes;
    patch32(b, 16, 0xffffffff);
    CHECK_ERR(parse(b), ErrorCode::LimitExceeded);
}

ORVYN_TEST(ncmds_cannot_fit_in_sizeofcmds) {
    auto b = build_macho({}).bytes;
    patch32(b, 16, 5000);
    CHECK_ERR(parse(b), ErrorCode::Malformed);
}

ORVYN_TEST(command_size_too_small_or_unaligned) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch32(b, built.cmd["uuid"] + 4, 4);
    CHECK_ERR(parse(b), ErrorCode::Malformed);
    b = built.bytes;
    patch32(b, built.cmd["uuid"] + 4, 20);
    CHECK_ERR(parse(b), ErrorCode::Malformed);
}

ORVYN_TEST(command_size_exceeds_sizeofcmds) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch32(b, built.cmd["uuid"] + 4, 0x10000);
    CHECK_ERR(parse(b), ErrorCode::OutOfBounds);
}

ORVYN_TEST(ncmds_larger_than_actual_commands) {
    auto b = build_macho({}).bytes;
    uint32_t n = static_cast<uint32_t>(b[16]) | (static_cast<uint32_t>(b[17]) << 8);
    patch32(b, 16, n + 1);
    CHECK(!parse(b).ok());
}

ORVYN_TEST(segment_file_range_out_of_bounds) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch64(b, built.cmd["linkedit"] + 48, 0x100000);
    patch64(b, built.cmd["linkedit"] + 32, 0x200000);
    CHECK_ERR(parse(b), ErrorCode::OutOfBounds);
}

ORVYN_TEST(segment_integer_overflow) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch64(b, built.cmd["text"] + 40, 0xfffffffffffff000ull);
    patch64(b, built.cmd["text"] + 48, 0x2000);
    CHECK_ERR(parse(b), ErrorCode::Overflow);
    b = built.bytes;
    patch64(b, built.cmd["text"] + 24, 0xfffffffffffff000ull);
    patch64(b, built.cmd["text"] + 32, 0x2000);
    CHECK_ERR(parse(b), ErrorCode::Overflow);
}

ORVYN_TEST(segment_filesize_larger_than_vmsize) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch64(b, built.cmd["text"] + 32, 0x800);
    CHECK_ERR(parse(b), ErrorCode::Malformed);
}

ORVYN_TEST(segment_section_count_exceeds_cmdsize) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch32(b, built.cmd["text"] + 64, 100000);
    CHECK_ERR(parse(b), ErrorCode::Malformed);
    b = built.bytes;
    patch32(b, built.cmd["text"] + 64, 0xffffffff);
    CHECK_ERR(parse(b), ErrorCode::Malformed);
}

ORVYN_TEST(section_outside_segment) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch64(b, built.cmd["text"] + 72 + 32, 0x200000000ull);
    CHECK_ERR(parse(b), ErrorCode::OutOfBounds);
    b = built.bytes;
    patch32(b, built.cmd["text"] + 72 + 48, 0xffffff00u);
    CHECK_ERR(parse(b), ErrorCode::OutOfBounds);
}

ORVYN_TEST(overlapping_segments_virtual_memory) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch64(b, built.cmd["linkedit"] + 24, kTextAddr + 0x800);
    CHECK_ERR(parse(b), ErrorCode::Overlap);
}

ORVYN_TEST(overlapping_segments_in_file) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch64(b, built.cmd["linkedit"] + 40, 0x800);
    CHECK_ERR(parse(b), ErrorCode::Overlap);
}

ORVYN_TEST(dylib_name_offset_invalid) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch32(b, built.cmd["dylib0"] + 8, 4);
    CHECK_ERR(parse(b), ErrorCode::Malformed);
    b = built.bytes;
    patch32(b, built.cmd["dylib0"] + 8, 0x10000);
    CHECK_ERR(parse(b), ErrorCode::Malformed);
}

ORVYN_TEST(unterminated_install_name) {
    auto built = build_macho({});
    auto b = built.bytes;
    uint32_t size = b[built.cmd["dylib0"] + 4];
    for (uint32_t i = 24; i < size; ++i) b[built.cmd["dylib0"] + i] = 'A';
    CHECK_ERR(parse(b), ErrorCode::Malformed);
}

ORVYN_TEST(unterminated_rpath) {
    MachoSpec s;
    s.rpaths = {"@loader_path/x"};
    auto built = build_macho(s);
    auto b = built.bytes;
    uint32_t size = b[built.cmd["rpath0"] + 4];
    for (uint32_t i = 12; i < size; ++i) b[built.cmd["rpath0"] + i] = 'A';
    CHECK_ERR(parse(b), ErrorCode::Malformed);
}

ORVYN_TEST(symtab_out_of_bounds) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch32(b, built.cmd["symtab"] + 12, 0x7fffffff);
    CHECK_ERR(parse(b), ErrorCode::OutOfBounds);
    b = built.bytes;
    patch32(b, built.cmd["symtab"] + 8, 0xfffffff0u);
    CHECK_ERR(parse(b), ErrorCode::OutOfBounds);
    b = built.bytes;
    patch32(b, built.cmd["symtab"] + 20, 0x7fffffff);
    CHECK_ERR(parse(b), ErrorCode::OutOfBounds);
}

ORVYN_TEST(symbol_name_index_out_of_string_table) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch32(b, built.linkedit_off + 8, 0xfffff);
    CHECK_ERR(parse(b), ErrorCode::Malformed);
    CHECK(parse(b, false).ok());
}

ORVYN_TEST(symbol_count_limit) {
    auto built = build_macho({});
    macho::ParseOptions o;
    o.parse_symbols = true;
    o.limits.max_symbols = 1;
    CHECK_ERR(macho::parse_image(ByteView(built.bytes), o), ErrorCode::LimitExceeded);
}

ORVYN_TEST(dysymtab_inconsistent_with_symtab) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch32(b, built.cmd["dysymtab"] + 28, 1000);
    CHECK_ERR(parse(b), ErrorCode::OutOfBounds);
}

ORVYN_TEST(dysymtab_without_symtab) {
    MachoSpec s;
    s.symtab = false;
    CHECK_ERR(parse(build_macho(s).bytes), ErrorCode::Malformed);
}

ORVYN_TEST(dyld_info_range_out_of_bounds) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch32(b, built.cmd["dyld_info"] + 40, 0xfffffff0u);
    CHECK_ERR(parse(b), ErrorCode::OutOfBounds);
}

ORVYN_TEST(code_signature_range_out_of_bounds) {
    MachoSpec s;
    s.code_signature = true;
    auto built = build_macho(s);
    auto b = built.bytes;
    patch32(b, built.cmd["code_signature"] + 12, 0x7ffffff0);
    CHECK_ERR(parse(b), ErrorCode::OutOfBounds);
}

ORVYN_TEST(entry_point_outside_any_segment) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch64(b, built.cmd["main"] + 8, 0x500000);
    CHECK_ERR(parse(b), ErrorCode::Malformed);
}

ORVYN_TEST(duplicate_unique_commands_rejected) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch32(b, built.cmd["uuid"], 0x80000028);  // second LC_MAIN
    CHECK_ERR(parse(b), ErrorCode::Malformed);
}

ORVYN_TEST(build_version_tool_count_overflow) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch32(b, built.cmd["build_version"] + 20, 0xffffffff);
    CHECK_ERR(parse(b), ErrorCode::Malformed);
}

ORVYN_TEST(truncated_files_never_crash) {
    MachoSpec s;
    s.code_signature = true;
    s.rpaths = {"@loader_path/lib"};
    auto good = build_macho(s).bytes;
    int rejected = 0;
    for (size_t n = 0; n < good.size(); n += 7) {
        std::vector<uint8_t> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
        if (!parse(cut).ok()) ++rejected;
    }
    CHECK(rejected > 100);
    CHECK(parse(good).ok());
}

ORVYN_TEST(encrypted_range_out_of_bounds) {
    MachoSpec s;
    s.encrypted = true;
    auto built = build_macho(s);
    auto b = built.bytes;
    patch32(b, built.cmd["encryption"] + 12, 0x7fffffff);
    CHECK_ERR(parse(b), ErrorCode::OutOfBounds);
}

// Regression (libFuzzer finding): strsize == 0 with a wild stroff made parse_symbols throw.
ORVYN_TEST(regression_symtab_zero_strsize_with_wild_offset_is_an_error_not_an_exception) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch32(b, built.cmd["symtab"] + 16, 0x7fffffff);  // stroff
    patch32(b, built.cmd["symtab"] + 20, 0);           // strsize
    CHECK_ERR(parse(b), ErrorCode::Malformed);          // symbol names point into a missing string table
    CHECK(parse(b, false).ok());                        // without symbols the image itself is fine
}
