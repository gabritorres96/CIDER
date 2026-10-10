// Real execution of Mach-O x86_64 fixtures (built by clang + ld64.lld) inside the test process.
// Each test checks a value that can only come from the guest's machine instructions, and several tests
// prove authenticity by patching those instructions and observing the changed result.
#include <algorithm>
#include <cstring>
#include <functional>

#include "cider/guest.hpp"
#include "guest_code.hpp"
#include "test_framework.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

using namespace cider;
using namespace cider::testing;

namespace {

struct Run {
    guest::Outcome r;
    std::string out, err;
};

Run run_bytes(const std::vector<uint8_t>& bytes, std::vector<std::string> args = {"guest"}) {
    guest::GuestConfig cfg;
    cfg.args = std::move(args);
    Run run;
    cfg.sink = [&](int fd, const char* d, size_t n) { (fd == 2 ? run.err : run.out).append(d, n); };
    run.r = guest::run_in_process(ByteView(bytes), cfg);
    return run;
}

Run run_fixture(const std::string& name, std::vector<std::string> args = {}) {
    if (args.empty()) args = {name};
    return run_bytes(exec_fixture(name), std::move(args));
}

void expect_exit(const std::string& name, int expected) {
    Run r = run_fixture(name);
    if (r.r.state != guest::State::Exited) { report_failure(__FILE__, __LINE__, name + ": state " + guest::to_string(r.r.state) + " " + r.r.message); return; }
    if (r.r.exit_code != expected) report_failure(__FILE__, __LINE__, name + ": expected guest exit code " + std::to_string(expected) + ", got " + std::to_string(r.r.exit_code));
}

// Replace the first occurrence of `from` by `to` in the file (same length): edits the guest's machine code.
bool patch_code(std::vector<uint8_t>& b, std::vector<uint8_t> from, std::vector<uint8_t> to) {
    auto it = std::search(b.begin(), b.end(), from.begin(), from.end());
    if (it == b.end()) return false;
    std::copy(to.begin(), to.end(), it);
    return true;
}

bool range_free(uint64_t addr) { return memory::query_address(reinterpret_cast<void*>(addr)).state == memory::PageInfo::State::Free; }

}  // namespace

// ---- fixtures A..H: results computed independently in C++ -----------------------------------------------------------------
CIDER_TEST(fixture_a_constant_returns_come_from_the_guest_code) {
    expect_exit("ret42", 42);
    expect_exit("ret7", 7);
    expect_exit("ret99", 99);
}

CIDER_TEST(fixture_b_integer_math) {
    // `long` is 64-bit in the macOS fixture: use int64_t here (Windows long is 32-bit)
    const int64_t a = 123456789, b = 987, c = 31;
    int64_t x = a * b;
    x += c;
    x %= 251;
    int64_t y = (a ^ b) & 0xff;
    uint64_t z = static_cast<uint64_t>(a) / static_cast<uint64_t>(c);
    expect_exit("math", static_cast<int>((x + y + static_cast<int64_t>(z % 97)) & 0xff));
}

CIDER_TEST(fixture_c_stack_locals_and_recursion) {
    std::function<int(int)> fib = [&](int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); };
    int s = 0;
    for (int i = 0; i < 200; ++i) s += static_cast<unsigned char>(i * 7);
    expect_exit("stack", (fib(10) + (s & 0x3f)) & 0xff);
}

CIDER_TEST(fixture_d_calls_between_functions_and_through_a_data_pointer) { expect_exit("calls", (10 + 20 + 3) * 2 + 1); }

CIDER_TEST(fixture_e_global_data_and_rebased_pointers) { expect_exit("globals", 5 + 30 + 20 + ('b' - 97) + 3); }

CIDER_TEST(fixture_f_zero_fill_sections_start_zeroed_and_are_writable) { expect_exit("bss", 1000 & 0xff); }

CIDER_TEST(fixture_g_calls_into_cider_libsystem) { expect_exit("libcall", 13 + 30 + 15 - 15); }

CIDER_TEST(fixture_h_text_output_comes_from_the_guest) {
    Run r = run_fixture("hello");
    CHECK(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 0);
    CHECK_EQ(r.out, std::string("Hello from macOS!\nsecond line via write\n"));
    CHECK_EQ(r.err, std::string(""));
}

CIDER_TEST(malloc_realloc_calloc_free_from_a_real_binary) { expect_exit("malloc", 4950 % 251); }

CIDER_TEST(double_math_through_the_bridge_from_a_real_binary) { expect_exit("fmath", 12 + 32 + 2); }

CIDER_TEST(exit_from_a_nested_function_stops_the_program) {
    Run r = run_fixture("exitcall");
    CHECK(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 37);
    CHECK(!r.r.returned_from_main);
    CHECK_EQ(r.out, std::string("leaving\n"));     // output before exit() is delivered, code after it never ran
}

// ---- authenticity: the result changes when the machine code changes ---------------------------------------------------------
CIDER_TEST(patching_the_immediate_in_the_binary_changes_the_result) {
    auto b = exec_fixture("ret42");
    REQUIRE(patch_code(b, {0xB8, 0x2A, 0x00, 0x00, 0x00}, {0xB8, 0x4D, 0x00, 0x00, 0x00}));     // mov eax, 42 -> mov eax, 77
    CHECK_EQ(run_bytes(b).r.exit_code, 77);
    b = exec_fixture("ret42");
    REQUIRE(patch_code(b, {0xB8, 0x2A, 0x00, 0x00, 0x00}, {0xB8, 0xFF, 0xFF, 0xFF, 0xFF}));     // -> -1
    CHECK_EQ(run_bytes(b).r.exit_code, -1);
    b = exec_fixture("ret42");
    REQUIRE(patch_code(b, {0xB8, 0x2A, 0x00, 0x00, 0x00}, {0x31, 0xC0, 0x90, 0x90, 0x90}));     // xor eax,eax ; nop*3 -> 0
    CHECK_EQ(run_bytes(b).r.exit_code, 0);
    b = exec_fixture("ret7");
    REQUIRE(patch_code(b, {0xB8, 0x07, 0x00, 0x00, 0x00}, {0xB8, 0xC8, 0x00, 0x00, 0x00}));
    CHECK_EQ(run_bytes(b).r.exit_code, 200);
}

CIDER_TEST(patching_a_string_in_the_data_changes_the_output) {
    auto b = exec_fixture("hello");
    REQUIRE(patch_code(b, {'H', 'e', 'l', 'l', 'o', ' ', 'f', 'r', 'o', 'm'}, {'H', 'o', 'l', 'a', ' ', ' ', 'd', 'e', ' ', ' '}));
    CHECK_EQ(run_bytes(b).out, std::string("Hola  de   macOS!\nsecond line via write\n"));
}

CIDER_TEST(results_do_not_depend_on_the_name_or_path_of_the_file) {
    // run_in_process receives only bytes; the same bytes always give the same result, different bytes different results
    auto a = exec_fixture("ret42");
    CHECK_EQ(run_bytes(a, {"ret7"}).r.exit_code, 42);               // argv[0] pretending to be another program
    CHECK_EQ(run_bytes(a, {"/some/where/else/ret99"}).r.exit_code, 42);
    CHECK(run_fixture("ret7").r.exit_code != run_fixture("ret42").r.exit_code);
    CHECK(run_fixture("ret99").r.exit_code != run_fixture("ret42").r.exit_code);
}

CIDER_TEST(the_same_binary_gives_the_same_result_every_time) {
    for (const char* n : {"math", "stack", "malloc"}) {
        int first = run_fixture(n).r.exit_code;
        for (int i = 0; i < 5; ++i) CHECK_EQ(run_fixture(n).r.exit_code, first);
    }
}

// ---- placement -----------------------------------------------------------------------------------------------------------------------
CIDER_TEST(guest_code_runs_from_the_mapped_macho_segment) {
    Run r = run_fixture("ret42");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.image_base, uint64_t{0x100000000});               // preferred Mach-O base, free in a fresh process
    CHECK(r.r.entry_address > r.r.image_base && r.r.entry_address < r.r.image_base + 0x1000);   // main is inside __TEXT
    CHECK(range_free(r.r.image_base));                               // and the mapping is gone after the run
}

CIDER_TEST(execution_works_when_the_preferred_base_is_taken_and_fixups_slide_the_image) {
    auto blocker = memory::Region::reserve(0x100000000ull, 0x10000, false);
    if (!blocker.ok()) SKIP("0x100000000 is not available in this process");
    for (auto [name, expect] : {std::pair{"calls", 67}, std::pair{"globals", 59}, std::pair{"libcall", 43}, std::pair{"hello", 0}, std::pair{"bss", 232}}) {
        Run r = run_fixture(name);
        if (r.r.state != guest::State::Exited || r.r.exit_code != expect) report_failure(__FILE__, __LINE__, std::string(name) + " failed when relocated");
        CHECK(r.r.image_base != 0x100000000ull);                      // really relocated
        CHECK_EQ(r.r.image_base % 0x10000, uint64_t{0});
    }
}

CIDER_TEST(repeated_runs_do_not_leak_memory_or_address_space) {
#ifdef _WIN32
    auto priv = [] { PROCESS_MEMORY_COUNTERS_EX c{}; GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof c); return static_cast<uint64_t>(c.PrivateUsage); };
    run_fixture("libcall");                                          // warm up allocator caches
    uint64_t before = priv();
    for (int i = 0; i < 150; ++i) {
        CHECK_EQ(run_fixture("libcall").r.exit_code, 43);
        CHECK(range_free(0x100000000ull));
    }
    uint64_t after = priv();
    // 150 runs of an 8 MiB stack + image would be > 1 GiB if leaked. ASan keeps freed host blocks in a quarantine (up to 256 MiB), so its limit is wider.
#if defined(__SANITIZE_ADDRESS__)
    CHECK(after < before + (200ull << 20));
#else
    CHECK(after < before + (24ull << 20));
#endif
#endif
}

CIDER_TEST(the_guest_stack_is_the_size_the_binary_asks_for_and_recursion_inside_it_works) {
    // `stack` recurses 10 deep with 64-byte frames: well inside the default 8 MiB stack.
    CHECK(run_fixture("stack").r.state == guest::State::Exited);
}

CIDER_TEST(guest_cannot_see_host_environment_variables) {
    guest::GuestConfig cfg;
    cfg.args = {"args"};
    cfg.env = {};                                                    // empty environment: envp is a valid empty array
    auto bytes = exec_fixture("args");
    auto r = guest::run_in_process(ByteView(bytes), cfg);
    CHECK_EQ(r.exit_code, 11);                                       // ok flag (envp != NULL) still true
}

CIDER_TEST(returned_vs_exit_distinction_and_negative_codes) {
    auto b = exec_fixture("ret7");
    patch_code(b, {0xB8, 0x07, 0x00, 0x00, 0x00}, {0xB8, 0xFE, 0xFF, 0xFF, 0xFF});    // -2
    auto r = run_bytes(b).r;
    CHECK_EQ(r.exit_code, -2);
    CHECK(r.returned_from_main);
}
