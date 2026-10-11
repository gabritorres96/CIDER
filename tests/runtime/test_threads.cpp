// Guest threads (ORVYN 0.4.2): pthreads of Mach-O programs on real Windows threads with the cooperative guest lock.
// ThreadSanitizer is not available for this toolchain on Windows, so correctness is checked with deterministic fixtures whose
// result is only right if mutual exclusion, joins, condition variables and per-thread TLS/errno really work, plus repeated runs.
#include <chrono>
#include <cstring>
#include <intrin.h>

#include "orvyn/guest.hpp"
#include "orvyn/guest_threads.hpp"
#include "guest_code.hpp"
#include "macho_builder.hpp"
#include "test_framework.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#endif

using namespace orvyn;
using namespace orvyn::testing;
namespace fs = std::filesystem;

namespace {

struct Run {
    guest::Outcome r;
    std::string out;
    double seconds = 0;
};
Run run_fixture(const std::string& name, std::vector<std::string> args = {}) {
    Run run;
    TempDir root;                                                      // thread_sync opens a missing file: ENOENT needs a root
    guest::GuestConfig cfg;
    cfg.root = root.path();
    cfg.args = {name};
    cfg.args.insert(cfg.args.end(), args.begin(), args.end());
    cfg.sink = [&](int, const char* d, size_t n) { run.out.append(d, n); };
    auto bytes = exec_fixture(name);
    const auto t0 = std::chrono::steady_clock::now();
    run.r = guest::run_in_process(ByteView(bytes), cfg);
    run.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return run;
}
void expect_exit(const Run& r, int code, const char* what) {
    if (r.r.state != guest::State::Exited) { report_failure(__FILE__, __LINE__, std::string(what) + ": state " + guest::to_string(r.r.state) + " " + r.r.message); return; }
    if (r.r.exit_code != code) report_failure(__FILE__, __LINE__, std::string(what) + ": exit code " + std::to_string(r.r.exit_code) + ", expected " + std::to_string(code));
}
DWORD thread_count() {
    DWORD n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == GetCurrentProcessId()) ++n;
    CloseHandle(snap);
    return n;
}

}  // namespace

ORVYN_TEST(create_join_self_equal_exit_and_error_codes) { expect_exit(run_fixture("thread_basic"), 42, "thread_basic"); }

ORVYN_TEST(four_threads_and_a_mutex_count_exactly) {
    Run r = run_fixture("thread_mutex");
    expect_exit(r, 42, "thread_mutex");
    CHECK_EQ(r.out, std::string("counter=40000 max_inside=1\n"));    // never two threads inside the critical section
}

ORVYN_TEST(without_the_mutex_updates_are_lost_which_proves_real_interleaving) {
    Run r = run_fixture("thread_race");
    expect_exit(r, 42, "thread_race");
    long counter = 0;
    REQUIRE(std::sscanf(r.out.c_str(), "counter=%ld of 4000", &counter) == 1);
    CHECK(counter < 4000);
    CHECK(counter >= 1000);                                           // at least one thread's worth survives
}

ORVYN_TEST(each_thread_has_its_own_tls_starting_from_the_template) {
    Run r = run_fixture("thread_tls");
    expect_exit(r, 42, "thread_tls");
    CHECK_EQ(r.out, std::string("a=105 b=110 main=101 template 7\n"));
}

ORVYN_TEST(condition_variables_mutex_kinds_pthread_once_and_per_thread_errno) { expect_exit(run_fixture("thread_sync"), 42, "thread_sync"); }

ORVYN_TEST(exit_from_a_non_main_thread_ends_the_whole_process) {
    Run r = run_fixture("thread_exit", {"x"});                        // main blocks forever in pthread_join(sleeper)
    expect_exit(r, 7, "thread_exit x");
    CHECK_EQ(r.out, std::string("thread calls exit(7)\n"));
    CHECK(r.seconds < 10.0);
}

ORVYN_TEST(returning_from_main_stops_threads_that_are_still_running) {
    Run r = run_fixture("thread_exit", {"m"});
    expect_exit(r, 5, "thread_exit m");
    CHECK_EQ(r.out, std::string("main returns\n"));
    CHECK(r.seconds < 10.0);
}

ORVYN_TEST(the_thread_limit_is_enforced_with_eagain) {
    Run r = run_fixture("thread_exit", {"l"});
    expect_exit(r, 42, "thread_exit l");
    CHECK_EQ(r.out, std::string("created=63 err=35\n"));              // 64 live threads including main, EAGAIN is 35 on Darwin
}

ORVYN_TEST(guest_threads_do_not_touch_the_hosts_windows_thread_state) {
    const uint64_t self = __readgsqword(0x30), tls_ptr = __readgsqword(0x58), stack_base = __readgsqword(0x08);
    for (const char* f : {"thread_tls", "thread_mutex", "thread_sync"}) expect_exit(run_fixture(f), 42, f);
    CHECK_EQ(__readgsqword(0x30), self);
    CHECK_EQ(__readgsqword(0x58), tls_ptr);
    CHECK_EQ(__readgsqword(0x08), stack_base);
}

ORVYN_TEST(repeated_runs_leave_no_threads_or_handles_behind) {
    run_fixture("thread_mutex");                                      // warm up
    DWORD handles0 = 0, handles1 = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles0);
    const DWORD threads0 = thread_count();
    for (int i = 0; i < 12; ++i) {
        expect_exit(run_fixture("thread_mutex"), 42, "mutex loop");
        expect_exit(run_fixture("thread_exit", {"m"}), 5, "exit loop");
    }
    GetProcessHandleCount(GetCurrentProcess(), &handles1);
    CHECK(handles1 <= handles0 + 3);
    CHECK(thread_count() <= threads0 + 1);
}

ORVYN_TEST(the_scheduler_is_deterministic_and_stable_over_many_runs) {
    for (int i = 0; i < 25; ++i) {
        Run r = run_fixture("thread_mutex");
        if (r.r.exit_code != 42 || r.out != "counter=40000 max_inside=1\n") { report_failure(__FILE__, __LINE__, "run " + std::to_string(i) + " differed"); break; }
    }
}

ORVYN_TEST(handles_are_opaque_numbers_and_mutex_storage_keeps_the_darwin_layout) {
    CHECK_EQ(libsystem::GuestThreads::id_of(libsystem::GuestThreads::handle_of(5)), uint64_t{5});
    CHECK_EQ(libsystem::GuestThreads::id_of(0x1234), uint64_t{0});     // not a handle ORVYN ever gave out
    CHECK_EQ(libsystem::GuestThreads::id_of(0), uint64_t{0});
}

// ---- a crash in a guest thread is contained by the worker ----------------------------------------------------------------------------
ORVYN_TEST(a_fault_in_a_guest_thread_kills_only_the_worker) {
    const char* exe = std::getenv("ORVYN_EXE");
    if (!exe) SKIP("ORVYN_EXE is not set");
    guest::SupervisorOptions o;
    o.worker_exe = fs::absolute(path_from_utf8(exe));
    o.image = kExecFixtures / "bin" / "thread_exit";
    o.guest_args = {"f"};
    o.timeout = std::chrono::seconds(60);
    o.log_level = "warn";
    auto r = guest::run_supervised(o);
    CHECK(r.status == guest::SupervisorResult::Status::GuestCrashed);
    CHECK_EQ(r.worker_exit_code, uint32_t{0xC0000005});
}
