// More libSystem functions: strings, math with mixed signatures, aggregate returns. Where a function cannot be
// expressed with the positional thunk (mixed integer/double arguments, structures returned in registers) it is a
// raw function that reads the System V registers itself.
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "cider/libsystem.hpp"
#include "internal.hpp"

namespace cider::libsystem {
namespace {

using abi::GuestRegs;
using abi::RegsReader;

// ---- strings (positional) ---------------------------------------------------------------------------------------
char* w_strcpy(char* d, const char* s) { std::strcpy(d, s); return d; }
char* w_strncpy(char* d, const char* s, uint64_t n) { std::strncpy(d, s, n); return d; }
char* w_strcat(char* d, const char* s) { std::strcat(d, s); return d; }
char* w_strchr(const char* s, int c) { return const_cast<char*>(std::strchr(s, c)); }
char* w_strrchr(const char* s, int c) { return const_cast<char*>(std::strrchr(s, c)); }
char* w_strstr(const char* h, const char* n) { return const_cast<char*>(std::strstr(h, n)); }
char* w_strdup(const char* s) {
    if (!s) return nullptr;
    const size_t n = std::strlen(s) + 1;
    char* p = static_cast<char*>(detail::guest_malloc(n));
    if (p) std::memcpy(p, s, n);
    return p;
}
int w_atoi(const char* s) { return std::atoi(s); }
int64_t w_strtol(const char* s, char** end, int base) { return std::strtoll(s, end, base); }

// ---- math (positional: all-double or one-double signatures keep their xmm positions on both ABIs) ----------------
double w_sin(double x) { return std::sin(x); }
double w_cos(double x) { return std::cos(x); }
double w_tan(double x) { return std::tan(x); }
double w_exp(double x) { return std::exp(x); }
double w_log(double x) { return std::log(x); }
double w_log10(double x) { return std::log10(x); }
double w_atan(double x) { return std::atan(x); }
double w_atan2(double a, double b) { return std::atan2(a, b); }
double w_fmod(double a, double b) { return std::fmod(a, b); }

// ---- raw: signatures the positional thunk cannot express ----------------------------------------------------------------
// double ldexp(double x, int e): System V = x in xmm0, e in EDI. Windows would expect e in EDX, so a positional thunk is wrong.
void w_ldexp(GuestRegs* r) {
    RegsReader a(*r);
    const double x = a.next_double();
    const int e = static_cast<int>(static_cast<int32_t>(a.next_int()));
    const double v = std::ldexp(x, e);
    std::memcpy(&r->ret_xmm0, &v, 8);
}
// double frexp(double x, int* exp)
void w_frexp(GuestRegs* r) {
    RegsReader a(*r);
    const double x = a.next_double();
    int* e = reinterpret_cast<int*>(a.next_int());
    int tmp = 0;
    const double v = std::frexp(x, &tmp);
    if (e) *e = tmp;
    std::memcpy(&r->ret_xmm0, &v, 8);
}
// div_t div(int, int): struct { int quot; int rem; } is 8 bytes, class INTEGER -> returned in rax.
void w_div(GuestRegs* r) {
    RegsReader a(*r);
    const int32_t n = static_cast<int32_t>(a.next_int()), d = static_cast<int32_t>(a.next_int());
    if (d == 0) { detail::trap("div: division by zero"); return; }
    const int32_t q = (n == INT32_MIN && d == -1) ? INT32_MIN : n / d;
    const int32_t m = (n == INT32_MIN && d == -1) ? 0 : n % d;
    r->ret_rax = static_cast<uint32_t>(q) | (static_cast<uint64_t>(static_cast<uint32_t>(m)) << 32);
}
// ldiv_t ldiv(long, long): struct { long quot; long rem; } is 16 bytes, INTEGER+INTEGER -> rax:rdx.
void w_ldiv(GuestRegs* r) {
    RegsReader a(*r);
    const int64_t n = static_cast<int64_t>(a.next_int()), d = static_cast<int64_t>(a.next_int());
    if (d == 0) { detail::trap("ldiv: division by zero"); return; }
    const bool overflow = n == INT64_MIN && d == -1;
    r->ret_rax = static_cast<uint64_t>(overflow ? INT64_MIN : n / d);
    r->ret_rdx = static_cast<uint64_t>(overflow ? 0 : n % d);
}

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

namespace detail {
void add_misc_symbols(std::vector<HostSymbol>& t) {
    t.push_back({"_strcpy", addr(w_strcpy)});   t.push_back({"_strncpy", addr(w_strncpy)}); t.push_back({"_strcat", addr(w_strcat)});
    t.push_back({"_strchr", addr(w_strchr)});   t.push_back({"_strrchr", addr(w_strrchr)}); t.push_back({"_strstr", addr(w_strstr)});
    t.push_back({"_strdup", addr(w_strdup)});   t.push_back({"_atoi", addr(w_atoi)});       t.push_back({"_strtol", addr(w_strtol)});
    t.push_back({"_sin", addr(w_sin)});         t.push_back({"_cos", addr(w_cos)});         t.push_back({"_tan", addr(w_tan)});
    t.push_back({"_exp", addr(w_exp)});         t.push_back({"_log", addr(w_log)});         t.push_back({"_log10", addr(w_log10)});
    t.push_back({"_atan", addr(w_atan)});       t.push_back({"_atan2", addr(w_atan2)});     t.push_back({"_fmod", addr(w_fmod)});
    t.push_back({"_ldexp", addr(w_ldexp), true});
    t.push_back({"_frexp", addr(w_frexp), true});
    t.push_back({"_div", addr(w_div), true});
    t.push_back({"_ldiv", addr(w_ldiv), true});
}
}  // namespace detail

}  // namespace cider::libsystem
