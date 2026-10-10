#include "cider/macho.hpp"
#include "macho_builder.hpp"
#include "test_framework.hpp"

using namespace cider;
using namespace cider::testing;

static std::vector<uint8_t> x64() { return build_macho({}).bytes; }
static std::vector<uint8_t> arm64() {
    MachoSpec s;
    s.cputype = macho::kCpuArm64;
    s.cpusubtype = 0;
    return build_macho(s).bytes;
}

CIDER_TEST(fat_two_architectures) {
    auto fat = build_fat({{macho::kCpuX86_64, 3, x64()}, {macho::kCpuArm64, 0, arm64()}});
    CHECK(macho::identify(ByteView(fat)) == macho::Format::Fat);
    auto r = macho::parse_file(ByteView(fat));
    REQUIRE(r.ok());
    const auto& f = r.value();
    CHECK(f.fat);
    REQUIRE_EQ(f.slices.size(), size_t{2});
    CHECK_EQ(f.slices[0].offset, uint64_t{0x4000});
    CHECK(f.slices[0].image.has_value());
    CHECK(f.slices[1].image.has_value());
    CHECK(f.find_slice(macho::kCpuArm64) != nullptr);
    CHECK(f.warnings.empty());
}

CIDER_TEST(fat64_container) {
    auto fat = build_fat({{macho::kCpuX86_64, 3, x64()}}, true);
    CHECK(macho::identify(ByteView(fat)) == macho::Format::Fat64);
    auto r = macho::parse_file(ByteView(fat));
    REQUIRE(r.ok());
    CHECK(r.value().fat64);
    CHECK(r.value().slices[0].image.has_value());
}

CIDER_TEST(fat_slice_error_is_isolated) {
    auto bad = x64();
    patch32(bad, 12, 99);  // invalid filetype
    auto fat = build_fat({{macho::kCpuX86_64, 3, x64()}, {macho::kCpuArm64, 0, bad}});
    auto r = macho::parse_file(ByteView(fat));
    REQUIRE(r.ok());
    CHECK(r.value().slices[0].image.has_value());
    CHECK(!r.value().slices[1].image.has_value());
    REQUIRE(r.value().slices[1].error.has_value());
}

CIDER_TEST(fat_header_cputype_disagrees_with_slice) {
    auto fat = build_fat({{macho::kCpuArm64, 0, x64()}});
    auto r = macho::parse_file(ByteView(fat));
    REQUIRE(r.ok());
    CHECK(!r.value().slices[0].image.has_value());
}

CIDER_TEST(fat_32bit_slice_reported_not_fatal) {
    std::vector<uint8_t> m32{0xce, 0xfa, 0xed, 0xfe, 7, 0, 0, 0};
    m32.resize(64);
    auto fat = build_fat({{macho::kCpuX86_64, 3, x64()}, {macho::kCpuX86, 3, m32}});
    auto r = macho::parse_file(ByteView(fat));
    REQUIRE(r.ok());
    CHECK(r.value().slices[0].image.has_value());
    REQUIRE(r.value().slices[1].error.has_value());
    CHECK(r.value().slices[1].error->code == ErrorCode::Unsupported);
    CHECK_EQ(macho::cpu_name(macho::kCpuX86, 3), std::string("i386"));
}

CIDER_TEST(fat_invalid_headers) {
    auto good = build_fat({{macho::kCpuX86_64, 3, x64()}});
    CHECK_ERR(macho::parse_file(ByteView(good.data(), 7)), ErrorCode::Truncated);
    CHECK_ERR(macho::parse_file(ByteView(good.data(), 20)), ErrorCode::Truncated);

    auto b = good;
    for (int i = 0; i < 4; ++i) b[4 + i] = 0;  // zero archs
    CHECK_ERR(macho::parse_file(ByteView(b)), ErrorCode::Malformed);

    b = good;
    b[4] = 0xff; b[5] = 0xff; b[6] = 0xff; b[7] = 0xff;  // absurd count (also Java class look-alike)
    CHECK_ERR(macho::parse_file(ByteView(b)), ErrorCode::Malformed);
}

CIDER_TEST(fat_slice_out_of_bounds_or_overlapping) {
    auto good = build_fat({{macho::kCpuX86_64, 3, x64()}, {macho::kCpuArm64, 0, arm64()}});
    auto b = good;
    b[8 + 11] = 0xff; b[8 + 10] = 0xff;  // slice 0 size = huge
    CHECK_ERR(macho::parse_file(ByteView(b)), ErrorCode::OutOfBounds);

    b = good;  // slice 1 offset = slice 0 offset
    for (int i = 0; i < 4; ++i) b[8 + 20 + 8 + i] = b[8 + 8 + i];
    CHECK_ERR(macho::parse_file(ByteView(b)), ErrorCode::Overlap);

    b = good;  // slice offset inside arch table
    for (int i = 0; i < 4; ++i) b[8 + 8 + i] = 0;
    b[8 + 11] = 4;
    CHECK_ERR(macho::parse_file(ByteView(b)), ErrorCode::Overlap);

    b = good;  // zero-sized slice
    for (int i = 0; i < 4; ++i) b[8 + 12 + i] = 0;
    CHECK_ERR(macho::parse_file(ByteView(b)), ErrorCode::Malformed);
}

CIDER_TEST(fat_duplicate_arch_is_warning) {
    auto fat = build_fat({{macho::kCpuX86_64, 3, x64()}, {macho::kCpuX86_64, 3, x64()}});
    auto r = macho::parse_file(ByteView(fat));
    REQUIRE(r.ok());
    CHECK(!r.value().warnings.empty());
}

CIDER_TEST(fat_nested_container_rejected) {
    auto inner = build_fat({{macho::kCpuX86_64, 3, x64()}});
    auto outer = build_fat({{macho::kCpuX86_64, 3, inner}});
    auto r = macho::parse_file(ByteView(outer));
    REQUIRE(r.ok());
    REQUIRE(r.value().slices[0].error.has_value());
    CHECK(r.value().slices[0].error->code == ErrorCode::BadMagic);
}

CIDER_TEST(java_class_file_is_not_fat) {
    std::vector<uint8_t> cls{0xca, 0xfe, 0xba, 0xbe, 0x00, 0x00, 0x00, 0x34};  // version 52 -> "arch count" 52
    cls.resize(2000, 0);
    CHECK(!macho::parse_file(ByteView(cls)).ok());
}
