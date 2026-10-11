// Darwin libc functions that real programs (cliclick, anything built with Xcode defaults) need before their first line of code runs:
// stack protector, __assert_rtn, abort, getopt and its globals, __cxa_atexit, pthread_main_np, memchr, modf, fma, and the allocator
// extras posix_memalign / malloc_size (the latter two live in libc_bridge.cpp next to malloc).
//
// Every function re-implements the DARWIN contract, not the Windows CRT's:
//   * errno: posix_memalign returns the error number (it does not touch errno), like POSIX; the others do not fail.
//   * getopt is the BSD getopt (no argument permutation, "--" ends the options, no optional "::" arguments), NOT GNU getopt, and
//     its globals optarg/optind/optopt/opterr/optreset are real variables in guest-writable memory that the program binds to.
//   * abort / __stack_chk_fail / __assert_rtn end the program like SIGABRT: exit status 134, with the Darwin diagnostic text.
// Thread safety: the guest runs one thread at a time under the cooperative guest lock, so no function here needs its own lock.
#include <cmath>
#include <cstring>
#include <string>

#include "internal.hpp"
#include "orvyn/libsystem.hpp"

extern "C" void orvyn_chkstk_darwin();      // chkstk_darwin.asm

namespace orvyn::libsystem {
namespace {

using abi::GuestRegs;
using abi::RegsReader;

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

void err(const std::string& s) { console_write(2, s.data(), s.size()); }

// ---- stack protector / assert / abort ------------------------------------------------------------------------------------------
void w_stack_chk_fail() {
    err("*** stack smashing detected ***: terminated\n");
    detail::trap("stack smashing detected (__stack_chk_fail was called): the program corrupted its own stack and was stopped like SIGABRT");
}

// void __assert_rtn(const char *func, const char *file, int line, const char *expr) - Darwin's assert() failure handler.
void w_assert_rtn(const char* func, const char* file, int line, const char* expr) {
    std::string m = "Assertion failed: (";
    m += expr ? expr : "?";
    m += "), function ";
    m += func ? func : "?";
    m += ", file ";
    m += file ? file : "?";
    m += ", line " + std::to_string(line) + ".\n";
    err(m);
    detail::trap("assertion failed in the program (__assert_rtn): " + m.substr(0, m.size() - 1));
}

void w_abort() { detail::trap("abort() was called by the program (SIGABRT)"); }

// ---- memory / math -------------------------------------------------------------------------------------------------------------
void* w_memchr(const void* s, int c, uint64_t n) { return n ? const_cast<void*>(std::memchr(s, c, static_cast<size_t>(n))) : nullptr; }
double w_fma(double a, double b, double c) { return std::fma(a, b, c); }

// double modf(double x, double *iptr): System V = x in xmm0, pointer in RDI (a positional Windows thunk would read RCX/RDX).
void w_modf(GuestRegs* r) {
    RegsReader a(*r);
    const double x = a.next_double();
    double* ip = reinterpret_cast<double*>(a.next_int());
    double whole = 0;
    const double frac = std::modf(x, &whole);
    if (ip) *ip = whole;
    std::memcpy(&r->ret_xmm0, &frac, 8);
}

// ---- threads / atexit -----------------------------------------------------------------------------------------------------------
int w_pthread_main_np() {
    ThreadState* t = GuestThreads::current();
    return (!t || t->is_main) ? 1 : 0;
}

// int __cxa_atexit(void (*func)(void *), void *arg, void *dso_handle): like atexit, but the handler receives `arg`.
int w_cxa_atexit(uint64_t fn, uint64_t arg, uint64_t /*dso_handle*/) {
    RuntimeContext* c = context();
    if (!c || !is_valid_callback(fn)) return static_cast<int>(detail::fail_errno(detail::kEINVAL));
    if (c->atexit_handlers.size() >= 4096) return static_cast<int>(detail::fail_errno(detail::kENOMEM));
    c->atexit_handlers.push_back(fn);
    c->atexit_args.resize(c->atexit_handlers.size() - 1, 0);          // plain atexit() handlers recorded no argument
    c->atexit_args.push_back(arg);
    return 0;
}

// ---- BSD getopt ------------------------------------------------------------------------------------------------------------------
// Faithful to FreeBSD/Darwin libc getopt.c. The scanning position lives in the context; the five globals are guest memory.
constexpr int kBadChar = '?', kBadArg = ':';

std::string progname(const char* const* argv) {
    const char* p = argv && argv[0] ? argv[0] : "?";
    const char* base = p;
    for (const char* q = p; *q; ++q)
        if (*q == '/' || *q == '\\') base = q + 1;
    return base;
}

int w_getopt(int argc, const char* const* argv, const char* ostr) {
    RuntimeContext* c = context();
    LibcVars* v = c ? c->libc_vars : nullptr;
    if (!v || !argv || !ostr) return -1;
    const char* place = reinterpret_cast<const char*>(c->getopt_place);
    static const char kEmpty[] = "";
    if (!place) place = kEmpty;
    auto save = [&](const char* p) { c->getopt_place = reinterpret_cast<uint64_t>(p); };

    if (v->optreset || !*place) {                                  // update scanning pointer
        v->optreset = 0;
        if (v->optind >= argc || v->optind < 1 || !(place = argv[v->optind]) || *place != '-') { save(nullptr); return -1; }
        if (place[1] && *++place == '-' && place[1] == '\0') {     // found "--"
            ++v->optind;
            save(nullptr);
            return -1;
        }
    }
    // option letter okay?
    const char* oli = nullptr;
    v->optopt = static_cast<unsigned char>(*place++);
    if (v->optopt == ':' || !(oli = std::strchr(ostr, v->optopt))) {
        if (v->optopt == '-') { save(nullptr); return -1; }        // the user did not specify '-' as an option: it means "end"
        if (!*place) ++v->optind;
        if (v->opterr && *ostr != ':') err(progname(argv) + ": illegal option -- " + static_cast<char>(v->optopt) + "\n");
        save(place);
        return kBadChar;
    }
    if (*++oli != ':') {                                           // no argument needed
        v->optarg = 0;
        if (!*place) ++v->optind;
    } else {                                                       // argument needed
        if (*place) {
            v->optarg = reinterpret_cast<uint64_t>(place);         // "-ovalue"
        } else if (argc <= ++v->optind) {                          // no argument left
            save(nullptr);
            if (*ostr == ':') return kBadArg;
            if (v->opterr) err(progname(argv) + ": option requires an argument -- " + static_cast<char>(v->optopt) + "\n");
            return kBadChar;
        } else {
            v->optarg = reinterpret_cast<uint64_t>(argv[v->optind]);   // "-o value"
        }
        place = kEmpty;
        ++v->optind;
    }
    save(place == kEmpty ? nullptr : place);
    return v->optopt;
}

}  // namespace

namespace detail {
void add_darwin_libc_symbols(std::vector<HostSymbol>& t) {
    t.push_back({"___stack_chk_fail", addr(w_stack_chk_fail)});
    t.push_back({"___assert_rtn", addr(w_assert_rtn)});
    t.push_back({"_abort", addr(w_abort)});
    t.push_back({"_memchr", addr(w_memchr)});
    t.push_back({"_fma", addr(w_fma)});
    t.push_back({"_modf", reinterpret_cast<void*>(+[](GuestRegs* r) { w_modf(r); }), true});
    t.push_back({"_pthread_main_np", addr(w_pthread_main_np)});
    t.push_back({"___cxa_atexit", addr(w_cxa_atexit)});
    t.push_back({"_getopt", addr(w_getopt)});
    // ___chkstk_darwin is machine code the guest calls directly: it must keep RAX (see chkstk_darwin.asm)
    t.push_back({"___chkstk_darwin", reinterpret_cast<void*>(&orvyn_chkstk_darwin), false, false, true});
}
}  // namespace detail

}  // namespace orvyn::libsystem
