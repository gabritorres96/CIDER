// stdio subset of ORVYN's libSystem. FILE is OPAQUE: only stdin, stdout and stderr exist (their addresses are the
// values of the variables ___stdinp/___stdoutp/___stderrp the guest binds to), there is no fopen and no buffering:
// every call is written straight through, so fflush has nothing to do and output order between stdout and stderr is
// exactly the call order. printf-family arguments are read with ORVYN's own formatter (printf_format.hpp).
#include <algorithm>
#include <cstring>

#include "orvyn/libsystem.hpp"
#include "orvyn/log.hpp"
#include "orvyn/printf_format.hpp"
#include "internal.hpp"

namespace orvyn::libsystem {
namespace {

using abi::GuestRegs;
using abi::RegsReader;
using detail::fail_errno;

// Slot of the live stream a guest FILE* designates (0 stdin, 1 stdout, 2 stderr, 3.. fopen'ed), -1 when it is not a live stream.
int stream_of(uint64_t f) {
    RuntimeContext* c = context();
    const int idx = detail::stream_index(f);
    if (!c || idx < 0 || !c->streams[static_cast<size_t>(idx)].in_use) return -1;
    return idx;
}

// Formats into `out`; an unsupported specifier ends the guest run (a trap, never a guess).
bool render(const char* fmt, abi::ArgReader& args, std::string& out, const char* fn) {
    FormatStatus st = format_printf(fmt, args, out);
    if (st.ok) return true;
    detail::trap(std::string(fn) + ": " + st.error);
    return false;
}

int64_t write_stream(int stream, const char* data, size_t n) {
    if (stream < 0) return fail_errno(detail::kEBADF);
    return detail::stream_write(stream, data, n);
}

// ---- positional (non-variadic) functions ---------------------------------------------------------------------
int w_fputs(const char* s, uint64_t f) {
    if (!s) return static_cast<int>(fail_errno(detail::kEFAULT));
    const int st = stream_of(f);
    if (st < 0) return static_cast<int>(fail_errno(detail::kEBADF));
    return write_stream(st, s, std::strlen(s)) < 0 ? -1 : 1;
}
int w_fputc(int c, uint64_t f) {
    const int st = stream_of(f);
    if (st < 0) return static_cast<int>(fail_errno(detail::kEBADF));
    const char ch = static_cast<char>(c);
    return write_stream(st, &ch, 1) < 0 ? -1 : (c & 0xff);
}
uint64_t w_fwrite(const void* p, uint64_t size, uint64_t count, uint64_t f) {
    const int st = stream_of(f);
    if (st < 0) { fail_errno(detail::kEBADF); return 0; }
    if (size == 0 || count == 0) return 0;
    if (count > UINT64_MAX / size) { fail_errno(detail::kEINVAL); return 0; }
    if (!p) { fail_errno(detail::kEFAULT); return 0; }
    return write_stream(st, static_cast<const char*>(p), size * count) < 0 ? 0 : count;
}
int w_fflush(uint64_t f) {
    if (f != 0 && stream_of(f) < 0) return static_cast<int>(fail_errno(detail::kEBADF));
    return 0;   // output is never buffered by ORVYN: everything already reached the sink
}

// ---- variadic and va_list functions (raw thunks) -----------------------------------------------------------------------
// printf(fmt, ...)
void w_printf(GuestRegs* r) {
    RegsReader a(*r);
    const char* fmt = reinterpret_cast<const char*>(a.next_int());
    std::string out;
    if (!render(fmt, a, out, "printf")) { r->ret_rax = static_cast<uint64_t>(-1); return; }
    console_write(1, out.data(), out.size());
    r->ret_rax = static_cast<uint32_t>(static_cast<int32_t>(out.size()));
}
// fprintf(FILE*, fmt, ...)
void w_fprintf(GuestRegs* r) {
    RegsReader a(*r);
    const uint64_t f = a.next_int();
    const char* fmt = reinterpret_cast<const char*>(a.next_int());
    const int st = stream_of(f);
    if (st < 0) { r->ret_rax = static_cast<uint64_t>(fail_errno(detail::kEBADF)); return; }
    std::string out;
    if (!render(fmt, a, out, "fprintf")) { r->ret_rax = static_cast<uint64_t>(-1); return; }
    if (write_stream(st, out.data(), out.size()) < 0) { r->ret_rax = static_cast<uint64_t>(-1); return; }
    r->ret_rax = static_cast<uint32_t>(static_cast<int32_t>(out.size()));
}
// sprintf(buf, fmt, ...)   -- unbounded, like C
void w_sprintf(GuestRegs* r) {
    RegsReader a(*r);
    char* buf = reinterpret_cast<char*>(a.next_int());
    const char* fmt = reinterpret_cast<const char*>(a.next_int());
    std::string out;
    if (!buf) { r->ret_rax = static_cast<uint64_t>(fail_errno(detail::kEFAULT)); return; }
    if (!render(fmt, a, out, "sprintf")) { r->ret_rax = static_cast<uint64_t>(-1); return; }
    std::memcpy(buf, out.c_str(), out.size() + 1);
    r->ret_rax = static_cast<uint32_t>(static_cast<int32_t>(out.size()));
}
void store_bounded(char* buf, uint64_t cap, const std::string& out) {
    if (!buf || cap == 0) return;
    const size_t n = std::min<uint64_t>(out.size(), cap - 1);
    std::memcpy(buf, out.data(), n);
    buf[n] = 0;
}
// snprintf(buf, size, fmt, ...)
void w_snprintf(GuestRegs* r) {
    RegsReader a(*r);
    char* buf = reinterpret_cast<char*>(a.next_int());
    const uint64_t cap = a.next_int();
    const char* fmt = reinterpret_cast<const char*>(a.next_int());
    std::string out;
    if (!render(fmt, a, out, "snprintf")) { r->ret_rax = static_cast<uint64_t>(-1); return; }
    store_bounded(buf, cap, out);
    r->ret_rax = static_cast<uint32_t>(static_cast<int32_t>(out.size()));   // the length that WOULD have been written
}
// vprintf(fmt, va_list)
void w_vprintf(GuestRegs* r) {
    RegsReader a(*r);
    const char* fmt = reinterpret_cast<const char*>(a.next_int());
    abi::VaListReader va(a.next_int());
    std::string out;
    if (!render(fmt, va, out, "vprintf")) { r->ret_rax = static_cast<uint64_t>(-1); return; }
    console_write(1, out.data(), out.size());
    r->ret_rax = static_cast<uint32_t>(static_cast<int32_t>(out.size()));
}
// vfprintf(FILE*, fmt, va_list)
void w_vfprintf(GuestRegs* r) {
    RegsReader a(*r);
    const uint64_t f = a.next_int();
    const char* fmt = reinterpret_cast<const char*>(a.next_int());
    abi::VaListReader va(a.next_int());
    const int st = stream_of(f);
    if (st < 0) { r->ret_rax = static_cast<uint64_t>(fail_errno(detail::kEBADF)); return; }
    std::string out;
    if (!render(fmt, va, out, "vfprintf")) { r->ret_rax = static_cast<uint64_t>(-1); return; }
    if (write_stream(st, out.data(), out.size()) < 0) { r->ret_rax = static_cast<uint64_t>(-1); return; }
    r->ret_rax = static_cast<uint32_t>(static_cast<int32_t>(out.size()));
}
// vsprintf(buf, fmt, va_list)
void w_vsprintf(GuestRegs* r) {
    RegsReader a(*r);
    char* buf = reinterpret_cast<char*>(a.next_int());
    const char* fmt = reinterpret_cast<const char*>(a.next_int());
    abi::VaListReader va(a.next_int());
    std::string out;
    if (!buf) { r->ret_rax = static_cast<uint64_t>(fail_errno(detail::kEFAULT)); return; }
    if (!render(fmt, va, out, "vsprintf")) { r->ret_rax = static_cast<uint64_t>(-1); return; }
    std::memcpy(buf, out.c_str(), out.size() + 1);
    r->ret_rax = static_cast<uint32_t>(static_cast<int32_t>(out.size()));
}
// vsnprintf(buf, size, fmt, va_list)
void w_vsnprintf(GuestRegs* r) {
    RegsReader a(*r);
    char* buf = reinterpret_cast<char*>(a.next_int());
    const uint64_t cap = a.next_int();
    const char* fmt = reinterpret_cast<const char*>(a.next_int());
    abi::VaListReader va(a.next_int());
    std::string out;
    if (!render(fmt, va, out, "vsnprintf")) { r->ret_rax = static_cast<uint64_t>(-1); return; }
    store_bounded(buf, cap, out);
    r->ret_rax = static_cast<uint32_t>(static_cast<int32_t>(out.size()));
}

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

namespace detail {
void add_stdio_symbols(std::vector<HostSymbol>& t) {
    t.push_back({"_fputs", addr(w_fputs)});
    t.push_back({"_fputc", addr(w_fputc)});
    t.push_back({"_putc", addr(w_fputc)});
    t.push_back({"_fwrite", addr(w_fwrite)});
    t.push_back({"_fflush", addr(w_fflush)});
    t.push_back({"_printf", addr(w_printf), true});
    t.push_back({"_fprintf", addr(w_fprintf), true});
    t.push_back({"_sprintf", addr(w_sprintf), true});
    t.push_back({"_snprintf", addr(w_snprintf), true});
    t.push_back({"_vprintf", addr(w_vprintf), true});
    t.push_back({"_vfprintf", addr(w_vfprintf), true});
    t.push_back({"_vsprintf", addr(w_vsprintf), true});
    t.push_back({"_vsnprintf", addr(w_vsnprintf), true});
}
}  // namespace detail

const std::vector<DataSymbol>& implemented_data_symbols() {
    static const std::vector<DataSymbol> table = {{"___stdinp"}, {"___stdoutp"}, {"___stderrp"}, {"___stack_chk_guard"}, {"_optarg"}, {"_optind"}, {"_optopt"}, {"_opterr"}, {"_optreset"}};
    return table;
}

}  // namespace orvyn::libsystem
