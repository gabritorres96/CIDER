// The libSystem subset: every implemented function is called FROM guest code (through the System V thunks),
// so the argument conversion, return values and side effects are tested the way a Mach-O binary sees them.
#include <cmath>
#include <cstring>
#include <set>

#include "guest_code.hpp"
#include "test_framework.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace cider;
using namespace cider::libsystem;
using namespace cider::testing;

namespace {

struct Env {
    std::unique_ptr<LibSystemProvider> provider = take(LibSystemProvider::create());
    RuntimeContext ctx;
    std::string out, err;
    Env() {
        ctx.sink = [this](int fd, const char* d, size_t n) { (fd == 2 ? err : out).append(d, n); };
        set_context(&ctx);
    }
    ~Env() { set_context(nullptr); }
    abi::CallOutcome call(const char* sym, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0) { return call_host(*provider, sym, a, b, c, d); }
    static uint64_t p(const void* x) { return reinterpret_cast<uint64_t>(x); }
};

}  // namespace

CIDER_TEST(registry_lists_exactly_the_documented_symbols) {
    std::set<std::string> names;
    for (auto& s : implemented_symbols()) {
        CHECK(names.insert(s.name).second);                              // no duplicates
        CHECK(s.fn != nullptr);
        CHECK(s.name[0] == '_' || std::string(s.name) == "dyld_stub_binder");
    }
    // The complete, documented list (docs/DARWIN-RUNTIME.md). Anything else must not be implemented.
    const std::set<std::string> expected = {
        // 0.2
        "_puts", "_putchar", "_write", "_exit", "_getpid", "_getenv", "_strlen", "_strcmp", "_strncmp", "_memcpy", "_memmove", "_memset", "_memcmp",
        "_abs", "_labs", "_malloc", "_calloc", "_realloc", "_free", "_sqrt", "_fabs", "_floor", "_ceil", "_pow", "dyld_stub_binder",
        // 0.3 stdio
        "_fputs", "_fputc", "_putc", "_fwrite", "_fflush", "_printf", "_fprintf", "_sprintf", "_snprintf", "_vprintf", "_vfprintf", "_vsprintf", "_vsnprintf",
        // 0.3 POSIX
        "_open", "_close", "_read", "_lseek", "_stat", "_stat$INODE64", "_fstat", "_fstat$INODE64", "_access", "_getcwd", "_gettimeofday",
        "_clock_gettime", "_time", "_nanosleep", "_usleep", "_sleep", "_getppid", "__exit", "___error",
        // 0.3 strings, math, aggregates
        "_strcpy", "_strncpy", "_strcat", "_strchr", "_strrchr", "_strstr", "_strdup", "_atoi", "_strtol", "_sin", "_cos", "_tan", "_exp", "_log",
        "_log10", "_atan", "_atan2", "_fmod", "_ldexp", "_frexp", "_div", "_ldiv",
        // 0.3 thread-local storage
        "__tlv_bootstrap", "_tlv_get_addr",
        // 0.4 callbacks
        "_qsort", "_bsearch", "_atexit",
        // 0.4.1 FILE streams
        "_fopen", "_fclose", "_fread", "_fgetc", "_getc", "_ungetc", "_fgets", "_feof", "_ferror", "_clearerr", "_ftell", "_ftello", "_fseek",
        "_fseeko", "_rewind", "_fileno", "_setvbuf",
        // 0.4.2 pthreads
        "_pthread_create", "_pthread_join", "_pthread_detach", "_pthread_self", "_pthread_equal", "_pthread_exit", "_sched_yield", "_pthread_attr_init", "_pthread_attr_destroy", "_pthread_attr_setstacksize", "_pthread_attr_getstacksize", "_pthread_attr_setdetachstate", "_pthread_mutexattr_init", "_pthread_mutexattr_destroy", "_pthread_mutexattr_settype", "_pthread_mutexattr_gettype", "_pthread_mutex_init", "_pthread_mutex_destroy", "_pthread_mutex_lock", "_pthread_mutex_trylock", "_pthread_mutex_unlock", "_pthread_cond_init", "_pthread_cond_destroy", "_pthread_cond_wait", "_pthread_cond_timedwait", "_pthread_cond_signal", "_pthread_cond_broadcast", "_pthread_once", "_pthread_key_create", "_pthread_key_delete", "_pthread_getspecific", "_pthread_setspecific"};
    CHECK(names == expected);
    for (const auto& n : expected) CHECK(is_implemented(n));
    for (const char* n : {"___stdinp", "___stdoutp", "___stderrp"}) CHECK(is_implemented(n));
    // honest negatives: NOT implemented, so binaries importing them are rejected at load time
    for (const char* n : {"_fork", "_execve", "_signal", "_mmap", "_dlopen", "_sysctl", "_sscanf", "_scanf"})
        CHECK(!is_implemented(n));
}

CIDER_TEST(puts_putchar_and_write_reach_the_sink_with_correct_values) {
    Env e;
    const char* s = "hello";
    CHECK_EQ(static_cast<int32_t>(e.call("_puts", Env::p(s)).value), 1);
    CHECK_EQ(e.out, std::string("hello\n"));
    CHECK_EQ(static_cast<int32_t>(e.call("_putchar", 'Z').value), 'Z');
    CHECK_EQ(e.out, std::string("hello\nZ"));
    const char* w = "abc\n";
    CHECK_EQ(e.call("_write", 1, Env::p(w), 4).value, uint64_t{4});
    CHECK_EQ(e.call("_write", 2, Env::p("err"), 3).value, uint64_t{3});
    CHECK_EQ(e.out, std::string("hello\nZabc\n"));
    CHECK_EQ(e.err, std::string("err"));
    CHECK_EQ(e.call("_write", 7, Env::p(w), 4).value, ~uint64_t{0});     // unsupported fd: -1 as a full 64-bit value
    CHECK_EQ(e.call("_write", 1, 0, 5).value, ~uint64_t{0});             // NULL buffer
    CHECK_EQ(e.call("_write", 1, 0, 0).value, uint64_t{0});              // empty write is fine
    CHECK_EQ(static_cast<int32_t>(e.call("_puts", 0).value), -1);        // puts(NULL) refused instead of crashing
    CHECK(e.ctx.bytes_out > 0 && e.ctx.bytes_err == 3);
}

CIDER_TEST(string_and_memory_functions) {
    Env e;
    CHECK_EQ(e.call("_strlen", Env::p("")).value, uint64_t{0});
    CHECK_EQ(e.call("_strlen", Env::p("twelve chars")).value, uint64_t{12});
    CHECK_EQ(e.call("_strlen", 0).value, uint64_t{0});                   // tolerant of NULL
    CHECK(static_cast<int32_t>(e.call("_strcmp", Env::p("abc"), Env::p("abd")).value) < 0);
    CHECK_EQ(static_cast<int32_t>(e.call("_strcmp", Env::p("abc"), Env::p("abc")).value), 0);
    CHECK(static_cast<int32_t>(e.call("_strcmp", Env::p("b"), Env::p("a")).value) > 0);
    CHECK_EQ(static_cast<int32_t>(e.call("_strncmp", Env::p("abcX"), Env::p("abcY"), 3).value), 0);
    CHECK(static_cast<int32_t>(e.call("_strncmp", Env::p("abcX"), Env::p("abcY"), 4).value) < 0);

    char a[16] = "0123456789abcde", b[16] = {};
    CHECK_EQ(e.call("_memcpy", Env::p(b), Env::p(a), 16).value, Env::p(b));          // returns dest
    CHECK(std::memcmp(a, b, 16) == 0);
    char ov[16] = "abcdefghij";
    e.call("_memmove", Env::p(ov + 2), Env::p(ov), 8);                                // overlapping move
    CHECK_EQ(std::string(ov, 10), std::string("ababcdefgh"));
    char z[8];
    CHECK_EQ(e.call("_memset", Env::p(z), 0x5A, 8).value, Env::p(z));
    CHECK_EQ(std::string(z, 8), std::string(8, 'Z'));
    CHECK_EQ(static_cast<int32_t>(e.call("_memcmp", Env::p("aaa"), Env::p("aaa"), 3).value), 0);
    CHECK(static_cast<int32_t>(e.call("_memcmp", Env::p("aab"), Env::p("aaa"), 3).value) > 0);
    CHECK_EQ(static_cast<int32_t>(e.call("_abs", static_cast<uint64_t>(-17)).value), 17);
    CHECK_EQ(static_cast<int32_t>(e.call("_abs", 17).value), 17);
    CHECK_EQ(e.call("_labs", static_cast<uint64_t>(-5000000000ll)).value, uint64_t{5000000000ull});
}

CIDER_TEST(malloc_family_tracks_blocks_and_refuses_foreign_pointers) {
    Env e;
    uint64_t p = e.call("_malloc", 100).value;
    REQUIRE(p != 0);
    CHECK_EQ(p % 16, uint64_t{0});                                                    // 16-byte aligned like libSystem
    CHECK_EQ(e.ctx.allocations.size(), size_t{1});
    std::memset(reinterpret_cast<void*>(p), 0x11, 100);
    uint64_t q = e.call("_realloc", p, 1000).value;
    REQUIRE(q != 0);
    CHECK_EQ(reinterpret_cast<uint8_t*>(q)[99], uint8_t{0x11});                       // contents preserved
    CHECK_EQ(e.ctx.allocations.size(), size_t{1});
    uint64_t c = e.call("_calloc", 10, 8).value;
    bool zero = true;
    for (int i = 0; i < 80; ++i) zero &= reinterpret_cast<uint8_t*>(c)[i] == 0;
    CHECK(zero);
    CHECK_EQ(e.ctx.allocations.size(), size_t{2});
    e.call("_free", q);
    e.call("_free", c);
    CHECK(e.ctx.allocations.empty());
    e.call("_free", 0);                                                               // free(NULL) is a no-op
    // foreign / double frees must not reach the host heap (that would corrupt it)
    int local = 0;
    e.call("_free", Env::p(&local));
    e.call("_free", q);                                                               // double free
    CHECK_EQ(e.call("_realloc", Env::p(&local), 10).value, uint64_t{0});              // refused
    CHECK(e.call("_malloc", 0).value != 0);                                           // malloc(0) returns a unique pointer
    CHECK(e.call("_realloc", 0, 32).value != 0);                                      // realloc(NULL, n) == malloc
    // the two blocks still live are released together with the context (HeapDestroy): see the next test
}

CIDER_TEST(getenv_reads_the_guest_environment) {
    Env e;
    e.ctx.env = {"HOME=/Users/test", "EMPTY=", "A=1", "AB=2"};
    auto get = [&](const char* n) { uint64_t r = e.call("_getenv", Env::p(n)).value; return r ? std::string(reinterpret_cast<const char*>(r)) : std::string("<null>"); };
    CHECK_EQ(get("HOME"), std::string("/Users/test"));
    CHECK_EQ(get("EMPTY"), std::string(""));
    CHECK_EQ(get("A"), std::string("1"));                                             // not confused with AB
    CHECK_EQ(get("AB"), std::string("2"));
    CHECK_EQ(get("MISSING"), std::string("<null>"));
    CHECK_EQ(get("HOM"), std::string("<null>"));
    CHECK_EQ(e.call("_getenv", 0).value, uint64_t{0});
}

CIDER_TEST(getpid_returns_the_real_process_id) {
    Env e;
#ifdef _WIN32
    CHECK_EQ(static_cast<uint32_t>(e.call("_getpid").value), static_cast<uint32_t>(GetCurrentProcessId()));
#endif
    CHECK(static_cast<int32_t>(e.call("_getpid").value) > 0);
}

CIDER_TEST(math_functions_use_xmm_registers) {
    Env e;
    auto bits = [](double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; };
    auto unary = [&](const char* sym, double x) {
        auto s = e.provider->lookup("", sym).value();
        std::vector<uint8_t> c = {0x48, 0x83, 0xEC, 0x08, 0x48, 0xB8};
        put_u64(c, bits(x));
        c.insert(c.end(), {0x66, 0x48, 0x0F, 0x6E, 0xC0, 0x48, 0xB8});               // movq xmm0, rax ; mov rax, thunk
        put_u64(c, s.address);
        c.insert(c.end(), {0xFF, 0xD0, 0x66, 0x48, 0x0F, 0x7E, 0xC0, 0x48, 0x83, 0xC4, 0x08, 0xC3});   // call ; movq rax, xmm0 ; add rsp,8 ; ret
        uint64_t r = run_code(c).value;
        double d; std::memcpy(&d, &r, 8);
        return d;
    };
    CHECK_EQ(unary("_sqrt", 81.0), 9.0);
    CHECK_EQ(unary("_fabs", -2.5), 2.5);
    CHECK_EQ(unary("_floor", -1.5), -2.0);
    CHECK_EQ(unary("_ceil", 1.2), 2.0);
    CHECK(std::isnan(unary("_sqrt", -1.0)));
}

CIDER_TEST(exit_requests_a_guest_exit_with_the_low_32_bits) {
    Env e;
    auto o = e.call("_exit", 37);
    CHECK(o.exited);
    CHECK_EQ(o.value, uint64_t{37});
    o = e.call("_exit", 0xFFFFFFFFFFFFFFFFull);           // exit(-1): the upper register bits are garbage and must be ignored
    CHECK(o.exited);
    CHECK_EQ(static_cast<int32_t>(o.value), -1);
}

CIDER_TEST(dyld_stub_binder_traps_instead_of_silently_continuing) {
    Env e;
    auto o = e.call("dyld_stub_binder");
    CHECK(o.exited);
    CHECK_EQ(o.value, uint64_t{134});
    CHECK(e.ctx.fault.find("lazy binding") != std::string::npos);
}

CIDER_TEST(functions_work_without_a_runtime_context_by_falling_back_to_the_real_streams) {
    // no context: nothing to capture, nothing may crash (malloc tracking simply does not exist)
    set_context(nullptr);
    auto provider = take(LibSystemProvider::create());
    CHECK_EQ(call_host(*provider, "_strlen", Env::p("abc")).value, uint64_t{3});
    CHECK_EQ(call_host(*provider, "_getenv", Env::p("X")).value, uint64_t{0});
    uint64_t p = call_host(*provider, "_malloc", 10).value;
    CHECK(p != 0);
    call_host(*provider, "_free", p);          // not tracked: ignored with a warning (leaks 10 bytes of the process heap, by design)
#ifdef _WIN32
    HeapFree(GetProcessHeap(), 0, reinterpret_cast<void*>(p));
#endif
}

CIDER_TEST(guest_heap_is_destroyed_with_the_runtime_context_and_failures_return_null) {
    uint64_t p = 0;
    {
        Env e;
        p = e.call("_malloc", 8u << 20).value;
        REQUIRE(p != 0);
        CHECK(memory::query_address(reinterpret_cast<void*>(p)).state == memory::PageInfo::State::Committed);
        CHECK_EQ(e.call("_malloc", ~uint64_t{0} / 2).value, uint64_t{0});               // impossible request: NULL, not a crash/abort
        CHECK_EQ(e.call("_calloc", ~uint64_t{0} / 2, 4).value, uint64_t{0});            // a*b overflows: NULL
        uint64_t q = e.call("_malloc", 64).value;
        CHECK_EQ(e.call("_realloc", q, ~uint64_t{0} / 2).value, uint64_t{0});           // failed realloc leaves the block valid
        CHECK(e.ctx.allocations.count(reinterpret_cast<void*>(q)) == 1);
    }
    CHECK(memory::query_address(reinterpret_cast<void*>(p)).state != memory::PageInfo::State::Committed);   // freed with the context
}
