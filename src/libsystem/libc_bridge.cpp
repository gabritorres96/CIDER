// Host implementations of the libSystem subset. Every function here is a normal Windows x64 function;
// guest code reaches it through cider_guest_thunk (see abi.hpp), which converts the calling convention.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>

#include "cider/abi.hpp"
#include "cider/libsystem.hpp"
#include "cider/log.hpp"
#include "internal.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace cider::libsystem {
namespace {

void emit(int fd, const char* data, size_t n) { console_write(fd, data, n); }

// ---- I/O --------------------------------------------------------------------------------------
int w_puts(const char* s) {
    if (!s) return -1;
    emit(1, s, std::strlen(s));
    emit(1, "\n", 1);
    return 1;
}
int w_putchar(int c) {
    char ch = static_cast<char>(c);
    emit(1, &ch, 1);
    return c & 0xff;
}

// ---- process ----------------------------------------------------------------------------------
void w_exit(int code) { abi::request_guest_exit(static_cast<uint32_t>(code)); }
int w_getpid() {
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return 1;
#endif
}
const char* w_getenv(const char* name) {
    RuntimeContext* c = context();
    if (!name || !c) return nullptr;
    const size_t n = std::strlen(name);
    for (const auto& e : c->env)
        if (e.size() > n && e[n] == '=' && std::strncmp(e.c_str(), name, n) == 0) return e.c_str() + n + 1;
    return nullptr;
}
void w_lazy_bind_trap() {
    if (RuntimeContext* c = context()) c->fault = "lazy binding is not supported (dyld_stub_binder was called)";
    log::error("guest called dyld_stub_binder: lazy binding is not supported");
    abi::request_guest_exit(134);
}

// ---- strings / memory ------------------------------------------------------------------------
uint64_t w_strlen(const char* s) { return s ? std::strlen(s) : 0; }
int w_strcmp(const char* a, const char* b) { return std::strcmp(a, b); }
int w_strncmp(const char* a, const char* b, uint64_t n) { return std::strncmp(a, b, n); }
void* w_memcpy(void* d, const void* s, uint64_t n) { return std::memcpy(d, s, n); }
void* w_memmove(void* d, const void* s, uint64_t n) { return std::memmove(d, s, n); }
void* w_memset(void* d, int c, uint64_t n) { return std::memset(d, c, n); }
int w_memcmp(const void* a, const void* b, uint64_t n) { return std::memcmp(a, b, n); }
int w_abs(int v) { return v < 0 ? -v : v; }
int64_t w_labs(int64_t v) { return v < 0 ? -v : v; }

// Guest allocations live in a private Windows heap owned by the RuntimeContext (never the CRT/ASan heap):
// allocation failure returns NULL like libSystem does, and destroying the context frees every block.
#ifdef _WIN32
HANDLE heap_of(RuntimeContext* c) {
    if (!c) return GetProcessHeap();
    if (!c->heap) c->heap = HeapCreate(0, 0, 0);
    return static_cast<HANDLE>(c->heap ? c->heap : GetProcessHeap());
}
void track(void* p) { if (p) if (RuntimeContext* c = context()) c->allocations.insert(p); }
void* w_malloc(uint64_t n) {
    void* p = HeapAlloc(heap_of(context()), 0, n ? static_cast<SIZE_T>(n) : 1);
    track(p);
    return p;
}
void* w_calloc(uint64_t a, uint64_t b) {
    if (a && b > UINT64_MAX / a) return nullptr;                 // overflow in a*b: calloc must fail
    uint64_t n = a * b;
    void* p = HeapAlloc(heap_of(context()), HEAP_ZERO_MEMORY, n ? static_cast<SIZE_T>(n) : 1);
    track(p);
    return p;
}
void w_free(void* p) {
    if (!p) return;
    RuntimeContext* c = context();
    if (!c || c->allocations.erase(p) == 0) {
        log::warn(log::Category::LibSystem, "free({}) of a pointer that malloc did not return: ignored", p);
        return;
    }
    HeapFree(heap_of(c), 0, p);
}
void* w_realloc(void* p, uint64_t n) {
    if (!p) return w_malloc(n);
    RuntimeContext* c = context();
    if (!c || c->allocations.count(p) == 0) {
        log::warn(log::Category::LibSystem, "realloc({}) of a pointer that malloc did not return: refused", p);
        return nullptr;
    }
    void* q = HeapReAlloc(heap_of(c), 0, p, n ? static_cast<SIZE_T>(n) : 1);   // NULL on failure, original block intact
    if (q) { c->allocations.erase(p); c->allocations.insert(q); }
    return q;
}
#else
void track(void* p) { if (p) if (RuntimeContext* c = context()) c->allocations.insert(p); }
void* w_malloc(uint64_t n) { void* p = std::malloc(n ? n : 1); track(p); return p; }
void* w_calloc(uint64_t a, uint64_t b) { void* p = std::calloc(a ? a : 1, b ? b : 1); track(p); return p; }
void w_free(void* p) { if (p) { RuntimeContext* c = context(); if (c && c->allocations.erase(p)) std::free(p); } }
void* w_realloc(void* p, uint64_t n) { void* q = std::realloc(p, n ? n : 1); return q; }
#endif

// ---- math (double in xmm0[/xmm1], double out in xmm0: identical register use on both ABIs) ------
double w_sqrt(double x) { return std::sqrt(x); }
double w_fabs(double x) { return std::fabs(x); }
double w_floor(double x) { return std::floor(x); }
double w_ceil(double x) { return std::ceil(x); }
double w_pow(double a, double b) { return std::pow(a, b); }

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

void console_write(int fd, const char* data, size_t n) {
    RuntimeContext* c = context();
    if (c) {
        (fd == 2 ? c->bytes_err : c->bytes_out) += n;
        ++c->calls;
        if (c->sink) { c->sink(fd, data, n); return; }
    }
#ifdef _WIN32
    HANDLE h = GetStdHandle(fd == 2 ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    size_t done = 0;
    while (done < n && h && h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        if (!WriteFile(h, data + done, static_cast<DWORD>(std::min<size_t>(n - done, 1u << 20)), &w, nullptr) || w == 0) break;
        done += w;
    }
#else
    (void)fwrite(data, 1, n, fd == 2 ? stderr : stdout);
#endif
}

namespace detail {
void* guest_malloc(uint64_t n) { return w_malloc(n); }
void trap(const std::string& what) {
    if (RuntimeContext* c = context()) c->fault = what;
    log::error("guest trapped: {}", what);
    abi::request_guest_exit(134);
}
}  // namespace detail

const std::vector<HostSymbol>& implemented_symbols() {
    static const std::vector<HostSymbol> table = [] {
    std::vector<HostSymbol> t = {
        {"_puts", addr(w_puts)},       {"_putchar", addr(w_putchar)}, {"_exit", addr(w_exit)},
        {"_getpid", addr(w_getpid)},   {"_getenv", addr(w_getenv)},   {"_strlen", addr(w_strlen)}, {"_strcmp", addr(w_strcmp)},
        {"_strncmp", addr(w_strncmp)}, {"_memcpy", addr(w_memcpy)},   {"_memmove", addr(w_memmove)}, {"_memset", addr(w_memset)},
        {"_memcmp", addr(w_memcmp)},   {"_abs", addr(w_abs)},         {"_labs", addr(w_labs)},     {"_malloc", addr(w_malloc)},
        {"_calloc", addr(w_calloc)},   {"_realloc", addr(w_realloc)}, {"_free", addr(w_free)},     {"_sqrt", addr(w_sqrt)},
        {"_fabs", addr(w_fabs)},       {"_floor", addr(w_floor)},     {"_ceil", addr(w_ceil)},     {"_pow", addr(w_pow)},
        {"dyld_stub_binder", addr(w_lazy_bind_trap)},
    };
    detail::add_stdio_symbols(t);
    detail::add_posix_symbols(t);
    detail::add_misc_symbols(t);
    return t;
    }();
    return table;
}

bool is_implemented(std::string_view s) {
    for (const auto& h : implemented_symbols())
        if (s == h.name) return true;
    for (const auto& d : implemented_data_symbols())
        if (s == d.name) return true;
    return false;
}

}  // namespace cider::libsystem
