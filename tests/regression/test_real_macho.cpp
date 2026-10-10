// Real-world validation: Mach-O files produced by LLVM (clang + ld64.lld, see
// tools/gen-real-fixtures.cmd) are parsed by CIDER and the result is compared field by field with
// the output of an independent tool, llvm-objdump.
//
// * The committed golden snapshots (tests/fixtures/real/golden) are always compared.
// * When llvm-objdump is available (CIDER_LLVM_BIN or PATH) the live output must also equal the
//   golden snapshot (detects tool drift); otherwise that check is skipped.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include "cider/analysis.hpp"
#include "cider/bundle.hpp"
#include "cider/macho.hpp"
#include "cider/process.hpp"
#include "cider/util.hpp"
#include "macho_builder.hpp"
#include "objdump_compare.hpp"
#include "test_framework.hpp"

using namespace cider;
using namespace cider::testing;
namespace fs = std::filesystem;

namespace {

const fs::path kReal = fs::path(CIDER_REAL_FIXTURES_DIR);

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { report_failure(__FILE__, __LINE__, "cannot read " + path_to_utf8(p)); throw AbortTest{}; }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<uint8_t> bytes_of(const std::string& name) {
    auto r = read_file(kReal / "bin" / name, 64u << 20);
    if (!r.ok()) { report_failure(__FILE__, __LINE__, "fixture missing: " + name); throw AbortTest{}; }
    return r.value();
}

macho::File parse_real(const std::string& name, bool symbols = true) {
    auto b = bytes_of(name);
    macho::ParseOptions o;
    o.parse_symbols = symbols;
    auto r = macho::parse_file(ByteView(b), o);
    if (!r.ok()) { report_failure(__FILE__, __LINE__, name + ": " + r.error().message); throw AbortTest{}; }
    return r.value();
}

using namespace cider::testing::objdump;

// Is llvm-objdump runnable here?
std::optional<fs::path> objdump_exe() {
    static std::optional<fs::path> found = []() -> std::optional<fs::path> {
        std::vector<fs::path> dirs;
        if (const char* b = std::getenv("CIDER_LLVM_BIN")) dirs.push_back(path_from_utf8(b));
        if (const char* p = std::getenv("PATH")) {
            std::string s = p;
            size_t st = 0;
            while (st <= s.size()) {
                size_t e = s.find(';', st);
                dirs.push_back(path_from_utf8(s.substr(st, e == std::string::npos ? std::string::npos : e - st)));
                if (e == std::string::npos) break;
                st = e + 1;
            }
        }
        for (auto& d : dirs) {
            auto exe = d / "llvm-objdump.exe";
            std::error_code ec;
            if (d.is_absolute() && fs::is_regular_file(exe, ec)) return exe;
        }
        return std::nullopt;
    }();
    return found;
}

std::string live_objdump(const std::string& name, const std::vector<std::string>& flags) {
    std::vector<std::string> args = {"--macho"};
    args.insert(args.end(), flags.begin(), flags.end());
    args.push_back(path_to_utf8(kReal / "bin" / name));
    auto r = run_process(*objdump_exe(), args, std::chrono::seconds(60));
    return r.ok() ? r.value().output : std::string();
}

void check_fixture(const std::string& name, bool has_syms = true) {
    auto file = parse_real(name);
    REQUIRE_EQ(file.slices.size(), size_t{1});
    REQUIRE(file.slices[0].image.has_value());
    const auto& img = *file.slices[0].image;
    auto dump = parse_private_headers(slurp(kReal / "golden" / (name + ".private-headers.txt")));
    compare_image(name, dump, img);
    if (has_syms) compare_symbols(name, slurp(kReal / "golden" / (name + ".syms.txt")), img);
}

}  // namespace

CIDER_TEST(real_executable_matches_llvm_objdump) { check_fixture("hello_x86_64"); }
CIDER_TEST(real_arm64_executable_matches_llvm_objdump) { check_fixture("hello_arm64"); }
CIDER_TEST(real_adhoc_signed_executable_matches_llvm_objdump) {
    check_fixture("hello_signed_x86_64");
    auto img = *parse_real("hello_signed_x86_64").slices[0].image;
    REQUIRE(img.code_signature.has_value());
    CHECK(img.code_signature->blob_parsed);
    CHECK(img.code_signature->adhoc);
    CHECK_EQ(img.code_signature->identifier, std::string("hello_signed_x86_64"));  // from the SuperBlob, not the path
    CHECK(img.warnings.empty());
}
CIDER_TEST(real_chained_fixups_executable_matches_llvm_objdump) {
    check_fixture("hello_chained_x86_64");
    auto img = *parse_real("hello_chained_x86_64").slices[0].image;
    CHECK(img.chained_fixups.has_value());
    CHECK(img.exports_trie.has_value());
    CHECK(!img.dyld_info.has_value());
}
CIDER_TEST(real_dylibs_match_llvm_objdump) {
    check_fixture("libfoo.dylib");
    check_fixture("libweak.dylib");
    auto foo = *parse_real("libfoo.dylib").slices[0].image;
    CHECK_EQ(foo.id_dylib->path, std::string("@rpath/libfoo.dylib"));
    CHECK(foo.filetype == macho::kFileDylib);
    CHECK(!foo.entry.has_value());
}
CIDER_TEST(real_app_with_rpaths_and_weak_dependency_matches_llvm_objdump) {
    check_fixture("app_x86_64");
    auto img = *parse_real("app_x86_64").slices[0].image;
    REQUIRE_EQ(img.dylibs.size(), size_t{3});
    CHECK(img.dylibs[2].kind == macho::DylibKind::Weak);
    REQUIRE_EQ(img.rpaths.size(), size_t{2});
}
CIDER_TEST(real_relocatable_object_matches_llvm_objdump) {
    auto file = parse_real("hello_x86_64.o");
    const auto& img = *file.slices[0].image;
    CHECK(img.filetype == macho::kFileObject);
    auto dump = parse_private_headers(slurp(kReal / "golden" / "hello_x86_64.o.private-headers.txt"));
    compare_image("hello_x86_64.o", dump, img);
    compare_symbols("hello_x86_64.o", slurp(kReal / "golden" / "hello_x86_64.o.syms.txt"), img);
}

CIDER_TEST(real_universal_binary_matches_llvm_objdump_and_its_thin_slices) {
    auto file = parse_real("hello_universal");
    REQUIRE(file.fat);
    REQUIRE_EQ(file.slices.size(), size_t{2});
    std::string text = slurp(kReal / "golden" / "hello_universal.universal-headers.txt");
    // "offset N / size N / align 2^N" appear once per architecture, in order
    std::vector<uint64_t> offsets, sizes, aligns;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (line.rfind("offset ", 0) == 0) offsets.push_back(num(line.substr(7)));
        else if (line.rfind("size ", 0) == 0) sizes.push_back(num(line.substr(5)));
        else if (line.rfind("align 2^", 0) == 0) aligns.push_back(num(line.substr(8)));
    }
    REQUIRE_EQ(offsets.size(), size_t{2});
    for (size_t i = 0; i < 2; ++i) {
        CHECK_EQ(file.slices[i].offset, offsets[i]);
        CHECK_EQ(file.slices[i].size, sizes[i]);
        CHECK_EQ(static_cast<uint64_t>(file.slices[i].align), aligns[i]);
        REQUIRE(file.slices[i].image.has_value());
    }
    CHECK_EQ(macho::cpu_name(file.slices[0].cputype, file.slices[0].cpusubtype), std::string("x86_64"));
    CHECK_EQ(macho::cpu_name(file.slices[1].cputype, file.slices[1].cpusubtype), std::string("arm64"));
    CHECK(file.warnings.empty());
    // each slice parses to the same result as the thin file it was built from
    auto thin_x = parse_real("hello_x86_64");
    auto thin_a = parse_real("hello_arm64");
    CHECK_EQ(file.slices[0].image->segments.size(), thin_x.slices[0].image->segments.size());
    CHECK_EQ(file.slices[1].image->symbols.size(), thin_a.slices[0].image->symbols.size());
    CHECK_EQ(file.slices[0].image->entry->file_offset, thin_x.slices[0].image->entry->file_offset);
}

CIDER_TEST(live_llvm_objdump_still_agrees_with_the_golden_snapshots) {
    if (!objdump_exe()) SKIP("llvm-objdump not found (set CIDER_LLVM_BIN)");
    for (const char* n : {"hello_x86_64", "app_x86_64", "libfoo.dylib", "hello_signed_x86_64"}) {
        std::string live = live_objdump(n, {"--private-headers"});
        std::string golden = slurp(kReal / "golden" / (std::string(n) + ".private-headers.txt"));
        auto norm = [](std::string s) {  // first line is the (path-dependent) file name
            s.erase(0, s.find('\n') + 1);
            s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
            return s;
        };
        CHECK_EQ(norm(live), norm(golden));
        auto img = *parse_real(n).slices[0].image;
        compare_image(std::string("live ") + n, parse_private_headers(live), img);
    }
}

CIDER_TEST(real_fixtures_dependency_analysis_resolves_a_bundle) {
    TempDir t;
    // lay the real files out like an application bundle: rpath @executable_path/../Frameworks
    t.write("Real.app/Contents/Info.plist", xml_plist({{"CFBundleExecutable", "app"}, {"CFBundleIdentifier", "org.cider.real"}, {"CFBundleName", "Real"}}));
    t.write("Real.app/Contents/MacOS/app", bytes_of("app_x86_64"));
    t.write("Real.app/Contents/Frameworks/libfoo.dylib", bytes_of("libfoo.dylib"));
    t.write("Real.app/Contents/Frameworks/libweak.dylib", bytes_of("libweak.dylib"));
    auto info = bundle::inspect(t.path() / "Real.app");
    REQUIRE(info.ok());
    CHECK(info.value().executable_found);
    auto bytes = read_file(info.value().executable_path, 1 << 26).value();
    auto file = macho::parse_file(ByteView(bytes)).value();
    auto rep = analysis::analyze_dependencies(*file.slices[0].image, info.value().executable_path, info.value().root);
    size_t bundled = 0, system = 0;
    for (auto& d : rep.deps) {
        bundled += d.cls == analysis::DepClass::Bundled;
        system += d.cls == analysis::DepClass::System;
        CHECK(d.cls != analysis::DepClass::Missing);
    }
    CHECK_EQ(bundled, size_t{2});
    CHECK_EQ(rep.images_scanned, size_t{3});
    CHECK(system >= 1);
    auto compat = analysis::assess(file, &rep, &info.value());
    CHECK(compat.status == analysis::CompatStatus::AnalysisCompatible);
    CHECK(!compat.execution_confirmed);

    // without the frameworks the same executable reports a missing required dependency
    TempDir t2;
    t2.write("Real.app/Contents/Info.plist", xml_plist({{"CFBundleExecutable", "app"}}));
    t2.write("Real.app/Contents/MacOS/app", bytes_of("app_x86_64"));
    auto info2 = bundle::inspect(t2.path() / "Real.app").value();
    auto rep2 = analysis::analyze_dependencies(*file.slices[0].image, info2.executable_path, info2.root);
    CHECK_EQ(analysis::assess(file, &rep2, &info2).status, analysis::CompatStatus::PendingDependencies);
}

// ---- regression tests for bugs found by real fixtures ---------------------------------------------------------
CIDER_TEST(real_truncations_and_bitflips_of_real_binaries_never_crash) {
    for (const char* n : {"hello_x86_64", "hello_universal", "app_x86_64", "hello_signed_x86_64"}) {
        auto good = bytes_of(n);
        for (size_t cut = 0; cut < good.size(); cut += 61) {
            std::vector<uint8_t> c(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
            macho::ParseOptions o;
            o.parse_symbols = true;
            (void)macho::parse_file(ByteView(c), o);
        }
        for (size_t pos = 0; pos < std::min<size_t>(good.size(), 2048); ++pos) {
            auto b = good;
            b[pos] ^= 0xFF;
            macho::ParseOptions o;
            o.parse_symbols = true;
            (void)macho::parse_file(ByteView(b), o);
        }
    }
    CHECK(true);
}

// Regression for a crash found by libFuzzer (tests/fuzz/fuzz_macho.cpp, 2026-10-10): LC_SYMTAB with
// strsize == 0 skipped offset validation, then parse_symbols sliced at the unvalidated stroff and
// threw std::bad_variant_access. The input is the unminimised crashing file.
CIDER_TEST(regression_fuzz_crash_symtab_zero_strsize) {
    auto r = read_file(fs::path(CIDER_REAL_FIXTURES_DIR).parent_path() / "regression" / "fuzz-macho-strsize0-bad-variant-access.bin", 1 << 20);
    REQUIRE(r.ok());
    macho::ParseOptions o;
    o.parse_symbols = true;
    (void)macho::parse_file(ByteView(r.value()), o);  // must not throw or crash
    CHECK(true);
}

// ---- real-world applications (optional, not downloaded by the test-suite) ----------------------------------------
// tools/validate-real-dmgs.ps1 downloads official open-source release DMGs, extracts them with
// `cider extract` and points CIDER_REAL_APPS_DIR at the result. Every Mach-O file found there is
// parsed by CIDER and compared, slice by slice, with live llvm-objdump output.
namespace {
bool looks_macho(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    uint8_t m[4] = {};
    f.read(reinterpret_cast<char*>(m), 4);
    return f.gcount() == 4 && macho::identify(ByteView(m, 4)) != macho::Format::Unknown;
}

std::string run_objdump(const std::vector<std::string>& args) {
    auto r = run_process(*objdump_exe(), args, std::chrono::seconds(120), 256u << 20);
    return r.ok() ? r.value().output : std::string();
}
}  // namespace

CIDER_TEST(real_world_extracted_apps_match_llvm_objdump) {
    const char* dir = std::getenv("CIDER_REAL_APPS_DIR");
    if (!dir) SKIP("CIDER_REAL_APPS_DIR not set (see tools/validate-real-dmgs.ps1)");
    if (!objdump_exe()) SKIP("llvm-objdump not found (set CIDER_LLVM_BIN)");
    size_t files = 0, slices = 0, sym_checked = 0, failures_before = static_cast<size_t>(failure_count());
    std::error_code ec;
    for (fs::recursive_directory_iterator it(extended_path(path_from_utf8(dir)), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !looks_macho(it->path())) continue;
        auto bytes = read_file(it->path(), 512u << 20);
        if (!bytes.ok()) continue;
        macho::ParseOptions po;
        po.parse_symbols = true;
        auto f = macho::parse_file(ByteView(bytes.value()), po);
        std::string label = path_to_utf8(it->path().filename());
        if (!f.ok()) { report_failure(__FILE__, __LINE__, label + ": CIDER rejected a real file: " + f.error().message); continue; }
        ++files;
        for (auto& sl : f.value().slices) {
            std::string arch = macho::cpu_name(sl.cputype, sl.cpusubtype);
            if (!sl.image) { report_failure(__FILE__, __LINE__, label + " [" + arch + "]: slice rejected: " + (sl.error ? sl.error->message : "")); continue; }
            ++slices;
            std::string path = path_to_utf8(it->path());
            std::vector<std::string> base = {"--macho"};
            if (f.value().fat) base.push_back("--arch=" + arch);
            auto ph = base; ph.push_back("--private-headers"); ph.push_back(path);
            std::string text = run_objdump(ph);
            if (text.empty()) { report_failure(__FILE__, __LINE__, label + ": llvm-objdump produced no output"); continue; }
            compare_image(label + " [" + arch + "]", parse_private_headers(text), *sl.image, /*strict=*/false);
            if (sl.image->symtab && sl.image->symtab->nsyms < 20000) {
                auto sy = base; sy.push_back("--syms"); sy.push_back(path);
                compare_symbols(label + " [" + arch + "]", run_objdump(sy), *sl.image);
                ++sym_checked;
            }
        }
    }
    std::printf("    real-world: %zu Mach-O file(s), %zu slice(s) compared with llvm-objdump (%zu with symbol tables)\n", files, slices, sym_checked);
    CHECK(files > 0);
    CHECK_EQ(static_cast<size_t>(failure_count()), failures_before);
}
