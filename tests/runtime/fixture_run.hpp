#pragma once
// Shared helper of the Objective-C fixture suites: runs a compiled Mach-O program in-process and captures its stdout/stderr.
#include <filesystem>

#include "cider/guest.hpp"
#include "guest_code.hpp"
#include "test_framework.hpp"

namespace cider::testing {

struct FixtureRun {
    guest::Outcome r;
    std::string out, err;
};

inline std::filesystem::path objc_fixture(const std::string& scenario) {
    return std::filesystem::path(CIDER_OBJC_FIXTURES_DIR) / "bin" / scenario / scenario;
}

inline FixtureRun run_fixture_file(const std::filesystem::path& exe, std::vector<std::string> args = {}, unsigned timeout_s = 0) {
    FixtureRun run;
    guest::GuestConfig cfg;
    cfg.args = {cider::path_to_utf8(exe)};
    cfg.args.insert(cfg.args.end(), args.begin(), args.end());
    cfg.image_path = exe;
    cfg.sink = [&](int fd, const char* d, size_t n) { (fd == 2 ? run.err : run.out).append(d, n); };
    (void)timeout_s;
    auto b = cider::read_file(exe, 64u << 20);
    if (!b.ok()) { report_failure(__FILE__, __LINE__, "fixture missing: " + cider::path_to_utf8(exe)); throw AbortTest{}; }
    run.r = guest::run_in_process(ByteView(b.value()), cfg);
    return run;
}

inline void expect_exit_code(const FixtureRun& r, int code, const char* what) {
    if (r.r.state != guest::State::Exited) { report_failure(__FILE__, __LINE__, std::string(what) + ": state " + guest::to_string(r.r.state) + " " + r.r.message + " | stderr: " + r.err); return; }
    if (r.r.exit_code != code) report_failure(__FILE__, __LINE__, std::string(what) + ": exit code " + std::to_string(r.r.exit_code) + ", expected " + std::to_string(code) + " | stderr: " + r.err);
}

}  // namespace cider::testing
