#pragma once
// CIDER's own printf-family formatter (CIDER 0.3). Darwin guest code must NOT hand its `va_list`, or its System V
// variadic registers, to the Windows CRT: the layouts differ (a Darwin va_list is a struct with a register save
// area, the Windows one a plain pointer; doubles travel in xmm registers announced by AL). Instead the formatter pulls
// arguments one at a time from an abstract ArgReader (registers + stack of a direct call, or a Darwin va_list living
// in guest memory) and renders each conversion itself.
//
// SUPPORTED, exactly:
//   flags        '-' '+' ' ' '#' '0'
//   width        decimal digits or '*' (a negative '*' width means '-' flag)
//   precision    '.' decimal digits, '.' alone (= 0), or '.*' (negative = none)
//   length       (none) hh h l ll j z t q
//   conversions  d i   signed integer          u o x X   unsigned integer
//                c     character               s         NUL-terminated byte string, "(null)" for NULL
//                p     pointer, rendered "0x" + lowercase hex (Darwin style)
//                f F e E g G   double (inf/nan spelled like Darwin: "inf", "-inf", "nan")
//                %%    a literal percent sign
// NOT SUPPORTED (formatting stops with an error, nothing is guessed): %n (never honoured: it writes to memory),
// %a %A, %C %S %ls %lc (wide characters), the 'L' length (long double), %D %U %O, %m, the "'" flag,
// positional arguments (%1$d) and the ' ' / '#' flags on conversions where they have no meaning are ignored as in C.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "cider/abi.hpp"

namespace cider::libsystem {

struct FormatStatus {
    bool ok = true;
    std::string error;   // set when !ok: names the offending specifier
};

// Appends the formatted text to `out`. Fails (leaving whatever was produced so far) on an unsupported
// specifier, a malformed one, or when the output would exceed `max_output` bytes.
// `%s` reads the guest string up to its NUL or the precision, at most `max_string` bytes.
// Optional %@ support (Foundation's NSLog / stringWithFormat:): renders an Objective-C object (its -description) into `out`.
using ObjectFormatter = std::function<bool(uint64_t object, std::string& out)>;

FormatStatus format_printf(const char* fmt, abi::ArgReader& args, std::string& out, size_t max_output = 16u << 20,
                           size_t max_string = 8u << 20, const ObjectFormatter* objects = nullptr);

}  // namespace cider::libsystem
