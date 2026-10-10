// End-to-end tests: run the real cider executable (CIDER_EXE) on generated fixtures.
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <optional>

#include "cider/dmg.hpp"
#include "cider/macho.hpp"
#include "cider/process.hpp"
#include "cider/util.hpp"
#include "dmg_builder.hpp"
#include "test_framework.hpp"

using namespace cider;
using namespace cider::testing;
namespace fs = std::filesystem;

namespace {

struct Run {
    int code = -1;
    std::string out;
};

Run cider_run(const std::vector<std::string>& args) {
    const char* exe = std::getenv("CIDER_EXE");
    if (!exe) { report_failure(__FILE__, __LINE__, "CIDER_EXE is not set"); throw AbortTest{}; }
    auto r = run_process(fs::absolute(path_from_utf8(exe)), args, std::chrono::minutes(2));
    if (!r.ok()) { report_failure(__FILE__, __LINE__, "run_process: " + r.error().message); throw AbortTest{}; }
    return {r.value().exit_code, r.value().output};
}

bool has(const Run& r, const std::string& s) { return r.out.find(s) != std::string::npos; }

// Strict JSON syntax checker (no values kept): proves --json output is machine-readable.
class JsonCheck {
public:
    explicit JsonCheck(const std::string& s) : s_(s) {}
    bool valid() { ws(); if (!value()) return false; ws(); return p_ == s_.size(); }

private:
    const std::string& s_;
    size_t p_ = 0;
    void ws() { while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\n' || s_[p_] == '\r' || s_[p_] == '\t')) ++p_; }
    bool lit(const char* l) { size_t n = std::char_traits<char>::length(l); if (s_.compare(p_, n, l) != 0) return false; p_ += n; return true; }
    bool str() {
        if (p_ >= s_.size() || s_[p_] != '"') return false;
        ++p_;
        while (p_ < s_.size()) {
            unsigned char c = static_cast<unsigned char>(s_[p_]);
            if (c == '"') { ++p_; return true; }
            if (c < 0x20) return false;
            if (c == '\\') { ++p_; if (p_ >= s_.size()) return false; if (s_[p_] == 'u') p_ += 4; }
            ++p_;
        }
        return false;
    }
    bool num() {
        size_t st = p_;
        if (p_ < s_.size() && s_[p_] == '-') ++p_;
        while (p_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[p_])) || s_[p_] == '.' || s_[p_] == 'e' || s_[p_] == 'E' || s_[p_] == '+' || s_[p_] == '-')) ++p_;
        return p_ > st;
    }
    bool value() {
        ws();
        if (p_ >= s_.size()) return false;
        char c = s_[p_];
        if (c == '{') {
            ++p_; ws();
            if (p_ < s_.size() && s_[p_] == '}') { ++p_; return true; }
            for (;;) {
                ws(); if (!str()) return false; ws();
                if (p_ >= s_.size() || s_[p_++] != ':') return false;
                if (!value()) return false; ws();
                if (p_ < s_.size() && s_[p_] == ',') { ++p_; continue; }
                if (p_ < s_.size() && s_[p_] == '}') { ++p_; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++p_; ws();
            if (p_ < s_.size() && s_[p_] == ']') { ++p_; return true; }
            for (;;) {
                if (!value()) return false; ws();
                if (p_ < s_.size() && s_[p_] == ',') { ++p_; continue; }
                if (p_ < s_.size() && s_[p_] == ']') { ++p_; return true; }
                return false;
            }
        }
        if (c == '"') return str();
        if (c == 't') return lit("true");
        if (c == 'f') return lit("false");
        if (c == 'n') return lit("null");
        return num();
    }
};

// stdout/stderr are merged by the runner; JSON tests use commands that only log on stderr at WARN.
std::string json_part(const std::string& out) {
    auto at = out.find('{');
    return at == std::string::npos ? out : out.substr(at);
}

bool seven_zip_available() {
    static bool v = dmg::locate_7zip().ok();
    return v;
}

std::vector<uint8_t> iso() { return build_iso(example_app_tree()); }

}  // namespace

CIDER_TEST(version_and_help) {
    auto v = cider_run({"--version"});
    CHECK_EQ(v.code, 0);
    CHECK(has(v, "cider 0.1.1"));
    auto h = cider_run({"--help"});
    CHECK_EQ(h.code, 0);
    for (const char* cmd : {"inspect", "dmg-info", "extract", "dependencies", "compatibility", "run", "logs", "doctor"}) CHECK(has(h, cmd));
}

CIDER_TEST(usage_errors_return_2) {
    CHECK_EQ(cider_run({}).code, 2);
    CHECK_EQ(cider_run({"frobnicate"}).code, 2);
    CHECK_EQ(cider_run({"inspect", "--bogus"}).code, 2);
    CHECK_EQ(cider_run({"inspect"}).code, 2);
    CHECK_EQ(cider_run({"--log-level"}).code, 2);
    CHECK_EQ(cider_run({"--log-level", "loud", "doctor"}).code, 2);
    CHECK_EQ(cider_run({"extract", "x.dmg"}).code, 2);
    CHECK_EQ(cider_run({"extract", "x.dmg", "--max-size", "0", "-o", "y"}).code, 2);
}

CIDER_TEST(inspect_macho_text_and_json) {
    TempDir t;
    auto p = t.write("prog", build_macho({}).bytes);
    auto r = cider_run({"inspect", path_to_utf8(p)});
    CHECK_EQ(r.code, 0);
    CHECK(has(r, "CIDER - Darwin Executable Runtime"));
    CHECK(has(r, "Format: Mach-O 64-bit"));
    CHECK(has(r, "Architecture: x86_64"));
    CHECK(has(r, "Platform: macOS"));
    CHECK(has(r, "Entry Point: Detected"));
    CHECK(has(r, "/usr/lib/libSystem.B.dylib"));
    CHECK(has(r, "Execution support not yet confirmed"));

    auto j = cider_run({"inspect", path_to_utf8(p), "--json", "--symbols"});
    CHECK_EQ(j.code, 0);
    CHECK(JsonCheck(json_part(j.out)).valid());
    CHECK(has(j, "\"architecture\": \"x86_64\""));
    CHECK(has(j, "\"_main\""));
    CHECK(has(j, "\"execution_confirmed\": false"));
}

CIDER_TEST(inspect_fat_and_arm64_only) {
    TempDir t;
    MachoSpec a;
    a.cputype = macho::kCpuArm64;
    a.cpusubtype = 0;
    auto fat = t.write("fat", build_fat({{macho::kCpuX86_64, 3, build_macho({}).bytes}, {macho::kCpuArm64, 0, build_macho(a).bytes}}));
    auto r = cider_run({"inspect", path_to_utf8(fat)});
    CHECK_EQ(r.code, 0);
    CHECK(has(r, "Universal (FAT)"));
    CHECK(has(r, "x86_64, arm64"));
    auto arm = t.write("arm", build_macho(a).bytes);
    auto c = cider_run({"compatibility", path_to_utf8(arm)});
    CHECK_EQ(c.code, 1);
    CHECK(has(c, "not compatible"));
}

CIDER_TEST(inspect_app_bundle_and_dependencies) {
    TempDir t;
    MachoSpec s;
    s.dylibs = {"/usr/lib/libSystem.B.dylib", "@rpath/libmissing.dylib"};
    s.rpaths = {"@executable_path/../Frameworks"};
    auto app = make_app(t, "Example.app", build_macho(s).bytes);
    auto r = cider_run({"inspect", path_to_utf8(app)});
    CHECK_EQ(r.code, 0);
    CHECK(has(r, "Application: Example"));
    CHECK(has(r, "Bundle Identifier: com.example.app"));
    CHECK(has(r, "NOT FOUND"));
    CHECK(has(r, "pending dependencies"));
    auto d = cider_run({"dependencies", path_to_utf8(app), "--json"});
    CHECK_EQ(d.code, 0);
    CHECK(JsonCheck(json_part(d.out)).valid());
    CHECK(has(d, "\"class\": \"missing\""));
}

CIDER_TEST(invalid_inputs_fail_with_exit_1) {
    TempDir t;
    CHECK_EQ(cider_run({"inspect", path_to_utf8(t.path() / "nope")}).code, 1);
    auto junk = t.write("junk.bin", std::vector<uint8_t>(300, 0x41));
    auto r = cider_run({"inspect", path_to_utf8(junk)});
    CHECK_EQ(r.code, 1);
    CHECK(has(r, "unrecognised"));
    auto b = build_macho({}).bytes;
    patch32(b, 20, 0xffffffff);
    auto bad = t.write("bad", b);
    auto rb = cider_run({"inspect", path_to_utf8(bad)});
    CHECK_EQ(rb.code, 1);
    auto rj = cider_run({"inspect", path_to_utf8(bad), "--json"});
    CHECK_EQ(rj.code, 1);
    CHECK(JsonCheck(json_part(rj.out)).valid());
    CHECK(has(rj, "\"ok\": false"));
    t.write("Empty.app/Contents/placeholder", std::string("x"));
    CHECK_EQ(cider_run({"inspect", path_to_utf8(t.path() / "Empty.app")}).code, 1);
}

CIDER_TEST(run_is_honestly_unavailable) {
    TempDir t;
    auto p = t.write("prog", build_macho({}).bytes);
    auto r = cider_run({"run", path_to_utf8(p)});
    CHECK_EQ(r.code, 2);
    CHECK(has(r, "not available"));
}

CIDER_TEST(doctor_and_logs) {
    auto d = cider_run({"doctor"});
    CHECK_EQ(d.code, 0);
    CHECK(has(d, "mach-o parser"));
    CHECK(has(d, "not implemented in 0.1"));
    auto dj = cider_run({"doctor", "--json"});
    CHECK(JsonCheck(json_part(dj.out)).valid());

    TempDir t;
    auto p = t.write("prog", build_macho({}).bytes);
    auto log = path_to_utf8(t.path() / "cider.log");
    CHECK_EQ(cider_run({"inspect", path_to_utf8(p), "--log-file", log, "--log-level", "info"}).code, 0);
    auto l = cider_run({"logs", log});
    CHECK_EQ(l.code, 0);
    CHECK(has(l, "[CIDER][INFO] Loading"));
    CHECK(has(l, "[CIDER][INFO] Architecture: x86_64"));
    CHECK_EQ(cider_run({"logs", path_to_utf8(t.path() / "missing.log")}).code, 1);
}

// ---- disk images -------------------------------------------------------------------------------
CIDER_TEST(dmg_info_works_without_the_backend) {
    TempDir t;
    auto img = t.write("app.dmg", build_dmg(iso()).bytes);
    auto r = cider_run({"dmg-info", path_to_utf8(img), "--7z", path_to_utf8(t.path() / "no-such-7z.exe")});
    CHECK_EQ(r.code, 0);
    CHECK(has(r, "Container: UDIF"));
    CHECK(has(r, "Compression: raw (uncompressed)"));
    CHECK(has(r, "ISO 9660 + Joliet (verified from data)"));
    CHECK(has(r, "not available"));
    auto j = cider_run({"dmg-info", path_to_utf8(img), "--json", "--7z", path_to_utf8(t.path() / "no-such-7z.exe")});
    CHECK(JsonCheck(json_part(j.out)).valid());
    CHECK(has(j, "\"available\": false"));
}

CIDER_TEST(inspect_dmg_without_backend_is_a_partial_result) {
    TempDir t;
    auto img = t.write("app.dmg", build_dmg(iso()).bytes);
    auto r = cider_run({"inspect", path_to_utf8(img), "--7z", path_to_utf8(t.path() / "no-such-7z.exe")});
    CHECK_EQ(r.code, 3);
    CHECK(has(r, "Application analysis skipped"));
    CHECK(has(r, "Container: UDIF"));
    auto j = cider_run({"inspect", path_to_utf8(img), "--json", "--7z", path_to_utf8(t.path() / "no-such-7z.exe")});
    CHECK_EQ(j.code, 3);
    CHECK(JsonCheck(json_part(j.out)).valid());
    CHECK(has(j, "\"status\": \"partial\""));
}

CIDER_TEST(inspect_dmg_unsupported_kinds_fail_clearly) {
    TempDir t;
    std::vector<uint8_t> enc(4096, 0);
    std::copy_n("encrcdsa", 8, enc.begin());
    auto r = cider_run({"inspect", path_to_utf8(t.write("enc.dmg", enc))});
    CHECK_EQ(r.code, 1);
    CHECK(has(r, "encrypted"));
    std::vector<uint8_t> sp(4096, 0);
    std::copy_n("sprs", 4, sp.begin());
    CHECK(has(cider_run({"inspect", path_to_utf8(t.write("sp.dmg", sp))}), "sparse"));
    CHECK_EQ(cider_run({"extract", path_to_utf8(t.write("enc2.dmg", enc)), "-o", path_to_utf8(t.path() / "o")}).code, 1);
    CHECK(!fs::exists(t.path() / "o"));
    DmgOptions o;
    o.corrupt = DmgCorrupt::BadMishMagic;
    auto bad = cider_run({"inspect", path_to_utf8(t.write("bad.dmg", build_dmg(iso(), o).bytes))});
    CHECK_EQ(bad.code, 1);
    CHECK(has(bad, "mish"));
}

CIDER_TEST(inspect_dmg_end_to_end_with_7zip) {
    if (!seven_zip_available()) SKIP("7-Zip not installed");
    TempDir t;
    fs::create_directories(t.path() / "tmp");
    DmgOptions z;
    z.codec = DmgCodec::ZlibStored;
    auto img = t.write("anything.bin", build_dmg(iso(), z).bytes);  // extension is irrelevant
    auto r = cider_run({"inspect", path_to_utf8(img), "--temp-dir", path_to_utf8(t.path() / "tmp")});
    CHECK_EQ(r.code, 0);
    CHECK(has(r, "Compression: zlib"));
    CHECK(has(r, "Applications found: 1"));
    CHECK(has(r, "Application: Example 2.0"));
    CHECK(has(r, "Bundle Identifier: com.example.dmgapp"));
    CHECK(has(r, "Architecture: x86_64"));
    CHECK(has(r, "Foo.framework [x86_64]"));
    CHECK(has(r, "/usr/lib/libSystem.B.dylib"));
    CHECK(has(r, "Execution support not yet confirmed"));
    // the temporary extraction was removed
    size_t left = 0;
    for ([[maybe_unused]] auto& e : fs::directory_iterator(t.path() / "tmp")) ++left;
    CHECK_EQ(left, size_t{0});

    auto j = cider_run({"inspect", path_to_utf8(img), "--json", "--temp-dir", path_to_utf8(t.path() / "tmp")});
    CHECK_EQ(j.code, 0);
    CHECK(JsonCheck(json_part(j.out)).valid());
    CHECK(has(j, "\"cleanup\": true"));
    CHECK(has(j, "\"identifier\": \"com.example.dmgapp\""));
    CHECK(has(j, "\"status\": \"ok\""));
}

CIDER_TEST(extract_command_end_to_end_with_7zip) {
    if (!seven_zip_available()) SKIP("7-Zip not installed");
    TempDir t;
    auto img = t.write("app.dmg", build_dmg(iso()).bytes);
    auto out = t.path() / "extracted";
    auto r = cider_run({"extract", path_to_utf8(img), "--output", path_to_utf8(out)});
    CHECK_EQ(r.code, 0);
    CHECK(has(r, "Applications found: 1"));
    CHECK(has(r, "Nothing was executed"));
    CHECK(fs::exists(out / "0" / "Example.app" / "Contents" / "Info.plist"));

    // second run into the now non-empty directory is refused
    auto again = cider_run({"extract", path_to_utf8(img), "-o", path_to_utf8(out)});
    CHECK_EQ(again.code, 1);
    CHECK(has(again, "not empty"));
    // the extracted app can be analysed on its own
    auto app = cider_run({"inspect", path_to_utf8(out / "0" / "Example.app")});
    CHECK_EQ(app.code, 0);
    CHECK(has(app, "com.example.dmgapp"));

    auto limited = cider_run({"extract", path_to_utf8(img), "-o", path_to_utf8(t.path() / "small"), "--max-size", "1"});
    CHECK_EQ(limited.code, 0);  // 1 MiB is plenty for this fixture
    auto info = cider_run({"dmg-info", path_to_utf8(img)});
    CHECK_EQ(info.code, 0);
    CHECK(has(info, "can open this image: yes"));
}

// ---- real LLVM-built fixtures and a hdiutil-like image layout (CIDER 0.1.1) ---------------------------------------------
CIDER_TEST(inspect_real_llvm_built_binaries) {
    const fs::path bin = fs::path(CIDER_REAL_FIXTURES_DIR) / "bin";
    auto u = cider_run({"inspect", path_to_utf8(bin / "hello_universal")});
    CHECK_EQ(u.code, 0);
    CHECK(has(u, "Architecture: x86_64, arm64"));
    CHECK(has(u, "ad-hoc"));
    CHECK(has(u, "NOT verified"));
    auto j = cider_run({"inspect", path_to_utf8(bin / "app_x86_64"), "--json", "--symbols"});
    CHECK_EQ(j.code, 0);
    CHECK(JsonCheck(json_part(j.out)).valid());
    CHECK(has(j, "@rpath/libfoo.dylib"));
    CHECK(has(j, "\"kind\": \"weak\""));
    CHECK(has(j, "\"_main\""));
    CHECK(has(j, "@executable_path/../Frameworks"));
    auto o = cider_run({"inspect", path_to_utf8(bin / "hello_x86_64.o")});
    CHECK_EQ(o.code, 0);
    CHECK(has(o, "Type: object"));
}

CIDER_TEST(inspect_dmg_with_hdiutil_like_layout_has_no_noise) {
    if (!seven_zip_available()) SKIP("7-Zip not installed");
    TempDir t;
    DmgOptions o;
    o.real_layout = true;
    o.codec = DmgCodec::ZlibStored;
    o.partition_name = "disk image (Apple_HFS : 4)";
    auto img = t.write("layout.dmg", build_dmg(iso(), o).bytes);
    auto r = cider_run({"inspect", path_to_utf8(img), "--temp-dir", path_to_utf8(t.path())});
    CHECK_EQ(r.code, 0);
    CHECK(has(r, "Partitions: 5"));
    CHECK(has(r, "Applications found: 1"));
    CHECK(!has(r, "Warning:"));
}
