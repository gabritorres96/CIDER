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
    CHECK(has(v, "cider 0.2.0"));
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

// ---- cider run (CIDER 0.2): real execution of Mach-O x86_64 programs ------------------------------------------------------
namespace {
fs::path exec_bin(const char* n) { return fs::path(CIDER_EXEC_FIXTURES_DIR) / "bin" / n; }
}

CIDER_TEST(run_executes_a_real_macho_and_reports_the_guest_exit_code) {
    auto r = cider_run({"run", path_to_utf8(exec_bin("ret42"))});
    CHECK_EQ(r.code, 0);                                        // CIDER's own status: the guest ran to completion
    CHECK(has(r, "Guest exit code: 42"));
    CHECK(has(r, "[CIDER][LOADER] Mach-O x86_64 detected"));
    CHECK(has(r, "[CIDER][MEMORY] mapping __TEXT"));
    CHECK(has(r, "[CIDER][ABI] preparing the guest execution environment"));
    CHECK(has(r, "[CIDER][EXECUTION] entering guest code"));
    CHECK(has(r, "[CIDER][EXECUTION] guest returned 42"));
    CHECK(has(r, "[CIDER][PROCESS] cleanup completed"));
    CHECK(has(cider_run({"run", path_to_utf8(exec_bin("ret7"))}), "Guest exit code: 7"));
    CHECK(has(cider_run({"run", path_to_utf8(exec_bin("ret99"))}), "Guest exit code: 99"));
}

CIDER_TEST(run_does_not_depend_on_the_file_name) {
    TempDir t;
    auto copy = t.write("ret99", std::vector<uint8_t>(read_file(exec_bin("ret42"), 1 << 20).value()));   // ret42's bytes under ret99's name
    auto r = cider_run({"run", path_to_utf8(copy), "--log-level", "off"});
    CHECK(has(r, "Guest exit code: 42"));
}

CIDER_TEST(run_shows_real_guest_output_and_passes_arguments) {
    auto h = cider_run({"run", path_to_utf8(exec_bin("hello")), "--log-level", "warn"});
    CHECK_EQ(h.code, 0);
    CHECK(has(h, "Hello from macOS!"));
    CHECK(has(h, "second line via write"));
    CHECK(has(h, "Guest exit code: 0"));
    auto a = cider_run({"run", path_to_utf8(exec_bin("args")), "--log-level", "warn", "--", "first arg", "second"});
    CHECK(has(a, "first arg"));
    CHECK(has(a, "Guest exit code: 31"));
}

CIDER_TEST(run_log_level_controls_the_runtime_messages) {
    auto quiet = cider_run({"run", path_to_utf8(exec_bin("ret42")), "--log-level", "warn"});
    CHECK(!has(quiet, "[CIDER][LOADER]"));
    CHECK(has(quiet, "Guest exit code: 42"));
    auto verbose = cider_run({"run", path_to_utf8(exec_bin("calls")), "--verbose"});
    CHECK(has(verbose, "[CIDER][LOADER][DEBUG]") || has(verbose, "[CIDER][DYLD]"));
    CHECK(has(verbose, "[CIDER][ABI][DEBUG] entering guest function"));
    CHECK(has(verbose, "Guest exit code: 67"));
}

CIDER_TEST(run_json_separates_runtime_status_from_the_guest_result) {
    auto r = cider_run({"run", path_to_utf8(exec_bin("hello")), "--json"});
    CHECK_EQ(r.code, 0);
    CHECK(JsonCheck(json_part(r.out)).valid());
    CHECK(has(r, "\"command\": \"run\""));
    CHECK(has(r, "\"status\": \"completed\""));
    CHECK(has(r, "\"state\": \"exited\""));
    CHECK(has(r, "\"exit_code\": 0"));
    CHECK(has(r, "\"stdout\": \"Hello from macOS!\\nsecond line via write\\n\""));
    CHECK(has(r, "\"cider_exit_code\": 0"));
    CHECK(has(r, "\"required_libraries\""));
    CHECK(has(r, "[CIDER][LOADER] Mach-O x86_64 detected"));     // runtime log lines are inside the "log" array

    auto m = cider_run({"run", path_to_utf8(exec_bin("ret42")), "--json"});
    CHECK(has(m, "\"exit_code\": 42"));
    CHECK(has(m, "\"exit_status\": 42"));
    CHECK(has(m, "\"returned_from_main\": true"));
}

CIDER_TEST(run_unresolved_symbol_exit_code_and_diagnostics) {
    auto r = cider_run({"run", path_to_utf8(exec_bin("missing"))});
    CHECK_EQ(r.code, 12);
    CHECK(has(r, "[CIDER][ERROR] Unresolved symbol: _fork"));
    CHECK(has(r, "[CIDER][ERROR] Required library: /usr/lib/libSystem.B.dylib"));
    CHECK(has(r, "[CIDER][ERROR] Execution aborted"));
    CHECK(!has(r, "Guest exit code"));
    auto j = cider_run({"run", path_to_utf8(exec_bin("missing")), "--json"});
    CHECK_EQ(j.code, 12);
    CHECK(JsonCheck(json_part(j.out)).valid());
    CHECK(has(j, "\"status\": \"load-failed\""));
    CHECK(has(j, "\"load_failure\": \"unresolved-symbol\""));
    CHECK(has(j, "\"symbol\": \"_fork\""));
    CHECK(has(j, "\"state\": \"load-failed\""));
}

CIDER_TEST(run_incompatible_files_have_their_own_exit_code) {
    TempDir t;
    MachoSpec dyl;
    dyl.dylibs = {"/usr/lib/libSystem.B.dylib", "@rpath/libfoo.dylib"};
    auto dep = t.write("dep", build_macho(dyl).bytes);
    auto rd = cider_run({"run", path_to_utf8(dep), "--log-level", "off"});
    CHECK_EQ(rd.code, 12);                                       // dependency not available
    auto arm = cider_run({"run", path_to_utf8(fs::path(CIDER_REAL_FIXTURES_DIR) / "bin" / "hello_arm64")});
    CHECK_EQ(arm.code, 11);
    CHECK(has(arm, "no usable x86_64 slice"));
    CHECK_EQ(cider_run({"run", path_to_utf8(fs::path(CIDER_REAL_FIXTURES_DIR) / "bin" / "libfoo.dylib")}).code, 11);
    CHECK_EQ(cider_run({"run", path_to_utf8(fs::path(CIDER_REAL_FIXTURES_DIR) / "bin" / "hello_chained_x86_64")}).code, 11);
    auto junk = t.write("junk", std::vector<uint8_t>(300, 0x41));
    CHECK_EQ(cider_run({"run", path_to_utf8(junk)}).code, 11);
    CHECK_EQ(cider_run({"run", path_to_utf8(t.path() / "missing")}).code, 11);
    CHECK_EQ(cider_run({"run"}).code, 2);                        // usage error keeps its old code
    CHECK_EQ(cider_run({"run", path_to_utf8(exec_bin("ret42")), "--timeout", "0"}).code, 2);
}

CIDER_TEST(run_guest_crash_is_exit_14_and_cider_survives) {
    auto r = cider_run({"run", path_to_utf8(exec_bin("crash")), "--log-level", "warn"});
    CHECK_EQ(r.code, 14);
    CHECK(has(r, "access violation"));
    CHECK(has(r, "Execution aborted"));
    auto j = cider_run({"run", path_to_utf8(exec_bin("crash")), "--json"});
    CHECK_EQ(j.code, 14);
    CHECK(JsonCheck(json_part(j.out)).valid());
    CHECK(has(j, "\"status\": \"guest-crashed\""));
    CHECK(has(j, "\"worker_exit_code\": 3221225477"));          // 0xC0000005
    CHECK(has(j, "\"exit_code\": null"));                         // there is no guest exit code
}

CIDER_TEST(run_timeout_kills_the_guest) {
    auto r = cider_run({"run", path_to_utf8(exec_bin("spin")), "--timeout", "2", "--log-level", "warn"});
    CHECK_EQ(r.code, 14);
    CHECK(has(r, "did not finish within 2 s"));
}

CIDER_TEST(run_an_application_bundle) {
    TempDir t;
    auto app = make_app(t, "Hello.app", build_macho({}).bytes, "Example");     // synthetic loadable Mach-O returning 42
    auto r = cider_run({"run", path_to_utf8(app), "--log-level", "warn"});
    CHECK_EQ(r.code, 0);
    CHECK(has(r, "Guest exit code: 42"));
}

// ---- regression (0.2.0 -> 0.3.0): output of the guest must reach a real console ----------------------------------------------
// `cider run hello` printed nothing when stdout was a console (the worker was created with CREATE_NO_WINDOW, which
// gave it a fresh hidden console and invalidated the inherited console handles). Pipe-based tests could not see it.
// This test runs cider.exe attached to a genuine pseudo console (ConPTY), exactly like a terminal window.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
std::optional<std::string> run_in_pseudo_console(const std::wstring& cmdline) {
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr;
    // A pseudo console child inherits the PARENT's redirected std handles when the parent owns no console.
    DWORD pids[2];
    if (GetConsoleProcessList(pids, 2) == 0) AllocConsole();
    if (!CreatePipe(&in_r, &in_w, nullptr, 0) || !CreatePipe(&out_r, &out_w, nullptr, 0)) return std::nullopt;
    HPCON pc = nullptr;
    if (FAILED(CreatePseudoConsole({120, 40}, in_r, out_w, 0, &pc))) return std::nullopt;
    CloseHandle(in_r);
    CloseHandle(out_w);
    SIZE_T sz = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &sz);
    std::vector<uint8_t> attr(sz);
    auto* al = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr.data());
    InitializeProcThreadAttributeList(al, 1, 0, &sz);
    UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pc, sizeof pc, nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.lpAttributeList = al;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;
    // The child must not see this process's (possibly redirected) std handles, only the pseudo console.
    const HANDLE old_in = GetStdHandle(STD_INPUT_HANDLE), old_out = GetStdHandle(STD_OUTPUT_HANDLE), old_err = GetStdHandle(STD_ERROR_HANDLE);
    SetStdHandle(STD_INPUT_HANDLE, nullptr);
    SetStdHandle(STD_OUTPUT_HANDLE, nullptr);
    SetStdHandle(STD_ERROR_HANDLE, nullptr);
    const BOOL created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &si.StartupInfo, &pi);
    SetStdHandle(STD_INPUT_HANDLE, old_in);
    SetStdHandle(STD_OUTPUT_HANDLE, old_out);
    SetStdHandle(STD_ERROR_HANDLE, old_err);
    if (!created) return std::nullopt;
    std::string captured;
    // Drain on this thread while the child runs; ClosePseudoConsole afterwards flushes the remaining output.
    auto drain = [&](DWORD ms) {
        const ULONGLONG end = GetTickCount64() + ms;
        while (GetTickCount64() < end) {
            DWORD avail = 0;
            if (!PeekNamedPipe(out_r, nullptr, 0, nullptr, &avail, nullptr)) return;
            if (avail == 0) { Sleep(20); continue; }
            char buf[4096];
            DWORD got = 0;
            if (!ReadFile(out_r, buf, sizeof buf, &got, nullptr) || got == 0) return;
            captured.append(buf, got);
        }
    };
    const ULONGLONG deadline = GetTickCount64() + 60000;
    while (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT && GetTickCount64() < deadline) drain(50);
    drain(500);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ClosePseudoConsole(pc);
    drain(200);
    CloseHandle(in_w);
    CloseHandle(out_r);
    DeleteProcThreadAttributeList(al);
    return captured;
}
}  // namespace

CIDER_TEST(run_guest_output_reaches_a_real_console) {
    const char* exe = std::getenv("CIDER_EXE");
    REQUIRE(exe != nullptr);
    std::wstring cmd = L"\"" + fs::absolute(path_from_utf8(exe)).wstring() + L"\" run \"" + exec_bin("hello").wstring() +
                       L"\" --log-level warn";
    auto out = run_in_pseudo_console(cmd);
    if (!out) SKIP("pseudo console not available in this session");
    CHECK(out->find("Hello from macOS!") != std::string::npos);
    CHECK(out->find("second line via write") != std::string::npos);
    CHECK(out->find("Guest exit code: 0") != std::string::npos);
}
#endif

// ---- cider run (CIDER 0.3): dynamic libraries ----------------------------------------------------------------------------
namespace {
fs::path dylib_bin(const char* scenario) { return fs::path(CIDER_DYLIB_FIXTURES_DIR) / "bin" / scenario / scenario; }
}

CIDER_TEST(run_executable_with_a_dylib_logs_the_dynamic_linking_steps) {
    auto r = cider_run({"run", path_to_utf8(dylib_bin("one")), "--verbose"});
    CHECK_EQ(r.code, 0);
    CHECK(has(r, "[CIDER][DYLD] Loading main executable"));
    CHECK(has(r, "[CIDER][DYLD] Loading libmath.dylib"));
    CHECK(has(r, "[CIDER][DYLD] Dependencies resolved"));
    CHECK(has(r, "Resolving _cider_add"));
    CHECK(has(r, "Binding _cider_add"));
    CHECK(has(r, "[CIDER][DYLD] Running initializers"));
    CHECK(has(r, "[CIDER][EXECUTION] Guest started"));
    CHECK(has(r, "[CIDER][EXECUTION] guest returned 42"));
    CHECK(has(r, "Guest exit code: 42"));
    // the log lines appear in the order the linker really did the work
    CHECK(r.out.find("Loading libmath.dylib") < r.out.find("Binding _cider_add"));
    CHECK(r.out.find("Binding _cider_add") < r.out.find("Running initializers"));
    CHECK(r.out.find("Running initializers") < r.out.find("Guest started"));
}

CIDER_TEST(run_json_lists_the_loaded_images) {
    auto r = cider_run({"run", path_to_utf8(dylib_bin("trans")), "--json"});
    CHECK_EQ(r.code, 0);
    std::string json = r.out.substr(r.out.find('{'));
    CHECK(JsonCheck(json).valid());
    CHECK(has(r, "\"exit_code\": 42"));
    CHECK(has(r, "\"name\": \"@rpath/libtransa.dylib\""));
    CHECK(has(r, "\"name\": \"@rpath/libtransb.dylib\""));
    CHECK(has(r, "\"initializers_run\": 0"));
}

CIDER_TEST(run_missing_library_exit_12_names_the_library) {
    auto r = cider_run({"run", path_to_utf8(dylib_bin("missing")), "--log-level", "warn"});
    CHECK_EQ(r.code, 12);
    CHECK(has(r, "Missing library: @rpath/libgone.dylib"));
    CHECK(!has(r, "Guest exit code"));
    auto j = cider_run({"run", path_to_utf8(dylib_bin("missing")), "--json"});
    CHECK_EQ(j.code, 12);
    CHECK(JsonCheck(j.out.substr(j.out.find('{'))).valid());
    CHECK(has(j, "\"load_failure\": \"missing-dependency\""));
    CHECK(has(j, "\"missing_library\": \"@rpath/libgone.dylib\""));
}

CIDER_TEST(run_initializer_that_exits_and_initializer_that_crashes) {
    auto e = cider_run({"run", path_to_utf8(dylib_bin("initexit")), "--log-level", "warn"});
    CHECK_EQ(e.code, 0);
    CHECK(has(e, "Guest exit code: 7"));                         // the initializer's exit(7); main (42) never ran
    auto c = cider_run({"run", path_to_utf8(dylib_bin("initcrash")), "--log-level", "warn"});
    CHECK_EQ(c.code, 14);                                        // the worker died, CIDER survived
    CHECK(has(c, "0xc0000005"));
}

CIDER_TEST(run_cycles_are_tolerated_and_reported) {
    auto r = cider_run({"run", path_to_utf8(dylib_bin("cycle")), "--log-level", "warn"});
    CHECK_EQ(r.code, 0);
    CHECK(has(r, "dependency cycle"));
    CHECK(has(r, "Guest exit code: 42"));
}
