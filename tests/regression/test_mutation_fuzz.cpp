// Deterministic mutation fuzzing: random corruptions of valid fixtures must never crash,
// hang or read out of bounds (run under ORVYN_SANITIZE=ON for ASan coverage).
#include <cstdint>
#include <random>

#include "orvyn/macho.hpp"
#include "orvyn/plist.hpp"
#include "macho_builder.hpp"
#include "test_framework.hpp"

using namespace orvyn;
using namespace orvyn::testing;

namespace {
void mutate(std::vector<uint8_t>& b, std::mt19937_64& rng) {
    int edits = 1 + static_cast<int>(rng() % 4);
    for (int i = 0; i < edits; ++i) {
        size_t pos = rng() % b.size();
        switch (rng() % 4) {
            case 0: b[pos] = static_cast<uint8_t>(rng()); break;
            case 1: b[pos] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
            case 2: b[pos] = (rng() & 1) ? 0xff : 0x00; break;
            default: if (pos + 4 <= b.size()) patch32(b, pos, static_cast<uint32_t>(rng())); break;
        }
    }
}
}  // namespace

ORVYN_TEST(mutated_macho_never_crashes) {
    MachoSpec s;
    s.code_signature = true;
    s.rpaths = {"@loader_path/x"};
    s.weak_dylibs = {"@rpath/w.dylib"};
    auto thin = build_macho(s).bytes;
    MachoSpec a;
    a.cputype = macho::kCpuArm64;
    a.cpusubtype = 0;
    auto fat = build_fat({{macho::kCpuX86_64, 3, thin}, {macho::kCpuArm64, 0, build_macho(a).bytes}});
    std::mt19937_64 rng(0xC1DE5EEDull);
    macho::ParseOptions o;
    o.parse_symbols = true;
    size_t accepted = 0, rejected = 0;
    for (int iter = 0; iter < 20000; ++iter) {
        auto b = (iter & 1) ? fat : thin;
        mutate(b, rng);
        auto r = macho::parse_file(ByteView(b), o);
        (r.ok() ? accepted : rejected)++;
    }
    CHECK(rejected > 0);
    CHECK(accepted > 0);
}

ORVYN_TEST(mutated_plists_never_crash) {
    auto xml = xml_plist({{"CFBundleExecutable", "Example"}, {"CFBundleName", "A &amp; B"}});
    std::vector<uint8_t> x(xml.begin(), xml.end());
    auto bin = binary_plist({{"Name", "Example"}, {"Id", "com.x"}});
    std::mt19937_64 rng(42);
    for (int iter = 0; iter < 20000; ++iter) {
        auto b = (iter & 1) ? x : bin;
        mutate(b, rng);
        (void)plist::parse(ByteView(b));
    }
    CHECK(true);
}

ORVYN_TEST(random_garbage_never_crashes) {
    std::mt19937_64 rng(7);
    for (int iter = 0; iter < 5000; ++iter) {
        std::vector<uint8_t> b(1 + rng() % 256);
        for (auto& c : b) c = static_cast<uint8_t>(rng());
        if (iter % 3 == 0) { b[0] = 0xcf; if (b.size() > 3) { b[1] = 0xfa; b[2] = 0xed; b[3] = 0xfe; } }
        (void)macho::parse_file(ByteView(b));
        (void)plist::parse(ByteView(b));
    }
    CHECK(true);
}

// Regression: ncmds*8 vs sizeofcmds overflow check must use 64-bit math.
ORVYN_TEST(regression_ncmds_times_eight_does_not_wrap) {
    auto b = build_macho({}).bytes;
    patch32(b, 16, 0x20000000);  // 0x20000000 * 8 wraps in 32-bit arithmetic
    patch32(b, 20, 64);
    CHECK(!macho::parse_image(ByteView(b)).ok());
}
