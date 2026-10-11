#pragma once
// Shared helper of the Objective-C fixture suites: runs a compiled Mach-O program in-process and captures its stdout/stderr.
#include <filesystem>

#include "orvyn/guest.hpp"
#include "guest_code.hpp"
#include "test_framework.hpp"

namespace orvyn::testing {

struct FixtureRun {
    guest::Outcome r;
    std::string out, err;
};

inline std::filesystem::path appkit_fixture(const std::string& scenario) {
    return std::filesystem::path(ORVYN_APPKIT_FIXTURES_DIR) / "bin" / scenario / scenario;
}
inline std::filesystem::path objc_fixture(const std::string& scenario) {
    return std::filesystem::path(ORVYN_OBJC_FIXTURES_DIR) / "bin" / scenario / scenario;
}

struct FixtureOptions {
    std::vector<std::string> args;
    std::filesystem::path root;
    std::string gui_backend;                  // "" = default (win32); tests use "offscreen"
    std::vector<std::string> gui_script;
};

inline FixtureRun run_fixture_file(const std::filesystem::path& exe, const FixtureOptions& opt) {
    FixtureRun run;
    guest::GuestConfig cfg;
    if (!opt.gui_backend.empty()) cfg.gui_backend = opt.gui_backend;
    cfg.gui_script = opt.gui_script;
    cfg.root = opt.root;
    std::vector<std::string> args = opt.args;
    cfg.args = {orvyn::path_to_utf8(exe)};
    cfg.args.insert(cfg.args.end(), args.begin(), args.end());
    cfg.image_path = exe;
    cfg.sink = [&](int fd, const char* d, size_t n) { (fd == 2 ? run.err : run.out).append(d, n); };
    auto b = orvyn::read_file(exe, 64u << 20);
    if (!b.ok()) { report_failure(__FILE__, __LINE__, "fixture missing: " + orvyn::path_to_utf8(exe)); throw AbortTest{}; }
    run.r = guest::run_in_process(ByteView(b.value()), cfg);
    return run;
}

inline FixtureRun run_fixture_file(const std::filesystem::path& exe, std::vector<std::string> args = {}, const std::filesystem::path& root = {}) {
    FixtureOptions o;
    o.args = std::move(args);
    o.root = root;
    return run_fixture_file(exe, o);
}

inline void expect_exit_code(const FixtureRun& r, int code, const char* what) {
    if (r.r.state != guest::State::Exited) { report_failure(__FILE__, __LINE__, std::string(what) + ": state " + guest::to_string(r.r.state) + " " + r.r.message + " | stderr: " + r.err); return; }
    if (r.r.exit_code != code) report_failure(__FILE__, __LINE__, std::string(what) + ": exit code " + std::to_string(r.r.exit_code) + ", expected " + std::to_string(code) + " | stderr: " + r.err);
}

}  // namespace orvyn::testing
