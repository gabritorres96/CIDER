// NSString / NSConstantString (immutable). Text is kept as UTF-8; lengths and indexes are UTF-16 code units, like Apple's NSString.
// Not implemented: NSMutableString, encodings other than UTF-8, localized/case-insensitive comparison, regular expressions, paths, URLs.
#include <algorithm>
#include <cstring>
#include <format>

#include "orvyn/libsystem.hpp"
#include "orvyn/objc.hpp"
#include "orvyn/printf_format.hpp"
#include "../libsystem/internal.hpp"
#include "foundation_internal.hpp"

namespace orvyn::foundation {
namespace {

using abi::GuestRegs;
using abi::RegsReader;
constexpr int64_t kNotFound = INT64_MAX;     // NSNotFound


bool str_of(Addr obj, std::string_view& out) { return string_view_of(obj, out); }
Class* cls_for(Addr self) {
    Class* c = rt().class_at(self);
    return c && !c->is_meta && rt().is_kind_of(c, classes().string) ? c : classes().string;
}

// Initialises the (empty) payload of an allocated NSString.
Addr init_with(Addr self, std::string utf8) {
    uint64_t* slot = payload_slot(self);
    if (!slot) { raise("NSInvalidArgumentException", "NSString: -init sent to an object that is not an NSString"); return 0; }
    if (*slot) store_drop(*slot);
    auto p = std::make_unique<StringPayload>();
    p->utf8 = std::move(utf8);
    *slot = store_add(std::move(p));
    return self;
}
Addr make_of(Addr cls_self, std::string utf8) {
    const Addr o = rt().create_instance(cls_for(cls_self), 0);
    if (!o) return 0;
    return autoreleased(init_with(o, std::move(utf8)));
}

// ---- class methods --------------------------------------------------------------------------------------------------------------------
Addr c_stringWithUTF8String(Addr self, Addr, Addr cstr) {
    std::string s;
    if (!rt().read_cstring(cstr, s, 64u << 20) || !valid_utf8(s)) return 0;     // invalid UTF-8 or NULL: nil, like Apple
    return make_of(self, std::move(s));
}
Addr c_stringWithString(Addr self, Addr, Addr other) {
    std::string_view sv;
    if (!str_of(other, sv)) { raise("NSInvalidArgumentException", "+[NSString stringWithString:]: argument is not an NSString"); return 0; }
    return make_of(self, std::string(sv));
}
Addr c_string(Addr self, Addr) { return make_of(self, {}); }

// NSString* formatted with %@ support; the reader is positioned after the format argument
bool format_into(const std::string& format, RegsReader& a, std::string& out) {
    libsystem::ObjectFormatter objects = [](uint64_t obj, std::string& text) { text = describe(obj); return true; };
    auto st = libsystem::format_printf(format.c_str(), a, out, 16u << 20, 8u << 20, &objects);
    if (!st.ok) { libsystem::detail::trap("stringWithFormat:: " + st.error); return false; }
    return true;
}
void c_stringWithFormat(GuestRegs* r) {
    RegsReader a(*r);
    const Addr self = a.next_int();
    a.next_int();                                                   // _cmd
    std::string_view f;
    if (!str_of(a.next_int(), f)) { raise("NSInvalidArgumentException", "+[NSString stringWithFormat:]: the format is not an NSString"); return; }
    std::string out;
    if (!format_into(std::string(f), a, out)) return;
    r->ret_rax = make_of(self, std::move(out));
}

// ---- initialisers -------------------------------------------------------------------------------------------------------------------------
Addr i_init(Addr self, Addr) { return init_with(self, {}); }
Addr i_initWithUTF8String(Addr self, Addr, Addr cstr) {
    std::string s;
    if (!rt().read_cstring(cstr, s, 64u << 20) || !valid_utf8(s)) { rt().release(self); return 0; }
    return init_with(self, std::move(s));
}
Addr i_initWithString(Addr self, Addr, Addr other) {
    std::string_view sv;
    if (!str_of(other, sv)) { raise("NSInvalidArgumentException", "-[NSString initWithString:]: argument is not an NSString"); return 0; }
    return init_with(self, std::string(sv));
}
void i_initWithFormat(GuestRegs* r) {
    RegsReader a(*r);
    const Addr self = a.next_int();
    a.next_int();
    std::string_view f;
    if (!str_of(a.next_int(), f)) { raise("NSInvalidArgumentException", "-[NSString initWithFormat:]: the format is not an NSString"); return; }
    std::string out;
    if (!format_into(std::string(f), a, out)) return;
    r->ret_rax = init_with(self, std::move(out));
}
Addr i_dealloc(Addr self, Addr) {
    if (uint64_t* slot = payload_slot(self)) { store_drop(*slot); *slot = 0; }
    rt().destruct_instance(self);
    rt().dispose_instance(self);
    return 0;
}

// ---- queries ----------------------------------------------------------------------------------------------------------------------------------
uint64_t i_length(Addr self, Addr) { std::string_view s; return str_of(self, s) ? utf16_length(s) : 0; }
Addr i_UTF8String(Addr self, Addr) { std::string_view s; return str_of(self, s) ? reinterpret_cast<Addr>(s.data()) : 0; }   // NUL-terminated: payload strings are std::string, constants are C strings
uint64_t i_characterAtIndex(Addr self, Addr, uint64_t idx) {
    std::string_view s;
    if (!str_of(self, s)) return 0;
    const auto u = to_u16(s);
    if (idx >= u.size()) { raise("NSRangeException", std::format("-[NSString characterAtIndex:]: Range or index out of bounds: index {} beyond bounds [0 .. {}]", idx, u.empty() ? 0 : u.size() - 1)); return 0; }
    return u[idx];
}
uint64_t i_isEqualToString(Addr self, Addr, Addr other) {
    std::string_view a, b;
    return str_of(self, a) && str_of(other, b) && a == b ? 1 : 0;
}
uint64_t i_isEqual(Addr self, Addr, Addr other) { return i_isEqualToString(self, 0, other); }
uint64_t i_hash(Addr self, Addr) {
    std::string_view s;
    uint64_t h = 1469598103934665603ull;                            // FNV-1a over the UTF-8 bytes
    if (str_of(self, s)) for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}
int64_t i_compare(Addr self, Addr, Addr other) {
    std::string_view a, b;
    if (!str_of(self, a) || !str_of(other, b)) { raise("NSInvalidArgumentException", "-[NSString compare:]: argument is not an NSString"); return 0; }
    const int c = a.compare(b);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);                            // NSOrderedAscending / NSOrderedSame / NSOrderedDescending
}
uint64_t i_hasPrefix(Addr self, Addr, Addr p) { std::string_view a, b; return str_of(self, a) && str_of(p, b) && a.substr(0, b.size()) == b ? 1 : 0; }
uint64_t i_hasSuffix(Addr self, Addr, Addr p) { std::string_view a, b; return str_of(self, a) && str_of(p, b) && a.size() >= b.size() && a.substr(a.size() - b.size()) == b ? 1 : 0; }
uint64_t i_containsString(Addr self, Addr, Addr p) { std::string_view a, b; return str_of(self, a) && str_of(p, b) && a.find(b) != std::string_view::npos ? 1 : 0; }
void i_rangeOfString(GuestRegs* r) {                                // NSRange: {location, length} returned in rax:rdx
    RegsReader a(*r);
    const Addr self = a.next_int();
    a.next_int();
    std::string_view s, p;
    if (!str_of(self, s) || !str_of(a.next_int(), p)) { raise("NSInvalidArgumentException", "-[NSString rangeOfString:]: argument is not an NSString"); return; }
    const auto at = s.find(p);
    if (at == std::string_view::npos) { r->ret_rax = static_cast<uint64_t>(kNotFound); r->ret_rdx = 0; return; }
    r->ret_rax = utf16_length(s.substr(0, at));
    r->ret_rdx = utf16_length(p);
}
int32_t i_intValue(Addr self, Addr) { std::string_view s; return str_of(self, s) ? static_cast<int32_t>(std::strtol(std::string(s).c_str(), nullptr, 10)) : 0; }
int64_t i_integerValue(Addr self, Addr) { std::string_view s; return str_of(self, s) ? std::strtoll(std::string(s).c_str(), nullptr, 10) : 0; }
void i_doubleValue(GuestRegs* r) {                                  // double results travel in xmm0: raw method
    std::string_view s;
    double d = str_of(r->gp[0], s) ? std::strtod(std::string(s).c_str(), nullptr) : 0.0;
    std::memcpy(&r->ret_xmm0, &d, 8);
}

// ---- derived strings ---------------------------------------------------------------------------------------------------------------------------
Addr i_stringByAppendingString(Addr self, Addr, Addr other) {
    std::string_view a, b;
    if (!str_of(self, a) || !str_of(other, b)) { raise("NSInvalidArgumentException", "-[NSString stringByAppendingString:]: argument is not an NSString"); return 0; }
    return new_string_autoreleased(std::string(a) + std::string(b));
}
Addr substring(Addr self, uint64_t from, uint64_t len, const char* what) {
    std::string_view s;
    if (!str_of(self, s)) return 0;
    const auto u = to_u16(s);
    if (from > u.size() || len > u.size() - from) {
        raise("NSRangeException", std::format("-[NSString {}]: Range {{{}, {}}} out of bounds; string length {}", what, from, len, u.size()));
        return 0;
    }
    return new_string_autoreleased(from_u16(u.substr(from, len)));
}
Addr i_substringToIndex(Addr self, Addr, uint64_t n) { return substring(self, 0, n, "substringToIndex:"); }
Addr i_substringFromIndex(Addr self, Addr, uint64_t n) {
    std::string_view s;
    if (!str_of(self, s)) return 0;
    const uint64_t total = utf16_length(s);
    if (n > total) { raise("NSRangeException", std::format("-[NSString substringFromIndex:]: Index {} out of bounds; string length {}", n, total)); return 0; }
    return substring(self, n, total - n, "substringFromIndex:");
}
Addr i_substringWithRange(Addr self, Addr, uint64_t loc, uint64_t len) { return substring(self, loc, len, "substringWithRange:"); }
Addr i_lowercaseString(Addr self, Addr) { std::string_view s; if (!str_of(self, s)) return 0; std::string o(s); for (auto& c : o) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32); return new_string_autoreleased(o); }
Addr i_uppercaseString(Addr self, Addr) { std::string_view s; if (!str_of(self, s)) return 0; std::string o(s); for (auto& c : o) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32); return new_string_autoreleased(o); }
Addr i_componentsSeparatedByString(Addr self, Addr, Addr sep) {
    std::string_view s, d;
    if (!str_of(self, s) || !str_of(sep, d) || d.empty()) { raise("NSInvalidArgumentException", "-[NSString componentsSeparatedByString:]: invalid separator"); return 0; }
    std::vector<Addr> parts;
    size_t from = 0;
    for (;;) {
        const size_t at = s.find(d, from);
        parts.push_back(new_string(std::string(s.substr(from, at == std::string_view::npos ? std::string_view::npos : at - from))));
        if (at == std::string_view::npos) break;
        from = at + d.size();
    }
    return new_array(std::move(parts), /*mutable=*/false, /*retain_items=*/false);     // the new strings already carry +1: the array takes them over
}
Addr i_description(Addr self, Addr) { return self; }
Addr i_copyWithZone(Addr self, Addr, Addr) {
    if (!is_kind(self, classes().mstring)) return rt().retain(self);                               // immutable: copy == retain
    std::string_view sv;
    return str_of(self, sv) ? new_string(std::string(sv)) : 0;                                      // NSMutableString: an immutable snapshot (+1)
}
Addr i_mutableCopyWithZone(Addr self, Addr, Addr) {
    std::string_view sv;
    if (!str_of(self, sv)) return 0;
    const Addr o = rt().create_instance(classes().mstring, 0);
    return o ? init_with(o, std::string(sv)) : 0;
}
// ---- path helpers (POSIX paths in UTF-8) ------------------------------------------------------------------------------------------------
std::string last_component(std::string_view s) {
    while (s.size() > 1 && s.back() == '/') s.remove_suffix(1);
    const auto at = s.rfind('/');
    return std::string(at == std::string_view::npos || s.size() == 1 ? s : s.substr(at + 1));
}
Addr i_lastPathComponent(Addr self, Addr) { std::string_view s; return str_of(self, s) ? new_string_autoreleased(last_component(s)) : 0; }
Addr i_pathExtension(Addr self, Addr) {
    std::string_view s;
    if (!str_of(self, s)) return 0;
    const std::string last = last_component(s);
    const auto dot = last.rfind('.');
    return new_string_autoreleased(dot == std::string::npos || dot == 0 ? std::string() : last.substr(dot + 1));
}
Addr i_stringByDeletingLastPathComponent(Addr self, Addr) {
    std::string_view s;
    if (!str_of(self, s)) return 0;
    std::string t(s);
    while (t.size() > 1 && t.back() == '/') t.pop_back();
    const auto at = t.rfind('/');
    if (at == std::string::npos) return new_string_autoreleased("");
    return new_string_autoreleased(at == 0 ? "/" : t.substr(0, at));
}
Addr i_stringByAppendingPathComponent(Addr self, Addr, Addr other) {
    std::string_view a, b;
    if (!str_of(self, a) || !str_of(other, b)) { raise("NSInvalidArgumentException", "-[NSString stringByAppendingPathComponent:]: argument is not an NSString"); return 0; }
    std::string t(a);
    if (!t.empty() && t.back() != '/' && !b.empty() && b.front() != '/') t += '/';
    else if (!t.empty() && t.back() == '/' && !b.empty() && b.front() == '/') b.remove_prefix(1);
    return new_string_autoreleased(t + std::string(b));
}
uint64_t i_isAbsolutePath(Addr self, Addr) { std::string_view s; return str_of(self, s) && !s.empty() && s.front() == '/' ? 1 : 0; }

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

void install_nsstring(Runtime& r) {
    Class* s = r.declare_native_class("NSString", "NSObject", 16);
    classes().string = s;
    Class* cs = r.declare_native_class("NSConstantString", "NSString", 32);          // layout of the compiler's __NSConstantString: isa, flags, str, length
    classes().constant_string = cs;
    r.add_native_methods(s, true, {
        {"stringWithUTF8String:", "@24@0:8r*16", addr(c_stringWithUTF8String), false},
        {"stringWithString:", "@24@0:8@16", addr(c_stringWithString), false},
        {"string", "@16@0:8", addr(c_string), false},
        {"stringWithFormat:", "@24@0:8@16", addr(c_stringWithFormat), true},
    });
    r.add_native_methods(s, false, {
        {"init", "@16@0:8", addr(i_init), false},
        {"initWithUTF8String:", "@24@0:8r*16", addr(i_initWithUTF8String), false},
        {"initWithString:", "@24@0:8@16", addr(i_initWithString), false},
        {"initWithFormat:", "@24@0:8@16", addr(i_initWithFormat), true},
        {"dealloc", "v16@0:8", addr(i_dealloc), false},
        {"length", "Q16@0:8", addr(i_length), false},
        {"UTF8String", "r*16@0:8", addr(i_UTF8String), false},
        {"characterAtIndex:", "S24@0:8Q16", addr(i_characterAtIndex), false},
        {"isEqualToString:", "B24@0:8@16", addr(i_isEqualToString), false},
        {"isEqual:", "B24@0:8@16", addr(i_isEqual), false},
        {"hash", "Q16@0:8", addr(i_hash), false},
        {"compare:", "q24@0:8@16", addr(i_compare), false},
        {"hasPrefix:", "B24@0:8@16", addr(i_hasPrefix), false},
        {"hasSuffix:", "B24@0:8@16", addr(i_hasSuffix), false},
        {"containsString:", "B24@0:8@16", addr(i_containsString), false},
        {"rangeOfString:", "{_NSRange=QQ}24@0:8@16", addr(i_rangeOfString), true},
        {"intValue", "i16@0:8", addr(i_intValue), false},
        {"integerValue", "q16@0:8", addr(i_integerValue), false},
        {"doubleValue", "d16@0:8", addr(i_doubleValue), true},
        {"stringByAppendingString:", "@24@0:8@16", addr(i_stringByAppendingString), false},
        {"substringToIndex:", "@24@0:8Q16", addr(i_substringToIndex), false},
        {"substringFromIndex:", "@24@0:8Q16", addr(i_substringFromIndex), false},
        {"substringWithRange:", "@32@0:8{_NSRange=QQ}16", addr(i_substringWithRange), false},
        {"lowercaseString", "@16@0:8", addr(i_lowercaseString), false},
        {"uppercaseString", "@16@0:8", addr(i_uppercaseString), false},
        {"componentsSeparatedByString:", "@24@0:8@16", addr(i_componentsSeparatedByString), false},
        {"description", "@16@0:8", addr(i_description), false},
        {"copyWithZone:", "@24@0:8^{_NSZone=}16", addr(i_copyWithZone), false},
        {"mutableCopyWithZone:", "@24@0:8^{_NSZone=}16", addr(i_mutableCopyWithZone), false},
        {"lastPathComponent", "@16@0:8", addr(i_lastPathComponent), false},
        {"pathExtension", "@16@0:8", addr(i_pathExtension), false},
        {"stringByDeletingLastPathComponent", "@16@0:8", addr(i_stringByDeletingLastPathComponent), false},
        {"stringByAppendingPathComponent:", "@24@0:8@16", addr(i_stringByAppendingPathComponent), false},
        {"isAbsolutePath", "B16@0:8", addr(i_isAbsolutePath), false},
    });
}

}  // namespace orvyn::foundation
