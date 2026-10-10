// Failure paths: load failures (in-process, nothing runs) and guest failures (worker process: the
// supervisor survives crashes, timeouts and resource limits).
#include <algorithm>
#include <chrono>
#include <cstring>

#include "cider/guest.hpp"
#include "cider/process.hpp"
#include "guest_code.hpp"
#include "macho_builder.hpp"
#include "test_framework.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace cider;
using namespace cider::testing;
namespace fs = std::filesystem;

namespace {

guest::Outcome load_only(const std::vector<uint8_t>& bytes, std::string* out = nullptr) {
    guest::GuestConfig cfg;
    cfg.args = {"x"};
    std::string captured;
    cfg.sink = [&](int, const char* d, size_t n) { captured.append(d, n); };
    auto r = guest::run_in_process(ByteView(bytes), cfg);
    if (out) *out = captured;
    return r;
}

bool range_free(uint64_t a) { return memory::query_address(reinterpret_cast<void*>(a)).state == memory::PageInfo::State::Free; }

guest::SupervisorOptions supervised(const std::string& fixture_name) {
    const char* exe = std::getenv("CIDER_EXE");
    if (!exe) { report_failure(__FILE__, __LINE__, "CIDER_EXE is not set"); throw AbortTest{}; }
    guest::SupervisorOptions o;
    o.worker_exe = fs::absolute(path_from_utf8(exe));
    o.image = kExecFixtures / "bin" / fixture_name;
    o.timeout = std::chrono::seconds(60);
    o.log_level = "warn";
    return o;
}

}  // namespace

// ---- load failures: the guest never runs ---------------------------------------------------------------------------------------
CIDER_TEST(unresolved_symbol_aborts_before_execution) {
    std::string out;
    auto r = load_only(exec_fixture("missing"), &out);
    CHECK(r.state == guest::State::LoadFailed);
    CHECK(r.load_failure == loader::LoadFailure::UnresolvedSymbol);
    REQUIRE_EQ(r.unresolved.size(), size_t{1});
    CHECK_EQ(r.unresolved[0].symbol, std::string("_fork"));
    CHECK_EQ(r.unresolved[0].library, std::string("/usr/lib/libSystem.B.dylib"));
    CHECK(r.message.find("_fork") != std::string::npos);
    CHECK_EQ(r.entry_address, uint64_t{0});                  // control was never transferred
    CHECK_EQ(out, std::string(""));
    CHECK(range_free(0x100000000ull));
}

CIDER_TEST(incompatible_files_are_rejected_with_specific_reasons) {
    auto real = [](const char* n) { return read_file(fs::path(CIDER_REAL_FIXTURES_DIR) / "bin" / n, 1 << 20).value(); };
    struct Case { std::vector<uint8_t> bytes; loader::LoadFailure kind; const char* needle; };
    Case cases[] = {
        {{}, loader::LoadFailure::Incompatible, "not a valid Mach-O"},
        {std::vector<uint8_t>(500, 0x41), loader::LoadFailure::Incompatible, "not a valid Mach-O"},
        {real("hello_arm64"), loader::LoadFailure::Incompatible, "no x86_64 slice"},
        {real("libfoo.dylib"), loader::LoadFailure::Incompatible, "not an executable"},
        {real("hello_x86_64.o"), loader::LoadFailure::Incompatible, "not an executable"},
    };
    for (auto& c : cases) {
        auto r = load_only(c.bytes);
        if (r.state != guest::State::LoadFailed) { report_failure(__FILE__, __LINE__, std::string("accepted: ") + c.needle); continue; }
        CHECK(r.load_failure == c.kind);
        if (r.message.find(c.needle) == std::string::npos) report_failure(__FILE__, __LINE__, "message was: " + r.message);
    }
    CHECK(range_free(0x100000000ull));
}

CIDER_TEST(missing_library_dependency_is_reported_by_name) {
    MachoSpec s;
    s.dylibs = {"/usr/lib/libSystem.B.dylib", "/usr/lib/libz.1.dylib", "@rpath/libfoo.dylib"};
    auto r = load_only(build_macho(s).bytes);
    CHECK(r.state == guest::State::LoadFailed);
    CHECK(r.load_failure == loader::LoadFailure::MissingDependency);
    CHECK(r.message.find("libz.1.dylib") != std::string::npos);
    CHECK(r.message.find("libfoo.dylib") != std::string::npos);
    CHECK_EQ(r.required_libs.size(), size_t{3});
}

CIDER_TEST(unsupported_features_are_refused_explicitly) {
    auto built = build_macho({});
    auto b = built.bytes;
    patch32(b, built.cmd["text"] + 72 + 64, 0x15);           // __text section flags: thread-local init functions (0x11-0x14 are served since 0.3.6)
    auto r = load_only(b);
    CHECK(r.state == guest::State::LoadFailed);
    CHECK(r.load_failure == loader::LoadFailure::Unsupported);
    CHECK(r.message.find("thread-local") != std::string::npos);

    MachoSpec u;
    u.main = false;
    u.unixthread = true;
    auto r2 = load_only(build_macho(u).bytes);
    CHECK(r2.state == guest::State::LoadFailed);
    CHECK(r2.message.find("LC_UNIXTHREAD") != std::string::npos);

    MachoSpec ios;
    ios.platform = 2;
    CHECK(load_only(build_macho(ios).bytes).state == guest::State::LoadFailed);
    MachoSpec enc;
    enc.encrypted = true;
    CHECK(load_only(build_macho(enc).bytes).state == guest::State::LoadFailed);
    MachoSpec dyl;
    dyl.filetype = 6;
    dyl.main = false;
    dyl.id_dylib = "@rpath/x.dylib";
    CHECK(load_only(build_macho(dyl).bytes).state == guest::State::LoadFailed);
    CHECK(range_free(0x100000000ull));
}

CIDER_TEST(corrupt_binaries_fail_cleanly_and_never_execute) {
    auto good = exec_fixture("hello");
    int failed = 0;
    for (size_t cut = 0; cut < 4096; cut += 37) {
        std::vector<uint8_t> c(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
        std::string out;
        auto r = load_only(c, &out);
        failed += r.state == guest::State::LoadFailed;
        CHECK_EQ(out, std::string(""));                      // a truncated file never produces guest output
    }
    CHECK(failed > 50);
    CHECK(range_free(0x100000000ull));
}

// ---- result protocol ------------------------------------------------------------------------------------------------------------------
CIDER_TEST(worker_result_round_trips_including_hostile_text) {
    guest::Outcome o;
    o.state = guest::State::LoadFailed;
    o.exit_code = -3;
    o.load_failure = loader::LoadFailure::UnresolvedSymbol;
    o.message = "line1\nline2 | 100% \r done";
    o.required_libs = {"/usr/lib/libSystem.B.dylib", "weird|name\n"};
    o.unresolved = {{"_fork", "/usr/lib/libSystem.B.dylib", false, "not|implemented\n"}};
    o.entry_address = 0x1000002d0;
    o.image_base = 0x100000000;
    auto p = guest::parse_result(guest::serialize_result(o));
    REQUIRE(p.has_value());
    CHECK(p->state == o.state);
    CHECK_EQ(p->exit_code, -3);
    CHECK(p->load_failure == o.load_failure);
    CHECK_EQ(p->message, o.message);
    CHECK((p->required_libs == o.required_libs));
    REQUIRE_EQ(p->unresolved.size(), size_t{1});
    CHECK_EQ(p->unresolved[0].reason, std::string("not|implemented\n"));
    CHECK_EQ(p->entry_address, o.entry_address);
}

CIDER_TEST(truncated_or_forged_result_files_are_rejected) {
    guest::Outcome o;
    o.state = guest::State::Exited;
    o.exit_code = 42;
    std::string text = guest::serialize_result(o);
    CHECK(guest::parse_result(text).has_value());
    for (size_t cut = 0; cut + 1 < text.size(); cut += 3) CHECK(!guest::parse_result(text.substr(0, cut)).has_value());   // worker died mid-write
    CHECK(!guest::parse_result("").has_value());
    CHECK(!guest::parse_result("cider-result=2\nstate=exited\nend\n").has_value());
    CHECK(!guest::parse_result("cider-result=1\nstate=bogus\nend\n").has_value());
    CHECK(!guest::parse_result("cider-result=1\nexit_code=abc\nstate=exited\nend\n").has_value());
    CHECK(!guest::parse_result("cider-result=1\nstate=exited\ngarbage line\nend\n").has_value());
}

// ---- worker process ---------------------------------------------------------------------------------------------------------------------
CIDER_TEST(worker_reports_the_guest_exit_code_and_captures_output) {
    auto o = supervised("hello");
    o.capture_output = true;
    auto r = guest::run_supervised(o);
    CHECK(r.status == guest::SupervisorResult::Status::Completed);
    CHECK(r.guest.state == guest::State::Exited);
    CHECK_EQ(r.guest.exit_code, 0);
    CHECK_EQ(r.guest_stdout, std::string("Hello from macOS!\nsecond line via write\n"));
    CHECK_EQ(r.guest_stderr, std::string(""));
    CHECK_EQ(r.worker_exit_code, 0u);

    o = supervised("ret42");
    CHECK_EQ(guest::run_supervised(o).guest.exit_code, 42);
    o = supervised("ret7");
    CHECK_EQ(guest::run_supervised(o).guest.exit_code, 7);
}

CIDER_TEST(guest_runs_in_a_separate_process) {
    auto o = supervised("pid");
    o.capture_output = true;
    auto r = guest::run_supervised(o);
    REQUIRE(r.status == guest::SupervisorResult::Status::Completed);
    REQUIRE_EQ(r.guest_stdout.size(), size_t{4});
    int32_t pid;
    std::memcpy(&pid, r.guest_stdout.data(), 4);
    CHECK(pid > 0);
#ifdef _WIN32
    CHECK(static_cast<uint32_t>(pid) != GetCurrentProcessId());   // getpid() inside the guest is the worker's pid
#endif
}

CIDER_TEST(arguments_reach_the_guest_through_the_worker) {
    auto o = supervised("args");
    o.guest_args = {"hello world", "second"};
    o.capture_output = true;
    auto r = guest::run_supervised(o);
    CHECK(r.status == guest::SupervisorResult::Status::Completed);
    CHECK_EQ(r.guest_stdout, std::string("hello world\n"));       // quoting of arguments with spaces survives the process boundary
    CHECK_EQ(r.guest.exit_code, 31);
}

CIDER_TEST(worker_reports_load_failures_without_running_the_guest) {
    auto r = guest::run_supervised(supervised("missing"));
    CHECK(r.status == guest::SupervisorResult::Status::LoadFailed);
    CHECK(r.guest.load_failure == loader::LoadFailure::UnresolvedSymbol);
    REQUIRE_EQ(r.guest.unresolved.size(), size_t{1});
    CHECK_EQ(r.guest.unresolved[0].symbol, std::string("_fork"));
    CHECK_EQ(r.worker_exit_code, 0u);
}

CIDER_TEST(a_guest_crash_kills_only_the_worker) {
    auto r = guest::run_supervised(supervised("crash"));
    CHECK(r.status == guest::SupervisorResult::Status::GuestCrashed);
    CHECK_EQ(r.worker_exit_code, 0xC0000005u);                    // access violation (write to address 0)
    CHECK(r.message.find("access violation") != std::string::npos);
    // CIDER itself is still alive and can run another program right away
    CHECK_EQ(guest::run_supervised(supervised("ret42")).guest.exit_code, 42);
}

CIDER_TEST(an_illegal_instruction_in_the_guest_is_a_contained_crash) {
    // ud2 patched over the start of main: the CPU raises #UD inside the Mach-O code
    auto b = exec_fixture("ret42");
    const std::vector<uint8_t> needle = {0xB8, 0x2A, 0x00, 0x00, 0x00};     // mov eax, 42
    auto it = std::search(b.begin(), b.end(), needle.begin(), needle.end());
    REQUIRE(it != b.end());
    *it = 0x0F; *(it + 1) = 0x0B;
    TempDir t;
    auto path = t.write("ud2.macho", b);
    auto o = supervised("ret42");
    o.image = path;
    auto r = guest::run_supervised(o);
    CHECK(r.status == guest::SupervisorResult::Status::GuestCrashed);
    CHECK_EQ(r.worker_exit_code, 0xC000001Du);
    CHECK(r.message.find("illegal instruction") != std::string::npos);
}

CIDER_TEST(guest_stack_overflow_hits_the_guard_page_and_is_contained) {
    auto r = guest::run_supervised(supervised("recurse"));
    CHECK(r.status == guest::SupervisorResult::Status::GuestCrashed);
    CHECK(r.worker_exit_code == 0xC0000005u || r.worker_exit_code == 0xC00000FDu);
}

CIDER_TEST(a_guest_that_never_finishes_is_killed_at_the_timeout) {
    auto o = supervised("spin");
    o.timeout = std::chrono::milliseconds(1500);
    auto t0 = std::chrono::steady_clock::now();
    auto r = guest::run_supervised(o);
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.status == guest::SupervisorResult::Status::Timeout);
    CHECK(dt >= 1400 && dt < 15000);
    CHECK(r.message.find("did not finish") != std::string::npos);
}

CIDER_TEST(job_memory_limit_is_enforced_on_the_worker) {
    auto o = supervised("hog");                                    // asks for 1.5 GiB
    o.memory_limit = 1ull << 30;                                   // 1 GiB per process
    auto r = guest::run_supervised(o);
    REQUIRE(r.status == guest::SupervisorResult::Status::Completed);
    CHECK_EQ(r.guest.exit_code, 3);                                // malloc failed: the limit held
}

CIDER_TEST(worker_with_a_missing_image_reports_a_load_failure) {
    auto o = supervised("ret42");
    o.image = kExecFixtures / "bin" / "does-not-exist";
    auto r = guest::run_supervised(o);
    CHECK(r.status == guest::SupervisorResult::Status::LoadFailed);
    CHECK(r.message.find("not found") != std::string::npos);
}

CIDER_TEST(an_unusable_worker_executable_is_an_internal_error) {
    auto o = supervised("ret42");
    o.worker_exe = kExecFixtures / "bin" / "ret42";                // a Mach-O, not a Windows program
    auto r = guest::run_supervised(o);
    CHECK(r.status == guest::SupervisorResult::Status::InternalError);
    o.worker_exe = fs::path("relative.exe");
    CHECK(guest::run_supervised(o).status == guest::SupervisorResult::Status::InternalError);
}
