// ORVYN 0.3 Darwin runtime: the printf formatter, stdio, the file-descriptor layer, time and process identity,
// exercised both directly (formatter with fake argument readers) and through real Mach-O fixtures that were compiled
// by clang and call the functions through ORVYN's thunks. Output is compared byte for byte.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>

#include "orvyn/guest.hpp"
#include "orvyn/printf_format.hpp"
#include "guest_code.hpp"
#include "macho_builder.hpp"
#include "test_framework.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace orvyn;
using namespace orvyn::testing;
namespace fs = std::filesystem;

namespace {

// ---- formatter tests: arguments come from a queue, so no machine code is involved ----------------------------------
struct FakeArgs final : abi::ArgReader {
    std::deque<uint64_t> ints;
    std::deque<double> doubles;
    uint64_t next_int() override { uint64_t v = ints.front(); ints.pop_front(); return v; }
    double next_double() override { double v = doubles.front(); doubles.pop_front(); return v; }
};

struct FmtResult { bool ok; std::string text, error; };

FmtResult fmt(const char* f, std::initializer_list<uint64_t> ints = {}, std::initializer_list<double> doubles = {}) {
    FakeArgs a;
    a.ints.assign(ints.begin(), ints.end());
    a.doubles.assign(doubles.begin(), doubles.end());
    std::string out;
    auto st = libsystem::format_printf(f, a, out);
    return {st.ok, out, st.error};
}

uint64_t P(const void* p) { return reinterpret_cast<uint64_t>(p); }
uint64_t I(int64_t v) { return static_cast<uint64_t>(v); }

// ---- running fixtures ----------------------------------------------------------------------------------------------------------
struct Run {
    guest::Outcome r;
    std::string out, err;
};

Run run_fixture(const std::string& name, std::vector<std::string> args = {}, const fs::path& root = {}) {
    Run run;
    guest::GuestConfig cfg;
    cfg.args = {name};
    cfg.args.insert(cfg.args.end(), args.begin(), args.end());
    cfg.root = root;
    cfg.sink = [&](int fd, const char* d, size_t n) { (fd == 2 ? run.err : run.out).append(d, n); };
    auto bytes = exec_fixture(name);
    run.r = guest::run_in_process(ByteView(bytes), cfg);
    return run;
}

void expect_exited(const Run& r, int code, const char* what) {
    if (r.r.state != guest::State::Exited) { report_failure(__FILE__, __LINE__, std::string(what) + ": state " + guest::to_string(r.r.state) + " " + r.r.message); return; }
    if (r.r.exit_code != code) report_failure(__FILE__, __LINE__, std::string(what) + ": exit code " + std::to_string(r.r.exit_code) + ", expected " + std::to_string(code));
}

// The reference text comes from the HOST printf with the same format and values, an implementation independent of
// ORVYN's argument handling (it only validates the numeric rendering).
std::string host_f(const char* f, double v) {
    char b[128];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

}  // namespace

// ============================== formatter ===================================================================================
ORVYN_TEST(format_plain_text_and_percent) {
    CHECK_EQ(fmt("no conversions").text, std::string("no conversions"));
    CHECK_EQ(fmt("100%% sure").text, std::string("100% sure"));
    CHECK_EQ(fmt("").text, std::string(""));
}

ORVYN_TEST(format_signed_and_unsigned_integers) {
    CHECK_EQ(fmt("%d", {I(-5)}).text, std::string("-5"));
    CHECK_EQ(fmt("%i", {I(42)}).text, std::string("42"));
    CHECK_EQ(fmt("[%5d][%-5d][%05d][%+d][% d]", {I(42), I(42), I(42), I(42), I(42)}).text, std::string("[   42][42   ][00042][+42][ 42]"));
    CHECK_EQ(fmt("%u", {I(-1)}).text, std::string("4294967295"));                     // 32-bit unsigned
    CHECK_EQ(fmt("%x %X %#x %o %#o", {255, 255, 255, 8, 8}).text, std::string("ff FF 0xff 10 010"));
    CHECK_EQ(fmt("%x", {I(-1)}).text, std::string("ffffffff"));
    CHECK_EQ(fmt("%lx", {I(-1)}).text, std::string("ffffffffffffffff"));
    CHECK_EQ(fmt("%ld %lld %zd %jd", {I(-5000000000), I(9000000000000000000), I(-1), I(7)}).text, std::string("-5000000000 9000000000000000000 -1 7"));
    CHECK_EQ(fmt("%hhd %hd %hhu %hu", {300, 70000, I(-1), I(-1)}).text, std::string("44 4464 255 65535"));
    CHECK_EQ(fmt("%.5d|%8.3d|%-8.3d|", {42, 7, 7}).text, std::string("00042|     007|007     |"));
    CHECK_EQ(fmt("%.0d", {0}).text, std::string(""));                                  // C: zero with precision 0 prints nothing
    CHECK_EQ(fmt("%d", {I(-2147483647 - 1)}).text, std::string("-2147483648"));
}

ORVYN_TEST(format_star_width_and_precision) {
    CHECK_EQ(fmt("[%*d]", {6, 7}).text, std::string("[     7]"));
    CHECK_EQ(fmt("[%*d]", {I(-6), 7}).text, std::string("[7     ]"));                  // negative width = left justify
    CHECK_EQ(fmt("[%.*d]", {4, 7}).text, std::string("[0007]"));
    CHECK_EQ(fmt("[%.*d]", {I(-1), 7}).text, std::string("[7]"));                      // negative precision = none
    CHECK_EQ(fmt("[%*.*f]", {10, 2}, {3.14159}).text, std::string("[      3.14]"));
    CHECK_EQ(fmt("[%-*s]", {6, P("ab")}).text, std::string("[ab    ]"));
}

ORVYN_TEST(format_characters_and_strings) {
    CHECK_EQ(fmt("%c%c", {'o', 'k'}).text, std::string("ok"));
    CHECK_EQ(fmt("[%3c][%-3c]", {'B', 'C'}).text, std::string("[  B][C  ]"));
    CHECK_EQ(fmt("Text: %s", {P("ORVYN")}).text, std::string("Text: ORVYN"));
    CHECK_EQ(fmt("[%10s][%-10s][%.3s][%10.2s]", {P("hello"), P("hello"), P("hello"), P("hello")}).text, std::string("[     hello][hello     ][hel][        he]"));
    CHECK_EQ(fmt("[%s][%8s]", {0, 0}).text, std::string("[(null)][  (null)]"));
    const char with_nul[] = {'a', 0, 'b'};
    CHECK_EQ(fmt("%s", {P(with_nul)}).text, std::string("a"));                         // stops at the first NUL
    char zero_prec[] = "abc";
    CHECK_EQ(fmt("%.0s|", {P(zero_prec)}).text, std::string("|"));
}

ORVYN_TEST(format_pointers_use_the_darwin_spelling) {
    CHECK_EQ(fmt("%p", {0x1234}).text, std::string("0x1234"));
    CHECK_EQ(fmt("%p", {0}).text, std::string("0x0"));
    CHECK_EQ(fmt("[%10p][%-10p]", {0x1f, 0x1f}).text, std::string("[      0x1f][0x1f      ]"));
}

ORVYN_TEST(format_doubles_match_the_host_rendering) {
    CHECK_EQ(fmt("Float: %.2f", {}, {3.14}).text, std::string("Float: 3.14"));
    for (double v : {0.0, 0.5, -0.5, 3.14159, 12345.6789, 1e-5, 1e15, 123456789.125, -2.5e-8}) {
        for (const char* f : {"%f", "%.3f", "%10.2f", "%-10.1f|", "%+f", "%e", "%.2E", "%g", "%G", "%.10g", "%#g", "%08.2f"}) {
            FmtResult r = fmt(f, {}, {v});
            CHECK(r.ok);
            if (r.text != host_f(f, v)) report_failure(__FILE__, __LINE__, std::string(f) + " of " + std::to_string(v) + ": got " + r.text + " expected " + host_f(f, v));
        }
    }
    CHECK_EQ(fmt("%+.0f", {}, {2.5}).text, std::string("+2"));                          // round half to even, like Darwin
    CHECK_EQ(fmt("%.0f", {}, {3.5}).text, std::string("4"));
}

ORVYN_TEST(format_infinities_and_nan_are_spelled_like_darwin) {
    const double inf = INFINITY, nan = std::nan("");
    CHECK_EQ(fmt("%f %f %F %E %G", {}, {inf, -inf, inf, inf, inf}).text, std::string("inf -inf INF INF INF"));
    CHECK_EQ(fmt("[%5.1f][%-6f][%+f][% f]", {}, {nan, nan, inf, inf}).text, std::string("[  nan][nan   ][+inf][ inf]"));
    CHECK_EQ(fmt("%08f", {}, {inf}).text, std::string("     inf"));                       // the 0 flag is ignored for inf/nan
}

ORVYN_TEST(format_mixed_arguments_pull_from_the_right_class) {
    // int, double, string, long, double, char: integers and doubles are consumed from their own queues
    auto r = fmt("%d %f %s %ld %f %c", {1, P("two"), 3, '5'}, {1.5, 4.25});
    CHECK(r.ok);
    CHECK_EQ(r.text, std::string("1 1.500000 two 3 4.250000 5"));
}

ORVYN_TEST(format_refuses_what_it_does_not_support) {
    struct Case { const char* fmt; const char* needle; };
    const Case cases[] = {
        {"%n", "%n"}, {"%a", "hexadecimal"}, {"%A", "hexadecimal"}, {"%Lf", "long double"}, {"%ls", "wide"}, {"%lc", "wide"}, {"%C", "wide"}, {"%S", "wide"},
        {"%D", "legacy"}, {"%'d", "thousands"}, {"%1$d", "unsupported"}, {"%y", "unsupported"}, {"abc%", "ends inside"}, {"%5", "ends inside"},
        {"%99999999d", "too large"}, {"%.99999999d", "too large"},
    };
    for (auto& c : cases) {
        FmtResult r = fmt(c.fmt, {1, 2, 3}, {1.0});
        if (r.ok) { report_failure(__FILE__, __LINE__, std::string("accepted ") + c.fmt); continue; }
        if (r.error.find(c.needle) == std::string::npos) report_failure(__FILE__, __LINE__, std::string(c.fmt) + ": error was " + r.error);
    }
    CHECK(!fmt(nullptr).ok);
}

ORVYN_TEST(format_output_and_string_limits) {
    FakeArgs a;
    a.ints = {P("0123456789")};
    std::string out;
    auto st = libsystem::format_printf("%s", a, out, 1024, 4);       // string limit smaller than the string
    CHECK(!st.ok);
    FakeArgs c;
    c.ints = {50000, 1, 50000, 2};
    std::string out3;
    CHECK(!libsystem::format_printf("%*d%*d%*d", c, out3, 60000).ok);   // second field pushes the output over 60000 bytes
}

// ============================== stdio through real Mach-O fixtures ============================================================
ORVYN_TEST(hello_prints_exactly_the_bytes_the_macho_emits) {
    Run r = run_fixture("hello");
    expect_exited(r, 0, "hello");
    CHECK_EQ(r.out, std::string("Hello from macOS!\nsecond line via write\n"));
    CHECK_EQ(r.err, std::string(""));
}

ORVYN_TEST(printf_with_integers) {
    Run r = run_fixture("printf_int");
    expect_exited(r, 4, "printf_int");
    CHECK_EQ(r.out,
             std::string("Number: 42\n"
                         "[   42][42   ][00042][+42][ 42]\n"
                         "[ff][FF][0xff][10][4000000000]\n"
                         "[-5000000000][9000000000000000000][44][4464]\n"
                         "[A][  B][C  ]%\n"
                         "[     7][7     ][0007]\n"
                         "1 2 3 4 5 6 7 8 9 10\n"
                         "end\n"));
}

ORVYN_TEST(printf_with_doubles) {
    Run r = run_fixture("printf_double");
    expect_exited(r, 0, "printf_double");
    CHECK_EQ(r.out,
             std::string("Float: 3.14\n"
                         "[0.500000][1.234568e+04][0.0001234][1E-10]\n"
                         "[     3.142][2.5       ][+2][-0003.14]\n"
                         "[inf][-inf][  nan][INF][+inf]\n"
                         "1 2 3 4 5 6 7 8 9 10.5\n"));
}

ORVYN_TEST(printf_with_strings) {
    Run r = run_fixture("printf_str");
    expect_exited(r, 0, "printf_str");
    CHECK_EQ(r.out,
             std::string("Text: ORVYN\n"
                         "[     hello][hello     ][hel][        he]\n"
                         "[(null)][  (null)]\n"
                         "a b c d e f g\n"
                         "ok !\n"));
}

ORVYN_TEST(printf_with_mixed_arguments_stack_arguments_and_va_list) {
    Run r = run_fixture("printf_mixed");
    // the third line is produced by vsnprintf over a va_list built by clang (register save area + overflow area)
    const std::string line3 = "1 2 3 4 5 6 7 8 | 0.500000 1.500000 2.500000 3.500000 4.500000 5.500000 6.500000 7.500000 8.500000 9.500000 | tail";
    expect_exited(r, static_cast<int>(line3.size()), "printf_mixed");
    CHECK_EQ(r.out,
             std::string("1 1.500000 two 3 4.250000 5 6 7.750000 0x1234\n"
                         "1|a|1.500000|2|3|4|5|2.500000|6|3.500000|z|123456789012\n") + line3 + "\n" +
                 "va_list printf: 7 2.5 ok\n");
}

ORVYN_TEST(snprintf_truncates_and_reports_the_full_length) {
    Run r = run_fixture("snprintf");
    expect_exited(r, 17, "snprintf");
    CHECK_EQ(r.out, std::string("[abcdef-] 12\n6\n00042:x:2.2 11\n"));
}

ORVYN_TEST(stdout_and_stderr_carry_exactly_the_bytes_written) {
    Run r = run_fixture("stdio_out");
    expect_exited(r, 3, "stdio_out");
    CHECK_EQ(r.out, std::string("out-1\nA\nfw\nw1\nZ\nend ok\n"));
    CHECK_EQ(r.err, std::string("err-1\nerr-2\nw2\n"));
}

ORVYN_TEST(unsupported_printf_conversions_trap_instead_of_printing_something_wrong) {
    const struct { const char* arg; const char* needle; } cases[] = {
        {"n", "%n"}, {"a", "hexadecimal"}, {"L", "long double"}, {"w", "wide"}, {"q", "thousands"}};
    for (auto& c : cases) {
        Run r = run_fixture("unsupported", {c.arg});
        if (r.r.state != guest::State::Faulted) { report_failure(__FILE__, __LINE__, std::string("not faulted for ") + c.arg); continue; }
        if (r.r.message.find(c.needle) == std::string::npos) report_failure(__FILE__, __LINE__, "message: " + r.r.message);
        CHECK_EQ(r.out, std::string("before\n"));                                       // nothing wrong was printed, and "after" never ran
    }
}

ORVYN_TEST(aggregate_returns_and_mixed_signatures_work_in_real_code) {
    Run r = run_fixture("abi_ret");
    expect_exited(r, 17, "abi_ret");
    CHECK_EQ(r.out, std::string("9 2 -14285714285 -5 48.0 0.75 6 1.5\n"));
}

// ============================== files ======================================================================================
ORVYN_TEST(reading_files_through_the_guest_descriptor_table) {
    TempDir t;
    t.write("input.txt", std::string("hello, world!\n"));
    Run r = run_fixture("files_read", {}, t.path());
    expect_exited(r, 0, "files_read");
    CHECK_EQ(r.out, std::string("size=14 word=world\n"));
}

ORVYN_TEST(without_a_root_the_guest_cannot_open_files) {
    TempDir t;
    t.write("input.txt", std::string("hello, world!\n"));
    Run r = run_fixture("files_read", {}, {});                // no root: EACCES, the fixture reports step 10
    expect_exited(r, 10, "files_read without root");
}

ORVYN_TEST(writing_files_appears_on_the_host_with_the_exact_bytes) {
    TempDir t;
    fs::create_directories(t.path() / "root");
    Run r = run_fixture("files_write", {}, t.path() / "root");
    expect_exited(r, 0, "files_write");
    CHECK_EQ(r.out, std::string("written\n"));
    auto out = read_file(t.path() / "root" / "out.txt", 1 << 20);
    REQUIRE(out.ok());
    CHECK_EQ(std::string(out.value().begin(), out.value().end()), std::string("alpha\nbeta\ngamma\n"));
    // "../../../escape.txt" was clamped to the root: the file is INSIDE it, and nothing was created above it
    auto esc = read_file(t.path() / "root" / "escape.txt", 1 << 20);
    REQUIRE(esc.ok());
    CHECK_EQ(std::string(esc.value().begin(), esc.value().end()), std::string("inside"));
    CHECK(!fs::exists(t.path() / "escape.txt"));
    CHECK(!fs::exists(t.path().parent_path() / "escape.txt"));
}

ORVYN_TEST(guest_paths_cannot_leave_the_root_not_even_through_a_junction) {
    TempDir t;
    fs::create_directories(t.path() / "root");
    fs::create_directories(t.path() / "outside");
    t.write("outside/secret.txt", std::string("secret"));
    t.write("root/inside.txt", std::string("inside-data"));
    // a directory junction INSIDE the root that points OUTSIDE it (needs no privilege)
    auto native = [](fs::path p) { return p.make_preferred().string(); };
    const std::string cmd = "mklink /J " + std::string(1, '"') + native(t.path() / "root" / "link") + std::string(1, '"') + " " + std::string(1, '"') +
                            native(t.path() / "outside") + std::string(1, '"') + " >nul 2>nul";
    const bool have_junction = std::system(cmd.c_str()) == 0 && fs::exists(t.path() / "root" / "link" / "secret.txt");
    auto open_path = [&](const char* p) { return run_fixture("open_path", {p}, t.path() / "root").r.exit_code; };
    CHECK_EQ(open_path("inside.txt"), 0);                                    // control: a normal file works
    CHECK_EQ(open_path("/inside.txt"), 0);                                   // absolute guest paths start at the root
    CHECK_EQ(open_path("./sub/../inside.txt"), 0);                           // lexical normalisation
    CHECK_EQ(open_path("../outside/secret.txt"), 2);                         // ".." stops at the root: root/outside/secret.txt does not exist (ENOENT)
    CHECK_EQ(open_path("/../../outside/secret.txt"), 2);
    CHECK_EQ(open_path("C:/Windows/win.ini"), 22);                           // drive letters are not Darwin paths (EINVAL)
    CHECK_EQ(open_path("//server/share/file"), 2);                           // collapsed to /server/share/file inside the root
    CHECK_EQ(open_path("con"), 13);                                          // Windows device names are refused (EACCES)
    CHECK_EQ(open_path("aux.txt"), 13);
    CHECK_EQ(open_path("a*b"), 22);
    CHECK_EQ(open_path("trailing."), 22);                                    // Windows would silently strip the dot
    CHECK_EQ(open_path(""), 2);
    if (have_junction) CHECK_EQ(open_path("link/secret.txt"), 13);          // resolves outside the root: EACCES
    else std::printf("    (junction escape not tested: mklink /J unavailable)\n");
}

ORVYN_TEST(guest_files_and_handles_do_not_leak_into_the_host_process) {
    TempDir t;
    fs::create_directories(t.path() / "root");
    run_fixture("files_write", {}, t.path() / "root");        // warm up
    DWORD before = 0, after = 0;
    GetProcessHandleCount(GetCurrentProcess(), &before);
    for (int i = 0; i < 25; ++i) {
        Run r = run_fixture("files_write", {}, t.path() / "root");
        if (r.r.exit_code != 0) { report_failure(__FILE__, __LINE__, "files_write failed in the loop"); break; }
    }
    GetProcessHandleCount(GetCurrentProcess(), &after);
    CHECK(after <= before + 2);
    // and a guest that exits while holding open descriptors is cleaned up by the RuntimeContext
}

// ============================== time and process identity ===================================================================
ORVYN_TEST(time_clock_and_nanosleep_follow_the_darwin_contract) {
    Run r = run_fixture("time_clock");
    expect_exited(r, 0, "time_clock");
    // the printed time is the real wall-clock time
    long long printed = 0;
    REQUIRE(std::sscanf(r.out.c_str(), "time=%lld", &printed) == 1);
    const long long now = static_cast<long long>(std::time(nullptr));
    CHECK(std::llabs(now - printed) <= 5);
}

ORVYN_TEST(process_ids_are_the_real_ones) {
    Run r = run_fixture("ids");
    expect_exited(r, 0, "ids");
    int pid = 0, ppid = 0;
    REQUIRE(std::sscanf(r.out.c_str(), "pid=%d ppid=%d", &pid, &ppid) == 2);
    CHECK_EQ(pid, static_cast<int>(GetCurrentProcessId()));
    CHECK(ppid > 0);
    CHECK(ppid != pid);
}

ORVYN_TEST(errno_values_are_darwin_values_not_windows_or_linux) {
    // files_read checks ENOENT == 2 and EBADF == 9 inside the guest with Darwin's own numbering, EEXIST == 17 and EINVAL == 22 in files_write
    TempDir t;
    t.write("input.txt", std::string("hello, world!\n"));
    expect_exited(run_fixture("files_read", {}, t.path()), 0, "files_read errno checks");
}

// ============================== ORVYN 0.6: Darwin libc basics ====================================================================================
ORVYN_TEST(darwin_libc_basics_memchr_modf_fma_memalign_cxa_atexit_stack_guard_and_chkstk) {
    Run r = run_fixture("libc_darwin");
    expect_exited(r, 42, "libc_darwin");
    // handlers run in reverse registration order after main; __cxa_atexit ones receive their argument
    CHECK_EQ(r.out, std::string("libc ok\ncxa(three)\natexit\ncxa(one)\n"));
}

ORVYN_TEST(assert_failure_prints_the_darwin_message_and_ends_the_program_like_sigabrt) {
    Run r = run_fixture("libc_darwin", {"assert"});
    REQUIRE(r.r.state == guest::State::Faulted);
    CHECK_EQ(r.err, std::string("Assertion failed: (x == 1), function main, file libc_darwin.c, line 777.\n"));
    CHECK(r.r.message.find("assertion failed") != std::string::npos);
}

ORVYN_TEST(abort_stops_the_program_and_skips_atexit_handlers) {
    Run r = run_fixture("libc_darwin", {"abort"});
    REQUIRE(r.r.state == guest::State::Faulted);
    CHECK(r.r.message.find("abort()") != std::string::npos);
    CHECK_EQ(r.out, std::string("before abort\n"));
}

ORVYN_TEST(the_stack_protector_catches_a_smashed_canary) {
    expect_exited(run_fixture("stack_smash"), 42, "stack_smash (no overrun)");              // the canary never fires falsely
    Run r = run_fixture("stack_smash", {"overrun"});
    REQUIRE(r.r.state == guest::State::Faulted);
    CHECK(r.r.message.find("__stack_chk_fail") != std::string::npos);
    CHECK_EQ(r.err, std::string("*** stack smashing detected ***: terminated\n"));
    CHECK(r.out.find("returned normally") == std::string::npos);
}

ORVYN_TEST(getopt_follows_the_bsd_contract_and_its_globals_are_shared_with_the_program) {
    // options, an attached and a detached argument, a cluster, "--" and a non-option: stops at the first operand
    Run r = run_fixture("libc_darwin", {"getopt", "-a", "-bVAL", "-c", "-b", "two", "file", "-a"});
    expect_exited(r, 40 + 2, "getopt basic");                                                // operands left: "file" and "-a" (BSD getopt does not permute)
    CHECK_EQ(r.out, std::string("a\nb=VAL\nc\nb=two\noptind=7 rest=file\n"));
    CHECK(r.err.empty());
    Run cluster = run_fixture("libc_darwin", {"getopt", "-acbX", "--", "-a"});
    expect_exited(cluster, 40 + 1, "getopt cluster and --");
    CHECK_EQ(cluster.out, std::string("a\nc\nb=X\noptind=4 rest=-a\n"));
    // unknown option and missing argument: '?', message on stderr with the program name
    Run bad = run_fixture("libc_darwin", {"getopt", "-z", "-b"});
    expect_exited(bad, 40, "getopt errors");
    CHECK_EQ(bad.out, std::string("?(z)\n?(b)\noptind=4\n"));
    CHECK_EQ(bad.err, std::string("libc_darwin: illegal option -- z\nlibc_darwin: option requires an argument -- b\n"));
    // a leading ':' silences the messages and reports a missing argument as ':'
    Run quiet = run_fixture("libc_darwin", {"getopt_colon", "-z", "-b"});
    expect_exited(quiet, 40, "getopt_colon");
    CHECK_EQ(quiet.out, std::string("?(z)\n:(b)\noptind=4\n"));
    CHECK(quiet.err.empty());
    // no options at all
    Run none = run_fixture("libc_darwin", {"getopt"});
    expect_exited(none, 40, "getopt without options");
    CHECK_EQ(none.out, std::string("optind=2\n"));
}

ORVYN_TEST(darwin_libc_data_symbols_live_in_guest_writable_memory_and_restart_for_every_run) {
    auto p = take(libsystem::LibSystemProvider::create());
    for (const char* n : {"_optarg", "_optind", "_optopt", "_opterr", "_optreset", "___stack_chk_guard"}) {
        auto s = p->lookup(libsystem::kInstallName, n);
        REQUIRE(s.has_value());
        CHECK(!s->is_function);
        CHECK(s->address != 0);
    }
    libsystem::RuntimeContext ctx;
    p->install_stdio(ctx);
    REQUIRE(ctx.libc_vars != nullptr);
    CHECK_EQ(ctx.libc_vars->optind, 1);
    CHECK_EQ(ctx.libc_vars->opterr, 1);
    CHECK_EQ(*reinterpret_cast<int*>(p->lookup(libsystem::kInstallName, "_optind")->address), 1);
    ctx.libc_vars->optind = 9;
    p->install_stdio(ctx);                                              // a new process starts from the defaults again
    CHECK_EQ(ctx.libc_vars->optind, 1);
    uint64_t guard = *reinterpret_cast<uint64_t*>(p->lookup(libsystem::kInstallName, "___stack_chk_guard")->address);
    CHECK(guard != 0);
    CHECK_EQ(guard & 0xff, uint64_t{0});
}
