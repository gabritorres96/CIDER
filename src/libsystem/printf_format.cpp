#include "cider/printf_format.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>

namespace cider::libsystem {
namespace {

struct Spec {
    bool left = false, plus = false, space = false, alt = false, zero = false;
    int width = -1;          // -1: none
    int precision = -1;      // -1: none
    int length = 0;          // 0 none, 1 hh, 2 h, 3 l/ll/j/z/t/q (64-bit)
    char conv = 0;
};

// printf of ONE number through the host CRT, with a format string built from a validated Spec. The CRT only ever sees
// a double / long long it can read in its own convention: no guest va_list or register state reaches it.
template <class T>
std::string host_format(const Spec& s, const char* length_text, char conv, T value) {
    std::string f = "%";
    if (s.left) f += '-';
    if (s.plus) f += '+';
    if (s.space) f += ' ';
    if (s.alt) f += '#';
    if (s.zero) f += '0';
    if (s.width >= 0) f += std::to_string(s.width);
    if (s.precision >= 0) { f += '.'; f += std::to_string(s.precision); }
    f += length_text;
    f += conv;
    int n = std::snprintf(nullptr, 0, f.c_str(), value);
    if (n < 0) return {};
    std::string out(static_cast<size_t>(n) + 1, '\0');
    std::snprintf(out.data(), out.size(), f.c_str(), value);
    out.resize(static_cast<size_t>(n));
    return out;
}

void pad(std::string& out, const std::string& body, const Spec& s) {
    const size_t w = s.width > 0 ? static_cast<size_t>(s.width) : 0;
    if (body.size() >= w) { out += body; return; }
    if (s.left) { out += body; out.append(w - body.size(), ' '); }
    else { out.append(w - body.size(), ' '); out += body; }
}

}  // namespace

FormatStatus format_printf(const char* fmt, abi::ArgReader& args, std::string& out, size_t max_output, size_t max_string) {
    auto fail = [&](std::string why) { return FormatStatus{false, std::move(why)}; };
    if (!fmt) return fail("NULL format string");
    for (const char* p = fmt; *p;) {
        if (out.size() > max_output) return fail("output exceeds the limit");
        if (*p != '%') {
            const char* q = p;
            while (*q && *q != '%') ++q;
            out.append(p, static_cast<size_t>(q - p));
            p = q;
            continue;
        }
        const char* spec_start = p;
        ++p;                                        // after '%'
        Spec s;
        for (;; ++p) {                              // flags
            if (*p == '-') s.left = true;
            else if (*p == '+') s.plus = true;
            else if (*p == ' ') s.space = true;
            else if (*p == '#') s.alt = true;
            else if (*p == '0') s.zero = true;
            else if (*p == '\'') return fail("the ' (thousands grouping) flag is not supported");
            else break;
        }
        auto number = [&](int& dst) -> bool {
            long long v = 0;
            bool any = false;
            while (*p >= '0' && *p <= '9') {
                v = v * 10 + (*p++ - '0');
                any = true;
                if (v > 100000) return false;       // absurd width/precision
            }
            if (any) dst = static_cast<int>(v);
            return true;
        };
        if (*p == '*') {
            ++p;
            const int w = static_cast<int>(static_cast<int32_t>(args.next_int()));
            if (w < 0) { s.left = true; s.width = w == INT32_MIN ? 0 : -w; } else s.width = w;
            if (s.width > 100000) return fail("width too large");
        } else if (!number(s.width)) {
            return fail("width too large");
        }
        if (*p == '.') {
            ++p;
            s.precision = 0;
            if (*p == '*') {
                ++p;
                const int pr = static_cast<int>(static_cast<int32_t>(args.next_int()));
                s.precision = pr < 0 ? -1 : pr;
                if (s.precision > 100000) return fail("precision too large");
            } else if (!number(s.precision)) {
                return fail("precision too large");
            }
        }
        // length modifiers
        if (*p == 'h') { ++p; if (*p == 'h') { ++p; s.length = 1; } else s.length = 2; }
        else if (*p == 'l') { ++p; if (*p == 'l') ++p; s.length = 3; }
        else if (*p == 'j' || *p == 'z' || *p == 't' || *p == 'q') { ++p; s.length = 3; }
        else if (*p == 'L') return fail("the 'L' (long double) length modifier is not supported");

        s.conv = *p;
        if (!s.conv) return fail("format string ends inside a conversion");
        ++p;
        auto spec_text = [&] { return std::string(spec_start, static_cast<size_t>(p - spec_start)); };

        switch (s.conv) {
            case '%': out += '%'; break;
            case 'd': case 'i': {
                const uint64_t raw = args.next_int();
                long long v;
                switch (s.length) {
                    case 1: v = static_cast<int8_t>(raw); break;
                    case 2: v = static_cast<int16_t>(raw); break;
                    case 3: v = static_cast<int64_t>(raw); break;
                    default: v = static_cast<int32_t>(raw); break;
                }
                out += host_format(s, "ll", 'd', v);
                break;
            }
            case 'u': case 'o': case 'x': case 'X': {
                const uint64_t raw = args.next_int();
                unsigned long long v;
                switch (s.length) {
                    case 1: v = static_cast<uint8_t>(raw); break;
                    case 2: v = static_cast<uint16_t>(raw); break;
                    case 3: v = raw; break;
                    default: v = static_cast<uint32_t>(raw); break;
                }
                out += host_format(s, "ll", s.conv, v);
                break;
            }
            case 'c': {
                if (s.length == 3) return fail("%lc (wide characters) is not supported");
                std::string one(1, static_cast<char>(args.next_int()));
                pad(out, one, s);
                break;
            }
            case 's': {
                if (s.length == 3) return fail("%ls (wide strings) is not supported");
                const char* str = reinterpret_cast<const char*>(args.next_int());
                std::string body;
                if (!str) {
                    body = s.precision >= 0 && s.precision < 6 ? std::string("(null)").substr(0, static_cast<size_t>(s.precision)) : "(null)";
                } else {
                    size_t limit = s.precision >= 0 ? static_cast<size_t>(s.precision) : max_string;
                    if (limit > max_string) limit = max_string;
                    size_t n = 0;
                    while (n < limit && str[n]) ++n;
                    if (n == max_string && str[n]) return fail("%s argument is longer than the limit");
                    body.assign(str, n);
                }
                pad(out, body, s);
                break;
            }
            case 'p': {
                char buf[32];
                std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(args.next_int()));
                pad(out, buf, s);
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
                const double v = args.next_double();
                if (std::isfinite(v)) {
                    out += host_format(s, "", s.conv, v);
                } else {
                    const bool upper = s.conv == 'F' || s.conv == 'E' || s.conv == 'G';
                    std::string body = std::isnan(v) ? "nan" : "inf";
                    if (upper) for (auto& c : body) c = static_cast<char>(c - 'a' + 'A');
                    if (std::signbit(v) && !std::isnan(v)) body.insert(body.begin(), '-');
                    else if (s.plus) body.insert(body.begin(), '+');
                    else if (s.space) body.insert(body.begin(), ' ');
                    pad(out, body, s);
                }
                break;
            }
            case 'n': return fail("%n is never honoured (it would write to guest memory)");
            case 'a': case 'A': return fail("%a/%A (hexadecimal floating point) is not supported");
            case 'C': case 'S': return fail("%C/%S (wide characters) are not supported");
            case 'D': case 'U': case 'O': return fail("the legacy " + spec_text() + " conversions are not supported");
            default: return fail("unsupported conversion " + spec_text());
        }
    }
    if (out.size() > max_output) return fail("output exceeds the limit");
    return {};
}

}  // namespace cider::libsystem
