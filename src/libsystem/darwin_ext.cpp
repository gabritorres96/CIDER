// libSystem functions needed by the first unmodified external macOS binary (jq 1.7.1): libm beyond the basics, ctype, time conversion, locale,
// string helpers and fortify variants. Names and signatures come from the binary's own import table; layouts that matter (the rune-locale table the
// inlined isalpha/isspace read at offset 60) were confirmed in its disassembly.
//
// Scope decisions, all deliberate and documented in docs/LIMITATIONS.md:
//   * The guest's locale is "C" only (setlocale refuses everything else) and its time zone is UTC (localtime_r == gmtime_r, tm_zone "UTC").
//   * rand() is the FreeBSD/Darwin Park-Miller generator seeded with 1; nothing here claims bit-exact agreement with macOS beyond that algorithm.
//   * signals do not exist: raise(SIGABRT) ends the program like abort(), any other signal stops it with a diagnostic.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>

#include "internal.hpp"
#include "orvyn/libsystem.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#endif

namespace orvyn::libsystem {
namespace {

using abi::GuestRegs;
using abi::RegsReader;
using detail::fail_errno;

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }
void ret_double(GuestRegs* r, double v) { std::memcpy(&r->ret_xmm0, &v, 8); }

constexpr int kENOTTY = 25, kEOVERFLOW = 84;

// ---- libm: one or two doubles in xmm0/xmm1 and a double back in xmm0 use the same registers on both ABIs ---------------------------------------
double w_acos(double x) { return std::acos(x); }
double w_acosh(double x) { return std::acosh(x); }
double w_asin(double x) { return std::asin(x); }
double w_asinh(double x) { return std::asinh(x); }
double w_atanh(double x) { return std::atanh(x); }
double w_cbrt(double x) { return std::cbrt(x); }
double w_cosh(double x) { return std::cosh(x); }
double w_sinh(double x) { return std::sinh(x); }
double w_tanh(double x) { return std::tanh(x); }
double w_erf(double x) { return std::erf(x); }
double w_erfc(double x) { return std::erfc(x); }
double w_exp2(double x) { return std::exp2(x); }
double w_expm1(double x) { return std::expm1(x); }
double w_log1p(double x) { return std::log1p(x); }
double w_log2(double x) { return std::log2(x); }
double w_logb(double x) { return std::logb(x); }
double w_tgamma(double x) { return std::tgamma(x); }
double w_lgamma(double x) { return std::lgamma(x); }
double w___exp10(double x) { return std::pow(10.0, x); }
double w_j0(double x) { return ::_j0(x); }
double w_j1(double x) { return ::_j1(x); }
double w_y0(double x) { return ::_y0(x); }
double w_y1(double x) { return ::_y1(x); }
double w_fdim(double a, double b) { return std::fdim(a, b); }
double w_hypot(double a, double b) { return std::hypot(a, b); }
double w_nextafter(double a, double b) { return std::nextafter(a, b); }
double w_remainder(double a, double b) { return std::remainder(a, b); }
// scalb(x, y): x * 2^y for an integral y (NaN otherwise), y a double
double w_scalb(double x, double y) {
    if (std::isnan(x) || std::isnan(y)) return x + y;
    if (std::isinf(y)) return y > 0 ? x * y : x / -y;
    if (y != std::rint(y)) return std::nan("");
    return std::ldexp(x, y > 100000 ? 100000 : y < -100000 ? -100000 : static_cast<int>(y));
}

// int + double / double + long / double + pointer mixes: the integer and SSE registers are separate classes, so a positional thunk is wrong
void w_jn(GuestRegs* r) { RegsReader a(*r); const int n = static_cast<int>(static_cast<int32_t>(a.next_int())); ret_double(r, ::_jn(n, a.next_double())); }
void w_yn(GuestRegs* r) { RegsReader a(*r); const int n = static_cast<int>(static_cast<int32_t>(a.next_int())); ret_double(r, ::_yn(n, a.next_double())); }
void w_scalbln(GuestRegs* r) {
    RegsReader a(*r);
    const double x = a.next_double();
    const int64_t n = static_cast<int64_t>(a.next_int());
    ret_double(r, std::scalbln(x, static_cast<long>(n > 1000000 ? 1000000 : n < -1000000 ? -1000000 : n)));
}
// double lgamma_r(double x, int *signp): sign of Gamma(x) in *signp
void w_lgamma_r(GuestRegs* r) {
    RegsReader a(*r);
    const double x = a.next_double();
    int* sign = reinterpret_cast<int*>(a.next_int());
    int s = 1;
    if (x < 0 && x != std::floor(x)) s = (static_cast<int64_t>(std::floor(x)) & 1) ? -1 : 1;     // Gamma is negative on (-1,0), (-3,-2), ...
    if (sign) *sign = s;
    ret_double(r, std::lgamma(x));
}

// double nexttoward(double x, long double y): y is an x87 extended value passed in MEMORY (16 bytes on the stack), x in xmm0.
struct Ext { bool neg; int32_t exp; uint64_t mant; bool nan, inf, zero; };      // value = (-1)^neg * mant * 2^(exp-16383-63)
Ext ext_of_bytes(const uint8_t* p) {
    uint64_t mant;
    uint16_t se;
    std::memcpy(&mant, p, 8);
    std::memcpy(&se, p + 8, 2);
    Ext e{};
    e.neg = (se & 0x8000) != 0;
    e.exp = se & 0x7fff;
    e.mant = mant;
    e.nan = e.exp == 0x7fff && (mant << 1) != 0;
    e.inf = e.exp == 0x7fff && (mant << 1) == 0;
    e.zero = mant == 0 && e.exp != 0x7fff;
    return e;
}
Ext ext_of_double(double d) {
    Ext e{};
    uint64_t b;
    std::memcpy(&b, &d, 8);
    e.neg = (b >> 63) != 0;
    const int ex = static_cast<int>((b >> 52) & 0x7ff);
    const uint64_t fr = b & 0xfffffffffffffull;
    if (ex == 0x7ff) { e.nan = fr != 0; e.inf = fr == 0; e.exp = 0x7fff; e.mant = fr ? 0xc000000000000000ull : 0x8000000000000000ull; return e; }
    if (ex == 0 && fr == 0) { e.zero = true; return e; }
    uint64_t m;
    int exp2;
    if (ex == 0) { m = fr; exp2 = -1022 - 52; }                                  // subnormal: value = fr * 2^-1074
    else { m = fr | (1ull << 52); exp2 = ex - 1023 - 52; }
    int shift = 0;
    while (!(m & (1ull << 63))) { m <<= 1; ++shift; }
    e.mant = m;
    e.exp = exp2 - shift + 63 + 16383;
    return e;
}
// -1, 0, +1 for a < b, a == b, a > b (neither NaN)
int ext_cmp(const Ext& a, const Ext& b) {
    if (a.zero && b.zero) return 0;
    if (a.zero) return b.neg ? 1 : -1;
    if (b.zero) return a.neg ? -1 : 1;
    if (a.neg != b.neg) return a.neg ? -1 : 1;
    int mag;
    if (a.inf || b.inf) mag = a.inf == b.inf ? 0 : (a.inf ? 1 : -1);
    else if (a.exp != b.exp) mag = a.exp > b.exp ? 1 : -1;
    else mag = a.mant == b.mant ? 0 : (a.mant > b.mant ? 1 : -1);
    return a.neg ? -mag : mag;
}
double ext_to_double(const Ext& e) {
    if (e.nan) return std::nan("");
    if (e.inf) return e.neg ? -HUGE_VAL : HUGE_VAL;
    if (e.zero) return e.neg ? -0.0 : 0.0;
    const double m = static_cast<double>(e.mant);                                  // round-to-nearest of the 64-bit mantissa
    const double v = std::ldexp(m, e.exp - 16383 - 63);
    return e.neg ? -v : v;
}
void w_nexttoward(GuestRegs* r) {
    RegsReader a(*r);
    const double x = a.next_double();
    const uint8_t* yb = reinterpret_cast<const uint8_t*>(r->stack_args);
    if (!yb) { ret_double(r, std::nan("")); return; }
    const Ext y = ext_of_bytes(yb), xe = ext_of_double(x);
    if (std::isnan(x) || y.nan) { ret_double(r, std::nan("")); return; }
    const int c = ext_cmp(xe, y);
    if (c == 0) { ret_double(r, ext_to_double(y)); return; }
    ret_double(r, std::nextafter(x, c < 0 ? HUGE_VAL : -HUGE_VAL));
}

// ---- ctype: the table the inlined isalpha()/isspace() read, plus ___maskrune for everything else -------------------------------------------------
constexpr uint32_t kA = 0x100, kC = 0x200, kD = 0x400, kG = 0x800, kL = 0x1000, kP = 0x2000, kS = 0x4000, kU = 0x8000, kX = 0x10000, kB = 0x20000, kR = 0x40000;
uint32_t rune_flags(int c) {
    uint32_t f = 0;
    if (c < 32 || c == 127) f |= kC;
    if (c == ' ' || (c >= 9 && c <= 13)) f |= kS;
    if (c == ' ' || c == '\t') f |= kB;
    if (c >= 32 && c <= 126) f |= kR;
    if (c >= 33 && c <= 126) f |= kG;
    if (c >= '0' && c <= '9') f |= kD | kX | static_cast<uint32_t>(c - '0');
    if (c >= 'A' && c <= 'Z') { f |= kA | kU; if (c <= 'F') f |= kX | static_cast<uint32_t>(c - 'A' + 10); }
    if (c >= 'a' && c <= 'z') { f |= kA | kL; if (c <= 'f') f |= kX | static_cast<uint32_t>(c - 'a' + 10); }
    if (c >= 33 && c <= 126 && !(f & (kA | kD))) f |= kP;
    return f;
}
// int ___maskrune(__darwin_ct_rune_t c, unsigned long mask)
uint64_t w_maskrune(int c, uint64_t mask) { return (c >= 0 && c < 128) ? (rune_flags(c) & static_cast<uint32_t>(mask)) : 0; }

}  // namespace

// Fills the 4 KiB read-only page the provider exposes as __DefaultRuneLocale (struct _RuneLocale: magic[8], encoding[32], two function pointers,
// invalid_rune at 56, __runetype[256] at 60, __maplower[256] at 1084, __mapupper[256] at 2108 - offset 60 and the A/S flags are confirmed by jq's code).
void fill_default_rune_locale(uint8_t* page) {
    std::memset(page, 0, 4096);
    std::memcpy(page, "RuneMagA", 8);
    std::memcpy(page + 8, "NONE", 4);
    const int32_t invalid = 0xFFFD;
    std::memcpy(page + 56, &invalid, 4);
    for (int c = 0; c < 256; ++c) {
        const uint32_t f = c < 128 ? rune_flags(c) : 0;
        const int32_t lower = (c >= 'A' && c <= 'Z') ? c + 32 : c, upper = (c >= 'a' && c <= 'z') ? c - 32 : c;
        std::memcpy(page + 60 + 4 * c, &f, 4);
        std::memcpy(page + 1084 + 4 * c, &lower, 4);
        std::memcpy(page + 2108 + 4 * c, &upper, 4);
    }
}

namespace {

// ---- strings / memory ------------------------------------------------------------------------------------------------------------------------------
void* w_memmem(const void* h, uint64_t hl, const void* n, uint64_t nl) {
    if (nl == 0) return const_cast<void*>(h);
    if (!h || hl < nl) return nullptr;
    const auto* hp = static_cast<const uint8_t*>(h);
    const auto* np = static_cast<const uint8_t*>(n);
    for (uint64_t i = 0; i + nl <= hl; ++i)
        if (hp[i] == np[0] && std::memcmp(hp + i, np, nl) == 0) return const_cast<uint8_t*>(hp + i);
    return nullptr;
}
uint64_t w_strspn(const char* s, const char* accept) { return std::strspn(s, accept); }
void w___bzero(void* p, uint64_t n) { if (p) std::memset(p, 0, n); }
void* w___memcpy_chk(void* d, const void* s, uint64_t n, uint64_t dlen) {
    if (n > dlen) { detail::trap("memcpy: prevented write past end of buffer (___memcpy_chk): the program tried to copy " + std::to_string(n) + " bytes into a " + std::to_string(dlen) + "-byte buffer"); return d; }
    return std::memcpy(d, s, n);
}
void w_memset_pattern16(uint8_t* b, const uint8_t* pattern, uint64_t len) {
    if (!b || !pattern) return;
    for (uint64_t i = 0; i < len; ++i) b[i] = pattern[i & 15];
}

// Darwin error messages (BSD wording). Only the codes ORVYN can produce or a typical program reports; anything else is "Unknown error: N".
const char* const kErr[] = {"Undefined error: 0", "Operation not permitted", "No such file or directory", "No such process", "Interrupted system call", "Input/output error",
    "Device not configured", "Argument list too long", "Exec format error", "Bad file descriptor", "No child processes", "Resource deadlock avoided", "Cannot allocate memory",
    "Permission denied", "Bad address", "Block device required", "Resource busy", "File exists", "Cross-device link", "Operation not supported by device", "Not a directory",
    "Is a directory", "Invalid argument", "Too many open files in system", "Too many open files", "Inappropriate ioctl for device", "Text file busy", "File too large",
    "No space left on device", "Illegal seek", "Read-only file system", "Too many links", "Broken pipe", "Numerical argument out of domain", "Result too large",
    "Resource temporarily unavailable"};
const char* w_strerror(int e) {
    static thread_local char unknown[48];
    if (e >= 0 && e < static_cast<int>(sizeof kErr / sizeof kErr[0])) return kErr[e];
    if (e == 63) return "File name too long";
    if (e == 66) return "Directory not empty";
    if (e == 78) return "Function not implemented";
    if (e == 84) return "Value too large to be stored in data type";
    std::snprintf(unknown, sizeof unknown, "Unknown error: %d", e);
    return unknown;
}
void w_perror(const char* s) {
    std::string m;
    if (s && *s) { m = s; m += ": "; }
    m += w_strerror(detail::errno_ref());
    m += "\n";
    console_write(2, m.data(), m.size());
}

char* w_dirname(char* path) {
    static thread_local char buf[1100];
    if (!path || !*path) { std::strcpy(buf, "."); return buf; }
    std::string s(path);
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    const size_t slash = s.rfind('/');
    std::string out;
    if (slash == std::string::npos) out = ".";
    else {
        out = s.substr(0, slash);
        while (out.size() > 1 && out.back() == '/') out.pop_back();
        if (out.empty()) out = "/";
    }
    if (out.size() >= sizeof buf) out = ".";
    std::strcpy(buf, out.c_str());
    return buf;
}

// ---- process / environment ---------------------------------------------------------------------------------------------------------------------
int w_raise(int sig) {
    if (sig <= 0 || sig > 31) return static_cast<int>(fail_errno(detail::kEINVAL));
    if (sig == 6) { detail::trap("raise(SIGABRT) was called by the program (equivalent to abort())"); return 0; }
    detail::trap("raise(" + std::to_string(sig) + "): signals do not exist in ORVYN, so the program was stopped instead of the signal being ignored");
    return 0;
}
int w_isatty(int fd) {
    RuntimeContext* c = context();
    if (!c || fd < 0 || static_cast<size_t>(fd) >= c->fds.size() || c->fds[static_cast<size_t>(fd)].kind != GuestFd::Kind::Console) {
        fail_errno(fd < 0 || !c || static_cast<size_t>(fd) >= c->fds.size() || c->fds[static_cast<size_t>(fd)].kind == GuestFd::Kind::Closed ? detail::kEBADF : kENOTTY);
        return 0;
    }
#ifdef _WIN32
    if (!c->sink) {                                            // not captured: ask the real console handle
        HANDLE h = GetStdHandle(fd == 0 ? STD_INPUT_HANDLE : fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
        DWORD mode = 0;
        if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) return 1;
    }
#endif
    fail_errno(kENOTTY);
    return 0;
}
const char* w_setlocale(int /*category*/, const char* locale) {
    // only the "C" locale exists; asking for it (or for the environment default, which is "C" here) succeeds, anything else fails like an unknown locale
    if (!locale || !*locale || !std::strcmp(locale, "C") || !std::strcmp(locale, "POSIX")) return "C";
    return nullptr;
}
int64_t w_pathconf(const char* path, int name) {
    if (!path) return fail_errno(detail::kEFAULT);
    std::filesystem::path host;
    if (int e = detail::resolve_guest_path(path, host)) return fail_errno(e);
    std::error_code ec;
    if (!std::filesystem::exists(host, ec)) return fail_errno(detail::kENOENT);
    switch (name) {               // Darwin <unistd.h> _PC_* values, APFS limits
        case 1: return 32767;     // _PC_LINK_MAX
        case 4: return 255;       // _PC_NAME_MAX
        case 5: return 1024;      // _PC_PATH_MAX
        case 6: return 512;       // _PC_PIPE_BUF
        case 7: return 1;         // _PC_CHOWN_RESTRICTED
        case 8: return 1;         // _PC_NO_TRUNC
        default: return fail_errno(detail::kEINVAL);
    }
}
// char *realpath(const char *path, char *resolved): confined to the guest root, like every other path function
char* w_realpath(const char* path, char* resolved) {
    RuntimeContext* c = context();
    if (!path || !c) { fail_errno(detail::kEFAULT); return nullptr; }
    std::filesystem::path host;
    if (int e = detail::resolve_guest_path(path, host)) { fail_errno(e); return nullptr; }
    std::error_code ec;
    const auto canon = std::filesystem::canonical(host, ec);
    if (ec) { fail_errno(detail::kENOENT); return nullptr; }
    const auto root = std::filesystem::weakly_canonical(c->root, ec);
    const auto rel = canon.lexically_relative(root);
    if (rel.empty() || *rel.begin() == "..") { fail_errno(detail::kEACCES); return nullptr; }
    std::string g = "/";
    for (const auto& part : rel) {
        if (part == ".") continue;
        if (g.size() > 1) g += '/';
        g += part.generic_string();
    }
    if (g.size() >= 1024) { fail_errno(detail::kENAMETOOLONG); return nullptr; }
    char* out = resolved ? resolved : static_cast<char*>(detail::guest_malloc(1024));
    if (!out) { fail_errno(detail::kENOMEM); return nullptr; }
    std::memcpy(out, g.c_str(), g.size() + 1);
    return out;
}

uint32_t g_rand_state = 1;                       // FreeBSD/Darwin rand(): Park-Miller "minimal standard" (x = 16807 x mod 2^31-1), seeded with 1
int w_rand() {
    int64_t hi = g_rand_state / 127773, lo = g_rand_state % 127773;
    int64_t x = 16807 * lo - 2836 * hi;
    if (x < 0) x += 0x7fffffff;
    g_rand_state = static_cast<uint32_t>(x);
    return static_cast<int>(g_rand_state % 0x80000000ull);
}

// ---- time: UTC only ------------------------------------------------------------------------------------------------------------------------------------
struct DarwinTm {                                  // <time.h> on macOS x86_64: 56 bytes
    int32_t tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    int32_t pad;
    int64_t tm_gmtoff;
    const char* tm_zone;
};
static_assert(sizeof(DarwinTm) == 56, "struct tm layout");

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}
void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}
bool is_leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

DarwinTm* w_gmtime_r(const int64_t* t, DarwinTm* out) {
    if (!t || !out) { fail_errno(detail::kEFAULT); return nullptr; }
    int64_t secs = *t;
    int64_t days = secs / 86400;
    int64_t rem = secs % 86400;
    if (rem < 0) { rem += 86400; --days; }
    int64_t y;
    unsigned m, d;
    civil_from_days(days, y, m, d);
    if (y - 1900 > INT32_MAX || y - 1900 < INT32_MIN) { fail_errno(kEOVERFLOW); return nullptr; }
    out->tm_sec = static_cast<int>(rem % 60);
    out->tm_min = static_cast<int>(rem / 60 % 60);
    out->tm_hour = static_cast<int>(rem / 3600);
    out->tm_mday = static_cast<int>(d);
    out->tm_mon = static_cast<int>(m) - 1;
    out->tm_year = static_cast<int>(y - 1900);
    int64_t wd = (days + 4) % 7;                   // 1970-01-01 was a Thursday
    if (wd < 0) wd += 7;
    out->tm_wday = static_cast<int>(wd);
    out->tm_yday = static_cast<int>(days - days_from_civil(y, 1, 1));
    out->tm_isdst = 0;
    out->pad = 0;
    out->tm_gmtoff = 0;
    out->tm_zone = "UTC";
    return out;
}
DarwinTm* w_localtime_r(const int64_t* t, DarwinTm* out) { return w_gmtime_r(t, out); }      // the guest's time zone is UTC

// time_t timegm(struct tm *): normalises the fields and updates them
int64_t w_timegm(DarwinTm* tm) {
    if (!tm) return fail_errno(detail::kEFAULT);
    int64_t y = static_cast<int64_t>(tm->tm_year) + 1900;
    int64_t mon = tm->tm_mon;
    y += mon / 12;
    mon %= 12;
    if (mon < 0) { mon += 12; --y; }
    const int64_t days = days_from_civil(y, static_cast<unsigned>(mon + 1), 1) + (static_cast<int64_t>(tm->tm_mday) - 1);
    const int64_t secs = days * 86400 + static_cast<int64_t>(tm->tm_hour) * 3600 + static_cast<int64_t>(tm->tm_min) * 60 + tm->tm_sec;
    DarwinTm norm;
    if (!w_gmtime_r(&secs, &norm)) return fail_errno(kEOVERFLOW);
    *tm = norm;
    return secs;
}

const char* const kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* const kMonths[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

void iso_week(const DarwinTm& t, int64_t& iso_year, int& week) {
    const int wd = (t.tm_wday + 6) % 7;                              // Monday = 0
    int w = (t.tm_yday - wd + 10) / 7;
    int64_t y = static_cast<int64_t>(t.tm_year) + 1900;
    auto weeks_in = [](int64_t yy) {
        auto p = [](int64_t v) { return (v + v / 4 - v / 100 + v / 400) % 7; };
        return (p(yy) == 4 || p(yy - 1) == 3) ? 53 : 52;
    };
    if (w < 1) { --y; w = weeks_in(y); }
    else if (w > weeks_in(y)) { w = 1; ++y; }
    iso_year = y;
    week = w;
}

// size_t strftime(char *s, size_t max, const char *format, const struct tm *tm): C locale, English names
uint64_t w_strftime(char* s, uint64_t max, const char* fmt, const DarwinTm* t) {
    if (!s || !fmt || !t || max == 0) return 0;
    std::string out;
    std::function<void(const char*)> run = [&](const char* f) {
        char buf[64];
        for (; *f; ++f) {
            if (*f != '%') { out += *f; continue; }
            ++f;
            while (*f == 'E' || *f == 'O' || *f == '-' || *f == '_' || *f == '0' || *f == '^' || *f == '#') ++f;       // modifiers are ignored in the C locale
            if (!*f) break;
            const int yr = t->tm_year + 1900;
            switch (*f) {
                case 'a': out += std::string(kDays[((t->tm_wday % 7) + 7) % 7]).substr(0, 3); break;
                case 'A': out += kDays[((t->tm_wday % 7) + 7) % 7]; break;
                case 'b': case 'h': out += std::string(kMonths[((t->tm_mon % 12) + 12) % 12]).substr(0, 3); break;
                case 'B': out += kMonths[((t->tm_mon % 12) + 12) % 12]; break;
                case 'c': run("%a %b %e %H:%M:%S %Y"); break;
                case 'C': std::snprintf(buf, sizeof buf, "%02d", yr / 100); out += buf; break;
                case 'd': std::snprintf(buf, sizeof buf, "%02d", t->tm_mday); out += buf; break;
                case 'D': run("%m/%d/%y"); break;
                case 'e': std::snprintf(buf, sizeof buf, "%2d", t->tm_mday); out += buf; break;
                case 'F': run("%Y-%m-%d"); break;
                case 'g': { int64_t iy; int w; iso_week(*t, iy, w); std::snprintf(buf, sizeof buf, "%02d", static_cast<int>(iy % 100)); out += buf; break; }
                case 'G': { int64_t iy; int w; iso_week(*t, iy, w); out += std::to_string(iy); break; }
                case 'H': std::snprintf(buf, sizeof buf, "%02d", t->tm_hour); out += buf; break;
                case 'I': std::snprintf(buf, sizeof buf, "%02d", t->tm_hour % 12 == 0 ? 12 : t->tm_hour % 12); out += buf; break;
                case 'j': std::snprintf(buf, sizeof buf, "%03d", t->tm_yday + 1); out += buf; break;
                case 'k': std::snprintf(buf, sizeof buf, "%2d", t->tm_hour); out += buf; break;
                case 'l': std::snprintf(buf, sizeof buf, "%2d", t->tm_hour % 12 == 0 ? 12 : t->tm_hour % 12); out += buf; break;
                case 'm': std::snprintf(buf, sizeof buf, "%02d", t->tm_mon + 1); out += buf; break;
                case 'M': std::snprintf(buf, sizeof buf, "%02d", t->tm_min); out += buf; break;
                case 'n': out += '\n'; break;
                case 'p': out += t->tm_hour >= 12 ? "PM" : "AM"; break;
                case 'r': run("%I:%M:%S %p"); break;
                case 'R': run("%H:%M"); break;
                case 's': { DarwinTm copy = *t; out += std::to_string(w_timegm(&copy)); break; }
                case 'S': std::snprintf(buf, sizeof buf, "%02d", t->tm_sec); out += buf; break;
                case 't': out += '\t'; break;
                case 'T': case 'X': run("%H:%M:%S"); break;
                case 'u': out += std::to_string(t->tm_wday == 0 ? 7 : t->tm_wday); break;
                case 'U': std::snprintf(buf, sizeof buf, "%02d", (t->tm_yday + 7 - t->tm_wday) / 7); out += buf; break;
                case 'V': { int64_t iy; int w; iso_week(*t, iy, w); std::snprintf(buf, sizeof buf, "%02d", w); out += buf; break; }
                case 'w': out += std::to_string(t->tm_wday); break;
                case 'W': std::snprintf(buf, sizeof buf, "%02d", (t->tm_yday + 7 - (t->tm_wday + 6) % 7) / 7); out += buf; break;
                case 'x': run("%m/%d/%y"); break;
                case 'y': std::snprintf(buf, sizeof buf, "%02d", ((yr % 100) + 100) % 100); out += buf; break;
                case 'Y': out += std::to_string(yr); break;
                case 'z': { const int64_t off = t->tm_gmtoff; std::snprintf(buf, sizeof buf, "%c%02d%02d", off < 0 ? '-' : '+', static_cast<int>(std::llabs(off) / 3600), static_cast<int>(std::llabs(off) / 60 % 60)); out += buf; break; }
                case 'Z': out += t->tm_zone ? t->tm_zone : ""; break;
                case '%': out += '%'; break;
                default: out += '%'; out += *f; break;
            }
        }
    };
    run(fmt);
    if (out.size() + 1 > max) return 0;               // does not fit: contents of s are indeterminate, return 0
    std::memcpy(s, out.c_str(), out.size() + 1);
    return out.size();
}

// char *strptime(const char *buf, const char *format, struct tm *tm): the common conversions; fields it does not parse are left alone
bool strncasecmp_ascii(const char* a, const char* b, int n) {
    for (int i = 0; i < n; ++i) {
        char x = a[i], y = b[i];
        if (!x || !y) return false;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}
bool match_name(const char*& p, const char* const* names, int n, int& index) {
    for (int i = 0; i < n; ++i) {
        for (int len : {static_cast<int>(std::strlen(names[i])), 3}) {
            if (len > static_cast<int>(std::strlen(names[i]))) continue;
            if (strncasecmp_ascii(p, names[i], len)) { p += len; index = i; return true; }
        }
    }
    return false;
}
bool parse_num(const char*& p, int min, int max, int maxdigits, int& out) {
    if (*p < '0' || *p > '9') return false;
    int v = 0, d = 0;
    while (*p >= '0' && *p <= '9' && d < maxdigits) { v = v * 10 + (*p - '0'); ++p; ++d; }
    if (v < min || v > max) return false;
    out = v;
    return true;
}
unsigned g_strptime_flags = 0;                     // single guest thread at a time: bits for the fields the current strptime() call set (1 = month, 2 = day of month, 4 = day of year)
const char* parse_time(const char* p, const char* f, DarwinTm* tm, int depth);
const char* parse_time(const char* p, const char* f, DarwinTm* tm, int depth) {
    if (depth > 4) return nullptr;
    bool pm = false, have_pm = false, have_12h = false;
    int hour12 = 0;
    while (*f) {
        if (*f == ' ' || *f == '\t' || *f == '\n') { while (*p == ' ' || *p == '\t' || *p == '\n') ++p; ++f; continue; }
        if (*f != '%') { if (*p != *f) return nullptr; ++p; ++f; continue; }
        ++f;
        while (*f == 'E' || *f == 'O') ++f;
        int v = 0;
        switch (*f) {
            case '%': if (*p != '%') return nullptr; ++p; break;
            case 'a': case 'A': if (!match_name(p, kDays, 7, v)) return nullptr; tm->tm_wday = v; break;
            case 'b': case 'B': case 'h': if (!match_name(p, kMonths, 12, v)) return nullptr; tm->tm_mon = v; g_strptime_flags |= 1; break;
            case 'd': case 'e': while (*p == ' ') ++p; if (!parse_num(p, 1, 31, 2, v)) return nullptr; tm->tm_mday = v; g_strptime_flags |= 2; break;
            case 'H': if (!parse_num(p, 0, 23, 2, v)) return nullptr; tm->tm_hour = v; break;
            case 'I': if (!parse_num(p, 1, 12, 2, v)) return nullptr; hour12 = v; have_12h = true; break;
            case 'j': if (!parse_num(p, 1, 366, 3, v)) return nullptr; tm->tm_yday = v - 1; g_strptime_flags |= 4; break;
            case 'm': if (!parse_num(p, 1, 12, 2, v)) return nullptr; tm->tm_mon = v - 1; g_strptime_flags |= 1; break;
            case 'M': if (!parse_num(p, 0, 59, 2, v)) return nullptr; tm->tm_min = v; break;
            case 'S': if (!parse_num(p, 0, 60, 2, v)) return nullptr; tm->tm_sec = v; break;
            case 'y': if (!parse_num(p, 0, 99, 2, v)) return nullptr; tm->tm_year = v < 69 ? v + 100 : v; break;
            case 'Y': { bool neg = false; if (*p == '-') { neg = true; ++p; } if (!parse_num(p, 0, 9999, 4, v)) return nullptr; tm->tm_year = (neg ? -v : v) - 1900; break; }
            case 'p': case 'P':
                if (strncasecmp_ascii(p, "AM", 2)) { pm = false; have_pm = true; p += 2; }
                else if (strncasecmp_ascii(p, "PM", 2)) { pm = true; have_pm = true; p += 2; }
                else return nullptr;
                break;
            case 'n': case 't': while (*p == ' ' || *p == '\t' || *p == '\n') ++p; break;
            case 'T': p = parse_time(p, "%H:%M:%S", tm, depth + 1); if (!p) return nullptr; break;
            case 'R': p = parse_time(p, "%H:%M", tm, depth + 1); if (!p) return nullptr; break;
            case 'D': case 'x': p = parse_time(p, "%m/%d/%y", tm, depth + 1); if (!p) return nullptr; break;
            case 'F': p = parse_time(p, "%Y-%m-%d", tm, depth + 1); if (!p) return nullptr; break;
            case 'r': p = parse_time(p, "%I:%M:%S %p", tm, depth + 1); if (!p) return nullptr; break;
            case 'c': p = parse_time(p, "%a %b %e %H:%M:%S %Y", tm, depth + 1); if (!p) return nullptr; break;
            case 'X': p = parse_time(p, "%H:%M:%S", tm, depth + 1); if (!p) return nullptr; break;
            case 'Z': while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) ++p; break;      // a zone name is accepted and ignored
            case 'z': {
                if (*p == 'Z') { ++p; tm->tm_gmtoff = 0; break; }
                if (*p != '+' && *p != '-') return nullptr;
                const bool neg = *p == '-';
                ++p;
                int hh = 0, mm = 0;
                if (!parse_num(p, 0, 23, 2, hh)) return nullptr;
                if (*p == ':') ++p;
                if (*p >= '0' && *p <= '9' && !parse_num(p, 0, 59, 2, mm)) return nullptr;
                tm->tm_gmtoff = (neg ? -1 : 1) * (hh * 3600 + mm * 60);
                break;
            }
            case 's': { int64_t secs = 0; bool neg = false; if (*p == '-') { neg = true; ++p; } if (*p < '0' || *p > '9') return nullptr; while (*p >= '0' && *p <= '9') secs = secs * 10 + (*p++ - '0'); if (neg) secs = -secs; if (!w_gmtime_r(&secs, tm)) return nullptr; break; }
            default: return nullptr;
        }
        ++f;
    }
    if (have_12h) tm->tm_hour = (hour12 % 12) + ((have_pm && pm) ? 12 : 0);
    else if (have_pm && pm && tm->tm_hour < 12) tm->tm_hour += 12;
    return p;
}
char* w_strptime(const char* buf, const char* fmt, DarwinTm* tm) {
    if (!buf || !fmt || !tm) return nullptr;
    g_strptime_flags = 0;
    const char* end = parse_time(buf, fmt, tm, 0);
    // BSD strptime: a day of the year without month/day becomes month and day of month, so the fields stay consistent
    if (end && (g_strptime_flags & 4) && !(g_strptime_flags & 3)) {
        const int64_t y = static_cast<int64_t>(tm->tm_year) + 1900;
        const int64_t day = days_from_civil(y, 1, 1) + tm->tm_yday;
        int64_t yy;
        unsigned mm, dd;
        civil_from_days(day, yy, mm, dd);
        if (yy == y) { tm->tm_mon = static_cast<int>(mm) - 1; tm->tm_mday = static_cast<int>(dd); }
    }
    return const_cast<char*>(end);
}

}  // namespace

namespace detail {
void add_darwin_ext_symbols(std::vector<HostSymbol>& t) {
    auto fn = [&](const char* name, void* f) { t.push_back(HostSymbol{name, f}); };
    auto raw = [&](const char* name, void (*f)(GuestRegs*)) { t.push_back(HostSymbol{name, reinterpret_cast<void*>(f), true}); };
    fn("_acos", addr(w_acos)); fn("_acosh", addr(w_acosh)); fn("_asin", addr(w_asin)); fn("_asinh", addr(w_asinh)); fn("_atanh", addr(w_atanh));
    fn("_cbrt", addr(w_cbrt)); fn("_cosh", addr(w_cosh)); fn("_sinh", addr(w_sinh)); fn("_tanh", addr(w_tanh));
    fn("_erf", addr(w_erf)); fn("_erfc", addr(w_erfc)); fn("_exp2", addr(w_exp2)); fn("_expm1", addr(w_expm1)); fn("_log1p", addr(w_log1p));
    fn("_log2", addr(w_log2)); fn("_logb", addr(w_logb)); fn("_tgamma", addr(w_tgamma)); fn("_lgamma", addr(w_lgamma)); fn("___exp10", addr(w___exp10));
    fn("_j0", addr(w_j0)); fn("_j1", addr(w_j1)); fn("_y0", addr(w_y0)); fn("_y1", addr(w_y1));
    fn("_fdim", addr(w_fdim)); fn("_hypot", addr(w_hypot)); fn("_nextafter", addr(w_nextafter)); fn("_remainder", addr(w_remainder)); fn("_scalb", addr(w_scalb));
    raw("_jn", w_jn); raw("_yn", w_yn); raw("_scalbln", w_scalbln); raw("_lgamma_r", w_lgamma_r); raw("_nexttoward", w_nexttoward);
    fn("___maskrune", addr(w_maskrune));
    fn("_memmem", addr(w_memmem)); fn("_strspn", addr(w_strspn)); fn("___bzero", addr(w___bzero)); fn("___memcpy_chk", addr(w___memcpy_chk));
    fn("_memset_pattern16", addr(w_memset_pattern16)); fn("_strerror", addr(w_strerror)); fn("_perror", addr(w_perror)); fn("_dirname", addr(w_dirname));
    fn("_raise", addr(w_raise)); fn("_isatty", addr(w_isatty)); fn("_setlocale", addr(w_setlocale)); fn("_pathconf", addr(w_pathconf));
    fn("_realpath$DARWIN_EXTSN", addr(w_realpath)); fn("_rand", addr(w_rand));
    fn("_gmtime_r", addr(w_gmtime_r)); fn("_localtime_r", addr(w_localtime_r)); fn("_timegm", addr(w_timegm));
    fn("_strftime", addr(w_strftime)); fn("_strptime", addr(w_strptime));
}
}  // namespace detail

}  // namespace orvyn::libsystem
