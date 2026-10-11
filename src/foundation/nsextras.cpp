// Foundation, second batch (ORVYN 0.5.3): NSMutableString, NSData/NSMutableData, NSDate, NSURL, NSError, NSFileManager.
// Only what the fixtures and the AppKit layer use. All file access goes through the guest root (--root): a guest path is resolved lexically,
// re-checked after link resolution and refused outside the root; without --root every file operation fails with EACCES, like fopen.
// Not implemented: NSCalendar/NSDateFormatter/time zones (descriptions are UTC), NSURLComponents/relative URLs/percent-encoding of input,
// NSFileHandle, file attributes, atomic writes, NSError recovery, NSData compression/base64/contentsOfURL:.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string_view>
#include <unordered_map>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>

#include "orvyn/libsystem.hpp"
#include "orvyn/objc.hpp"
#include "orvyn/util.hpp"
#include "../libsystem/internal.hpp"
#include "foundation_internal.hpp"

namespace fs = std::filesystem;

namespace orvyn::foundation {
namespace {

using abi::GuestRegs;
using abi::RegsReader;
constexpr uint64_t kNotFound = INT64_MAX;
constexpr double kUnixToReference = 978307200.0;            // 2001-01-01 00:00:00 UTC in Unix seconds
constexpr size_t kMaxFile = 256u << 20;

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }
double bits_to_double(uint64_t b) { double d; std::memcpy(&d, &b, 8); return d; }
uint64_t double_to_bits(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }

// ======================================================================= NSMutableString
std::string_view sv_of(Addr o, bool& ok) {
    std::string_view s;
    ok = string_view_of(o, s);
    return s;
}
StringPayload* mutable_payload(Addr self, const char* who) {
    if (!is_kind(self, classes().mstring)) { raise("NSInvalidArgumentException", std::string(who) + ": receiver is not a mutable string"); return nullptr; }
    uint64_t* slot = payload_slot(self);
    if (!slot) return nullptr;
    if (!*slot) *slot = store_add(std::make_unique<StringPayload>());
    return dynamic_cast<StringPayload*>(store_get(*slot));
}
Addr ms_stringWithCapacity(Addr self, Addr, uint64_t) {
    Class* c = rt().class_at(self);
    if (!c || c->is_meta || !rt().is_kind_of(c, classes().mstring)) c = classes().mstring;
    const Addr o = rt().create_instance(c, 0);
    if (!o) return 0;
    if (!mutable_payload(o, "+[NSMutableString stringWithCapacity:]")) { rt().release(o); return 0; }
    return autoreleased(o);
}
Addr ms_initWithCapacity(Addr self, Addr, uint64_t) { return mutable_payload(self, "-[NSMutableString initWithCapacity:]") ? self : 0; }
Addr ms_appendString(Addr self, Addr, Addr other) {
    StringPayload* p = mutable_payload(self, "-[NSMutableString appendString:]");
    bool ok;
    std::string add(sv_of(other, ok));
    if (!p) return 0;
    if (!ok) { raise("NSInvalidArgumentException", "-[NSMutableString appendString:]: argument is not an NSString"); return 0; }
    if (p->utf8.size() + add.size() > (256u << 20)) { raise("NSRangeException", "-[NSMutableString appendString:]: the string would exceed 256 MiB"); return 0; }
    p->utf8 += add;
    return 0;
}
void ms_appendFormat(GuestRegs* r) {
    RegsReader a(*r);
    const Addr self = a.next_int();
    a.next_int();
    bool ok;
    std::string format(sv_of(a.next_int(), ok));
    StringPayload* p = mutable_payload(self, "-[NSMutableString appendFormat:]");
    if (!p) return;
    if (!ok) { raise("NSInvalidArgumentException", "-[NSMutableString appendFormat:]: the format is not an NSString"); return; }
    std::string out;
    if (!format_with_objects(format, a, out)) return;
    p = mutable_payload(self, "-[NSMutableString appendFormat:]");
    if (p) p->utf8 += out;
}
Addr ms_setString(Addr self, Addr, Addr other) {
    StringPayload* p = mutable_payload(self, "-[NSMutableString setString:]");
    bool ok;
    std::string v(sv_of(other, ok));
    if (!p) return 0;
    if (!ok) { raise("NSInvalidArgumentException", "-[NSMutableString setString:]: argument is not an NSString"); return 0; }
    p->utf8 = v;
    return 0;
}
Addr ms_replace(Addr self, uint64_t loc, uint64_t len, const std::string& with, const char* who) {
    StringPayload* p = mutable_payload(self, who);
    if (!p) return 0;
    std::u16string u = to_u16(p->utf8);
    if (loc > u.size() || len > u.size() - loc) { raise("NSRangeException", std::format("{}: Range {{{}, {}}} out of bounds; string length {}", who, loc, len, u.size())); return 0; }
    u.replace(loc, len, to_u16(with));
    p->utf8 = from_u16(u);
    return 0;
}
Addr ms_insertString(Addr self, Addr, Addr other, uint64_t idx) {
    bool ok;
    std::string v(sv_of(other, ok));
    if (!ok) { raise("NSInvalidArgumentException", "-[NSMutableString insertString:atIndex:]: argument is not an NSString"); return 0; }
    return ms_replace(self, idx, 0, v, "-[NSMutableString insertString:atIndex:]");
}
Addr ms_deleteCharacters(Addr self, Addr, uint64_t loc, uint64_t len) { return ms_replace(self, loc, len, "", "-[NSMutableString deleteCharactersInRange:]"); }
Addr ms_replaceCharacters(Addr self, Addr, uint64_t loc, uint64_t len) { (void)self; (void)loc; (void)len; return 0; }   // replaced by the raw version below
void ms_replaceCharactersRaw(GuestRegs* r) {                       // replaceCharactersInRange:(NSRange) withString:(NSString*): self,_cmd,loc,len,str
    bool ok;
    std::string v(sv_of(r->gp[4], ok));
    if (!ok) { raise("NSInvalidArgumentException", "-[NSMutableString replaceCharactersInRange:withString:]: argument is not an NSString"); return; }
    ms_replace(r->gp[0], r->gp[2], r->gp[3], v, "-[NSMutableString replaceCharactersInRange:withString:]");
}

// ======================================================================= NSData / NSMutableData
DataPayload* data_of(Addr obj) {
    uint64_t* slot = payload_slot(obj);
    if (!slot || !is_kind(obj, classes().data)) return nullptr;
    if (!*slot) *slot = store_add(std::make_unique<DataPayload>());
    return dynamic_cast<DataPayload*>(store_get(*slot));
}
bool mutable_data(Addr obj) { return is_kind(obj, classes().mdata); }
Addr make_data(Class* cls, std::vector<uint8_t> bytes, bool autorelease_it) {
    const Addr o = rt().create_instance(cls, 0);
    if (!o) return 0;
    DataPayload* p = data_of(o);
    if (!p) { rt().release(o); return 0; }
    p->bytes = std::move(bytes);
    return autorelease_it ? autoreleased(o) : o;
}
Class* data_class(Addr self, bool want_mutable) {
    Class* c = rt().class_at(self);
    if (!c || c->is_meta || !rt().is_kind_of(c, classes().data)) c = want_mutable ? classes().mdata : classes().data;
    return c;
}
bool read_guest_bytes(Addr ptr, uint64_t n, std::vector<uint8_t>& out, const char* who) {
    if (n > kMaxFile) { raise("NSInvalidArgumentException", std::string(who) + ": length too large"); return false; }
    if (n && (!ptr || !rt().readable(ptr, n))) { raise("NSInvalidArgumentException", std::string(who) + ": the bytes pointer is NULL or unreadable"); return false; }
    out.assign(reinterpret_cast<const uint8_t*>(ptr), reinterpret_cast<const uint8_t*>(ptr) + n);
    return true;
}
Addr d_data(Addr self, Addr) { return make_data(data_class(self, false), {}, true); }
Addr d_dataWithBytesLength(Addr self, Addr, Addr bytes, uint64_t n) {
    std::vector<uint8_t> v;
    return read_guest_bytes(bytes, n, v, "+[NSData dataWithBytes:length:]") ? make_data(data_class(self, false), std::move(v), true) : 0;
}
Addr d_dataWithData(Addr self, Addr, Addr other) {
    DataPayload* o = data_of(other);
    if (!o) { raise("NSInvalidArgumentException", "+[NSData dataWithData:]: argument is not NSData"); return 0; }
    return make_data(data_class(self, false), o->bytes, true);
}
Addr d_initWithBytesLength(Addr self, Addr, Addr bytes, uint64_t n) {
    DataPayload* p = data_of(self);
    std::vector<uint8_t> v;
    if (!p || !read_guest_bytes(bytes, n, v, "-[NSData initWithBytes:length:]")) return 0;
    p->bytes = std::move(v);
    return self;
}
Addr d_init(Addr self, Addr) { return data_of(self) ? self : 0; }
Addr d_dealloc(Addr self, Addr) {
    if (uint64_t* slot = payload_slot(self)) { store_drop(*slot); *slot = 0; }
    rt().destruct_instance(self);
    rt().dispose_instance(self);
    return 0;
}
uint64_t d_length(Addr self, Addr) { DataPayload* p = data_of(self); return p ? p->bytes.size() : 0; }
Addr d_bytes(Addr self, Addr) {
    static const uint8_t empty[1] = {0};
    DataPayload* p = data_of(self);
    return p ? (p->bytes.empty() ? reinterpret_cast<Addr>(empty) : reinterpret_cast<Addr>(p->bytes.data())) : 0;
}
Addr d_getBytesLength(Addr self, Addr, Addr buf, uint64_t n) {
    DataPayload* p = data_of(self);
    if (!p) return 0;
    n = std::min<uint64_t>(n, p->bytes.size());
    if (n && (!buf || !rt().readable(buf, n))) { raise("NSInvalidArgumentException", "-[NSData getBytes:length:]: the buffer is NULL or unreadable"); return 0; }
    if (n) std::memcpy(reinterpret_cast<void*>(buf), p->bytes.data(), n);
    return 0;
}
uint64_t d_isEqualToData(Addr self, Addr, Addr other) {
    DataPayload *a = data_of(self), *b = data_of(other);
    return a && b && a->bytes == b->bytes ? 1 : 0;
}
uint64_t d_isEqual(Addr self, Addr, Addr other) { return self == other || (is_kind(other, classes().data) && d_isEqualToData(self, 0, other)) ? 1 : 0; }
uint64_t d_hash(Addr self, Addr) {
    DataPayload* p = data_of(self);
    uint64_t h = 1469598103934665603ull;
    if (p) for (uint8_t c : p->bytes) { h ^= c; h *= 1099511628211ull; }
    return h;
}
Addr d_subdataWithRange(Addr self, Addr, uint64_t loc, uint64_t len) {
    DataPayload* p = data_of(self);
    if (!p) return 0;
    if (loc > p->bytes.size() || len > p->bytes.size() - loc) { raise("NSRangeException", std::format("-[NSData subdataWithRange:]: range {{{}, {}}} exceeds data length {}", loc, len, p->bytes.size())); return 0; }
    return make_data(classes().data, std::vector<uint8_t>(p->bytes.begin() + static_cast<ptrdiff_t>(loc), p->bytes.begin() + static_cast<ptrdiff_t>(loc + len)), true);
}
Addr d_description(Addr self, Addr) {
    DataPayload* p = data_of(self);
    if (!p) return 0;
    std::string s = "<";
    for (size_t i = 0; i < p->bytes.size() && i < 4096; ++i) {
        if (i && i % 4 == 0) s += ' ';
        s += std::format("{:02x}", p->bytes[i]);
    }
    if (p->bytes.size() > 4096) s += " ...";
    return new_string_autoreleased(s + ">");
}
Addr d_copyWithZone(Addr self, Addr, Addr) {
    if (!mutable_data(self)) return rt().retain(self);
    DataPayload* p = data_of(self);
    return p ? make_data(classes().data, p->bytes, false) : 0;
}
Addr d_mutableCopyWithZone(Addr self, Addr, Addr) {
    DataPayload* p = data_of(self);
    return p ? make_data(classes().mdata, p->bytes, false) : 0;
}
Addr md_dataWithCapacity(Addr self, Addr, uint64_t) { return make_data(data_class(self, true), {}, true); }
Addr md_dataWithLength(Addr self, Addr, uint64_t n) {
    if (n > kMaxFile) { raise("NSInvalidArgumentException", "+[NSMutableData dataWithLength:]: length too large"); return 0; }
    return make_data(data_class(self, true), std::vector<uint8_t>(n, 0), true);
}
Addr md_initWithCapacity(Addr self, Addr, uint64_t) { return data_of(self) ? self : 0; }
Addr md_mutableBytes(Addr self, Addr) { return d_bytes(self, 0); }
Addr md_appendBytesLength(Addr self, Addr, Addr bytes, uint64_t n) {
    DataPayload* p = data_of(self);
    std::vector<uint8_t> v;
    if (!p || !read_guest_bytes(bytes, n, v, "-[NSMutableData appendBytes:length:]")) return 0;
    if (p->bytes.size() + v.size() > kMaxFile) { raise("NSRangeException", "-[NSMutableData appendBytes:length:]: data would exceed 256 MiB"); return 0; }
    p->bytes.insert(p->bytes.end(), v.begin(), v.end());
    return 0;
}
Addr md_appendData(Addr self, Addr, Addr other) {
    DataPayload *p = data_of(self), *o = data_of(other);
    if (!p || !o) { raise("NSInvalidArgumentException", "-[NSMutableData appendData:]: argument is not NSData"); return 0; }
    const std::vector<uint8_t> copy = o->bytes;
    p->bytes.insert(p->bytes.end(), copy.begin(), copy.end());
    return 0;
}
Addr md_setLength(Addr self, Addr, uint64_t n) {
    DataPayload* p = data_of(self);
    if (!p) return 0;
    if (n > kMaxFile) { raise("NSRangeException", "-[NSMutableData setLength:]: length too large"); return 0; }
    p->bytes.resize(n, 0);
    return 0;
}
Addr md_increaseLengthBy(Addr self, Addr, uint64_t n) { DataPayload* p = data_of(self); return p ? md_setLength(self, 0, p->bytes.size() + n) : 0; }
void md_replaceBytes(GuestRegs* r) {                                // replaceBytesInRange:(NSRange) withBytes:(const void*): self,_cmd,loc,len,bytes
    DataPayload* p = data_of(r->gp[0]);
    const uint64_t loc = r->gp[2], len = r->gp[3];
    std::vector<uint8_t> v;
    if (!p || !read_guest_bytes(r->gp[4], len, v, "-[NSMutableData replaceBytesInRange:withBytes:]")) return;
    if (loc > p->bytes.size() || len > p->bytes.size() - loc) { raise("NSRangeException", "-[NSMutableData replaceBytesInRange:withBytes:]: range exceeds data length"); return; }
    std::copy(v.begin(), v.end(), p->bytes.begin() + static_cast<ptrdiff_t>(loc));
}

// ======================================================================= NSDate
DatePayload* date_of(Addr obj) {
    uint64_t* slot = payload_slot(obj);
    if (!slot || !is_kind(obj, classes().date)) return nullptr;
    if (!*slot) *slot = store_add(std::make_unique<DatePayload>());
    return dynamic_cast<DatePayload*>(store_get(*slot));
}
double now_reference() {
    const auto t = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration<double>(t).count() - kUnixToReference;
}
Addr make_date(Addr cls_self, double ref, bool autorelease_it) {
    Class* c = rt().class_at(cls_self);
    if (!c || c->is_meta || !rt().is_kind_of(c, classes().date)) c = classes().date;
    const Addr o = rt().create_instance(c, 0);
    if (!o) return 0;
    DatePayload* p = date_of(o);
    if (!p) { rt().release(o); return 0; }
    p->since_reference = ref;
    return autorelease_it ? autoreleased(o) : o;
}
void set_double_result(GuestRegs* r, double d) { r->ret_xmm0 = double_to_bits(d); }
void dt_date(GuestRegs* r) { r->ret_rax = make_date(r->gp[0], now_reference(), true); }
void dt_dateWithTimeIntervalSince1970(GuestRegs* r) { r->ret_rax = make_date(r->gp[0], bits_to_double(r->xmm[0]) - kUnixToReference, true); }
void dt_dateWithTimeIntervalSinceReferenceDate(GuestRegs* r) { r->ret_rax = make_date(r->gp[0], bits_to_double(r->xmm[0]), true); }
void dt_dateWithTimeIntervalSinceNow(GuestRegs* r) { r->ret_rax = make_date(r->gp[0], now_reference() + bits_to_double(r->xmm[0]), true); }
void dt_initWithTimeIntervalSince1970(GuestRegs* r) {
    DatePayload* p = date_of(r->gp[0]);
    if (!p) return;
    p->since_reference = bits_to_double(r->xmm[0]) - kUnixToReference;
    r->ret_rax = r->gp[0];
}
Addr dt_init(Addr self, Addr) {
    DatePayload* p = date_of(self);
    if (!p) return 0;
    p->since_reference = now_reference();
    return self;
}
Addr dt_dealloc(Addr self, Addr) {
    if (uint64_t* slot = payload_slot(self)) { store_drop(*slot); *slot = 0; }
    rt().destruct_instance(self);
    rt().dispose_instance(self);
    return 0;
}
void dt_timeIntervalSince1970(GuestRegs* r) { DatePayload* p = date_of(r->gp[0]); set_double_result(r, p ? p->since_reference + kUnixToReference : 0); }
void dt_timeIntervalSinceReferenceDate(GuestRegs* r) { DatePayload* p = date_of(r->gp[0]); set_double_result(r, p ? p->since_reference : 0); }
void dt_timeIntervalSinceNow(GuestRegs* r) { DatePayload* p = date_of(r->gp[0]); set_double_result(r, p ? p->since_reference - now_reference() : 0); }
void dt_timeIntervalSinceDate(GuestRegs* r) {
    DatePayload *a = date_of(r->gp[0]), *b = date_of(r->gp[2]);
    if (!a || !b) { raise("NSInvalidArgumentException", "-[NSDate timeIntervalSinceDate:]: argument is not an NSDate"); return; }
    set_double_result(r, a->since_reference - b->since_reference);
}
void dt_dateByAddingTimeInterval(GuestRegs* r) {
    DatePayload* p = date_of(r->gp[0]);
    if (p) r->ret_rax = make_date(rt().class_of_object(r->gp[0])->addr, p->since_reference + bits_to_double(r->xmm[0]), true);
}
int64_t dt_compare(Addr self, Addr, Addr other) {
    DatePayload *a = date_of(self), *b = date_of(other);
    if (!a || !b) { raise("NSInvalidArgumentException", "-[NSDate compare:]: argument is not an NSDate"); return 0; }
    return a->since_reference < b->since_reference ? -1 : (a->since_reference > b->since_reference ? 1 : 0);
}
uint64_t dt_isEqualToDate(Addr self, Addr, Addr other) {
    DatePayload *a = date_of(self), *b = date_of(other);
    return a && b && a->since_reference == b->since_reference ? 1 : 0;
}
uint64_t dt_isEqual(Addr self, Addr, Addr other) { return self == other || (is_kind(other, classes().date) && dt_isEqualToDate(self, 0, other)) ? 1 : 0; }
uint64_t dt_hash(Addr self, Addr) { DatePayload* p = date_of(self); return p ? static_cast<uint64_t>(static_cast<int64_t>(p->since_reference)) : 0; }
Addr dt_earlierDate(Addr self, Addr, Addr other) { return dt_compare(self, 0, other) <= 0 ? self : other; }
Addr dt_laterDate(Addr self, Addr, Addr other) { return dt_compare(self, 0, other) >= 0 ? self : other; }
Addr dt_description(Addr self, Addr) {
    DatePayload* p = date_of(self);
    if (!p) return 0;
    const double unix_s = p->since_reference + kUnixToReference;
    const std::time_t t = static_cast<std::time_t>(std::floor(unix_s));
    std::tm tmv{};
    gmtime_s(&tmv, &t);
    return new_string_autoreleased(std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02} +0000", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec));
}
Addr dt_copyWithZone(Addr self, Addr, Addr) { return rt().retain(self); }

// ======================================================================= NSURL
URLPayload* url_of(Addr obj) {
    uint64_t* slot = payload_slot(obj);
    if (!slot || !is_kind(obj, classes().url)) return nullptr;
    if (!*slot) *slot = store_add(std::make_unique<URLPayload>());
    return dynamic_cast<URLPayload*>(store_get(*slot));
}
std::string percent_decode(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16)));
            i += 2;
        } else out.push_back(s[i]);
    }
    return out;
}
std::string percent_encode_path(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || std::strchr("-._~/$&'()*+,;=:@!", c)) out.push_back(static_cast<char>(c));
        else out += std::format("%{:02X}", c);
    }
    return out;
}
bool parse_url(std::string_view s, URLPayload& u) {
    if (s.empty()) return false;
    for (unsigned char c : s) if (c <= 0x20 || c == 0x7f || c == '"' || c == '<' || c == '>' || c == '\\' || c == '^' || c == '`' || c == '{' || c == '|' || c == '}') return false;
    u.absolute = std::string(s);
    std::string_view rest = s;
    const auto colon = rest.find(':');
    const auto slash = rest.find('/');
    if (colon != std::string_view::npos && (slash == std::string_view::npos || colon < slash) && colon > 0) {
        u.scheme = std::string(rest.substr(0, colon));
        for (auto& ch : u.scheme) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        rest.remove_prefix(colon + 1);
    }
    if (const auto h = rest.find('#'); h != std::string_view::npos) rest = rest.substr(0, h);
    if (const auto q = rest.find('?'); q != std::string_view::npos) { u.query = std::string(rest.substr(q + 1)); rest = rest.substr(0, q); }
    if (rest.rfind("//", 0) == 0) {
        rest.remove_prefix(2);
        const auto end = rest.find('/');
        u.host = std::string(rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end);
    }
    u.path = percent_decode(rest);
    return true;
}
Addr make_url(Addr cls_self, const URLPayload& u) {
    Class* c = rt().class_at(cls_self);
    if (!c || c->is_meta || !rt().is_kind_of(c, classes().url)) c = classes().url;
    const Addr o = rt().create_instance(c, 0);
    if (!o) return 0;
    URLPayload* p = url_of(o);
    if (!p) { rt().release(o); return 0; }
    *p = u;
    return autoreleased(o);
}
Addr u_URLWithString(Addr self, Addr, Addr str) {
    bool ok;
    std::string s(sv_of(str, ok));
    URLPayload u;
    if (!ok || !parse_url(s, u)) return 0;                          // nil for malformed strings, like Apple
    return make_url(self, u);
}
Addr u_fileURLWithPath(Addr self, Addr, Addr str) {
    bool ok;
    std::string s(sv_of(str, ok));
    if (!ok) { raise("NSInvalidArgumentException", "+[NSURL fileURLWithPath:]: argument is not an NSString"); return 0; }
    URLPayload u;
    u.scheme = "file";
    u.path = s;
    u.absolute = "file://" + percent_encode_path(s);
    return make_url(self, u);
}
Addr u_dealloc(Addr self, Addr) {
    if (uint64_t* slot = payload_slot(self)) { store_drop(*slot); *slot = 0; }
    rt().destruct_instance(self);
    rt().dispose_instance(self);
    return 0;
}
Addr u_absoluteString(Addr self, Addr) { URLPayload* p = url_of(self); return p ? new_string_autoreleased(p->absolute) : 0; }
Addr u_path(Addr self, Addr) { URLPayload* p = url_of(self); return p ? new_string_autoreleased(p->path) : 0; }
Addr u_scheme(Addr self, Addr) { URLPayload* p = url_of(self); return p && !p->scheme.empty() ? new_string_autoreleased(p->scheme) : 0; }
Addr u_host(Addr self, Addr) { URLPayload* p = url_of(self); return p && !p->host.empty() ? new_string_autoreleased(p->host) : 0; }
Addr u_query(Addr self, Addr) { URLPayload* p = url_of(self); return p && !p->query.empty() ? new_string_autoreleased(p->query) : 0; }
uint64_t u_isFileURL(Addr self, Addr) { URLPayload* p = url_of(self); return p && p->scheme == "file" ? 1 : 0; }
Addr u_lastPathComponent(Addr self, Addr) {
    URLPayload* p = url_of(self);
    if (!p) return 0;
    std::string t = p->path;
    while (t.size() > 1 && t.back() == '/') t.pop_back();
    const auto at = t.rfind('/');
    return new_string_autoreleased(at == std::string::npos || t.size() == 1 ? t : t.substr(at + 1));
}
Addr u_pathExtension(Addr self, Addr) {
    URLPayload* p = url_of(self);
    if (!p) return 0;
    const auto slash = p->path.rfind('/');
    const std::string last = slash == std::string::npos ? p->path : p->path.substr(slash + 1);
    const auto dot = last.rfind('.');
    return new_string_autoreleased(dot == std::string::npos || dot == 0 ? std::string() : last.substr(dot + 1));
}
Addr u_URLByAppendingPathComponent(Addr self, Addr, Addr comp) {
    URLPayload* p = url_of(self);
    bool ok;
    std::string c(sv_of(comp, ok));
    if (!p || !ok) return 0;
    URLPayload u = *p;
    if (u.path.empty() || u.path.back() != '/') u.path += '/';
    u.path += c;
    const auto base_end = p->absolute.find_first_of("?#");
    std::string base = p->absolute.substr(0, base_end);
    if (base.empty() || base.back() != '/') base += '/';
    u.absolute = base + percent_encode_path(c) + (base_end == std::string::npos ? "" : p->absolute.substr(base_end));
    return make_url(rt().class_of_object(self)->addr, u);
}
uint64_t u_isEqual(Addr self, Addr, Addr other) {
    URLPayload *a = url_of(self), *b = url_of(other);
    return self == other || (a && b && a->absolute == b->absolute) ? 1 : 0;
}
uint64_t u_hash(Addr self, Addr) {
    URLPayload* p = url_of(self);
    uint64_t h = 1469598103934665603ull;
    if (p) for (unsigned char c : p->absolute) { h ^= c; h *= 1099511628211ull; }
    return h;
}
Addr u_description(Addr self, Addr) { return u_absoluteString(self, 0); }
Addr u_copyWithZone(Addr self, Addr, Addr) { return rt().retain(self); }

// ======================================================================= NSError
ErrorPayload* error_of(Addr obj) {
    uint64_t* slot = payload_slot(obj);
    if (!slot || !is_kind(obj, classes().error)) return nullptr;
    if (!*slot) *slot = store_add(std::make_unique<ErrorPayload>());
    return dynamic_cast<ErrorPayload*>(store_get(*slot));
}
Addr key_localized_description = 0;
Addr init_error(Addr o, Addr domain, int64_t code, Addr userinfo) {
    ErrorPayload* p = error_of(o);
    if (!p) return 0;
    if (!domain) { raise("NSInvalidArgumentException", "NSError: the domain must not be nil"); return 0; }
    if (p->domain) rt().release(p->domain);
    if (p->userinfo) rt().release(p->userinfo);
    p->domain = rt().retain(domain);
    p->userinfo = userinfo ? rt().retain(userinfo) : 0;
    p->code = code;
    return o;
}
Addr make_error(const std::string& domain, int64_t code, const std::string& description, const std::string& path) {
    const Addr dom = new_string(domain);
    const Addr dict = rt().create_instance(classes().mdict, 0);
    Addr info = 0;
    if (dict) {
        if (DictPayload* dp = dict_of(dict)) {
            (void)dp;
            const Addr desc = new_string(description);
            rt().send(dict, "setObject:forKey:", {desc, key_localized_description});
            rt().release(desc);
            if (!path.empty()) {
                const Addr ps = new_string(path);
                const Addr key = new_string("NSFilePath");
                rt().send(dict, "setObject:forKey:", {ps, key});
                rt().release(ps);
                rt().release(key);
            }
        }
        info = dict;
    }
    const Addr err = rt().create_instance(classes().error, 0);
    Addr result = err ? init_error(err, dom, code, info) : 0;
    rt().release(dom);
    if (info) rt().release(info);
    return result ? autoreleased(result) : 0;
}
Addr e_errorWithDomain(Addr self, Addr, Addr domain, int64_t code) { (void)self; (void)domain; (void)code; return 0; }   // raw version registered
void e_errorWithDomainRaw(GuestRegs* r) {                           // +errorWithDomain:code:userInfo: self,_cmd,domain,code,userInfo
    const Addr o = rt().create_instance(classes().error, 0);
    if (!o) return;
    if (!init_error(o, r->gp[2], static_cast<int64_t>(r->gp[3]), r->gp[4])) { rt().release(o); return; }
    r->ret_rax = autoreleased(o);
}
void e_initWithDomainRaw(GuestRegs* r) {
    r->ret_rax = init_error(r->gp[0], r->gp[2], static_cast<int64_t>(r->gp[3]), r->gp[4]);
}
Addr e_dealloc(Addr self, Addr) {
    if (ErrorPayload* p = error_of(self)) {
        const Addr d = p->domain, u = p->userinfo;
        p->domain = p->userinfo = 0;
        if (d) rt().release(d);
        if (u) rt().release(u);
    }
    if (uint64_t* slot = payload_slot(self)) { store_drop(*slot); *slot = 0; }
    rt().destruct_instance(self);
    rt().dispose_instance(self);
    return 0;
}
Addr e_domain(Addr self, Addr) { ErrorPayload* p = error_of(self); return p ? p->domain : 0; }
int64_t e_code(Addr self, Addr) { ErrorPayload* p = error_of(self); return p ? p->code : 0; }
Addr e_userInfo(Addr self, Addr) { ErrorPayload* p = error_of(self); return p ? p->userinfo : 0; }
Addr e_localizedDescription(Addr self, Addr) {
    ErrorPayload* p = error_of(self);
    if (!p) return 0;
    if (p->userinfo) {
        const Addr v = rt().send(p->userinfo, "objectForKey:", {key_localized_description});
        if (v) return v;
    }
    return new_string_autoreleased(std::format("The operation couldn\xe2\x80\x99t be completed. ({} error {}.)", describe(p->domain), p->code));
}
Addr e_description(Addr self, Addr) {
    ErrorPayload* p = error_of(self);
    if (!p) return 0;
    return new_string_autoreleased(std::format("Error Domain={} Code={} \"{}\"", describe(p->domain), p->code, describe(e_localizedDescription(self, 0))));
}
Addr e_copyWithZone(Addr self, Addr, Addr) { return rt().retain(self); }

// ======================================================================= NSFileManager
Addr g_default_manager = 0;
Addr fm_defaultManager(Addr, Addr) { return g_default_manager; }

bool guest_path(Addr str, fs::path& host, int& err, std::string& guest) {
    bool ok;
    guest = std::string(sv_of(str, ok));
    if (!ok) { err = libsystem::detail::kEINVAL; return false; }
    err = libsystem::detail::resolve_guest_path(guest.c_str(), host);
    return err == 0;
}
void set_error(Addr out_ptr, int e, const std::string& path, const char* what) {
    if (!out_ptr || !rt().readable(out_ptr, 8)) return;
    int64_t code = 256;                                             // NSFileReadUnknownError
    std::string text = std::string("The file could not be ") + what + ".";
    switch (e) {
        case libsystem::detail::kENOENT: code = 4; text = "The file doesn\xe2\x80\x99t exist."; break;                   // NSFileNoSuchFileError
        case libsystem::detail::kEACCES: code = 513; text = "You don\xe2\x80\x99t have permission to access the file."; break;   // NSFileWriteNoPermissionError
        case libsystem::detail::kEEXIST: code = 516; text = "The file already exists."; break;                         // NSFileWriteFileExistsError
        default: break;
    }
    *reinterpret_cast<Addr*>(out_ptr) = make_error("NSCocoaErrorDomain", code, text, path);
}
uint64_t fm_fileExistsAtPath(Addr self, Addr, Addr path) {
    (void)self;
    fs::path host;
    int e;
    std::string g;
    std::error_code ec;
    return guest_path(path, host, e, g) && fs::exists(host, ec) ? 1 : 0;
}
uint64_t fm_fileExistsAtPathIsDirectory(Addr self, Addr, Addr path, Addr is_dir) {
    (void)self;
    fs::path host;
    int e;
    std::string g;
    std::error_code ec;
    if (!guest_path(path, host, e, g) || !fs::exists(host, ec)) return 0;
    if (is_dir && rt().readable(is_dir, 1)) *reinterpret_cast<uint8_t*>(is_dir) = fs::is_directory(host, ec) ? 1 : 0;
    return 1;
}
Addr fm_contentsAtPath(Addr self, Addr, Addr path) {
    (void)self;
    fs::path host;
    int e;
    std::string g;
    std::error_code ec;
    if (!guest_path(path, host, e, g) || !fs::is_regular_file(host, ec)) return 0;
    const auto size = fs::file_size(host, ec);
    if (ec || size > kMaxFile) return 0;
    std::ifstream f(host, std::ios::binary);
    if (!f) return 0;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    return make_data(classes().data, std::move(bytes), true);
}
void fm_createFileAtPath(GuestRegs* r) {                            // createFileAtPath:contents:attributes: self,_cmd,path,data,attrs
    fs::path host;
    int e;
    std::string g;
    if (!guest_path(r->gp[2], host, e, g)) { r->ret_rax = 0; return; }
    std::error_code ec;
    if (fs::is_directory(host, ec)) { r->ret_rax = 0; return; }
    DataPayload* d = r->gp[3] ? data_of(r->gp[3]) : nullptr;
    if (r->gp[3] && !d) { raise("NSInvalidArgumentException", "-[NSFileManager createFileAtPath:contents:attributes:]: contents is not NSData"); return; }
    std::ofstream f(host, std::ios::binary | std::ios::trunc);
    if (!f) { r->ret_rax = 0; return; }
    if (d && !d->bytes.empty()) f.write(reinterpret_cast<const char*>(d->bytes.data()), static_cast<std::streamsize>(d->bytes.size()));
    r->ret_rax = f.good() ? 1 : 0;
}
uint64_t fm_removeItemAtPath(Addr self, Addr, Addr path, Addr error_out) {
    (void)self;
    fs::path host;
    int e;
    std::string g;
    if (!guest_path(path, host, e, g)) { set_error(error_out, e, g, "removed"); return 0; }
    std::error_code ec;
    if (fs::equivalent(host, libsystem::context()->root, ec)) { set_error(error_out, libsystem::detail::kEACCES, g, "removed"); return 0; }     // never remove the root itself
    if (!fs::exists(host, ec)) { set_error(error_out, libsystem::detail::kENOENT, g, "removed"); return 0; }
    fs::remove_all(host, ec);
    if (ec) { set_error(error_out, libsystem::detail::kEACCES, g, "removed"); return 0; }
    return 1;
}
void fm_createDirectory(GuestRegs* r) {                             // createDirectoryAtPath:withIntermediateDirectories:attributes:error:
    fs::path host;
    int e;
    std::string g;
    if (!guest_path(r->gp[2], host, e, g)) { set_error(r->gp[5], e, g, "created"); r->ret_rax = 0; return; }
    std::error_code ec;
    const bool intermediate = (r->gp[3] & 0xff) != 0;
    bool ok;
    if (fs::exists(host, ec)) {
        ok = fs::is_directory(host, ec) && intermediate;
        if (!ok) set_error(r->gp[5], libsystem::detail::kEEXIST, g, "created");
    } else {
        ok = intermediate ? fs::create_directories(host, ec) : fs::create_directory(host, ec);
        if (!ok || ec) { set_error(r->gp[5], ec ? libsystem::detail::kENOENT : libsystem::detail::kEACCES, g, "created"); ok = false; }
    }
    r->ret_rax = ok ? 1 : 0;
}
Addr fm_contentsOfDirectory(Addr self, Addr, Addr path, Addr error_out) {
    (void)self;
    fs::path host;
    int e;
    std::string g;
    if (!guest_path(path, host, e, g)) { set_error(error_out, e, g, "read"); return 0; }
    std::error_code ec;
    if (!fs::is_directory(host, ec)) { set_error(error_out, libsystem::detail::kENOENT, g, "read"); return 0; }
    std::vector<std::string> names;
    for (fs::directory_iterator it(host, ec), end; !ec && it != end; it.increment(ec)) names.push_back(path_to_utf8(it->path().filename()));
    std::sort(names.begin(), names.end());
    std::vector<Addr> items;
    for (auto& n : names) items.push_back(new_string(n));
    return new_array(std::move(items), false, false);
}
Addr fm_currentDirectoryPath(Addr, Addr) { return new_string_autoreleased("/"); }

std::unordered_map<std::string, Addr> vars;

void install_constants(Runtime& r) {
    key_localized_description = immortal_string("NSLocalizedDescription");
    vars.clear();
    const std::pair<const char*, const char*> table[] = {{"_NSLocalizedDescriptionKey", "NSLocalizedDescription"}, {"_NSFilePathErrorKey", "NSFilePath"},
                                                         {"_NSUnderlyingErrorKey", "NSUnderlyingError"},         {"_NSCocoaErrorDomain", "NSCocoaErrorDomain"},
                                                         {"_NSPOSIXErrorDomain", "NSPOSIXErrorDomain"},         {"_NSURLErrorDomain", "NSURLErrorDomain"}};
    for (auto& [sym, text] : table) {
        const Addr var = r.arena_alloc(8, 8);
        *reinterpret_cast<Addr*>(var) = std::string_view(text) == "NSLocalizedDescription" ? key_localized_description : immortal_string(text);
        vars[sym] = var;
    }
    r.add_dynamic_provider([](std::string_view name) -> std::optional<uint64_t> {
        auto it = vars.find(std::string(name));
        if (it != vars.end()) return it->second;
        return std::nullopt;
    });
}

}  // namespace

void install_nsextras(Runtime& r) {
    // ---- NSMutableString
    Class* ms = r.declare_native_class("NSMutableString", "NSString", 16);
    classes().mstring = ms;
    r.add_native_methods(ms, true, {{"stringWithCapacity:", "@24@0:8Q16", addr(ms_stringWithCapacity), false}});
    r.add_native_methods(ms, false, {
        {"initWithCapacity:", "@24@0:8Q16", addr(ms_initWithCapacity), false},
        {"appendString:", "v24@0:8@16", addr(ms_appendString), false},
        {"appendFormat:", "v24@0:8@16", addr(ms_appendFormat), true},
        {"setString:", "v24@0:8@16", addr(ms_setString), false},
        {"insertString:atIndex:", "v32@0:8@16Q24", addr(ms_insertString), false},
        {"deleteCharactersInRange:", "v32@0:8{_NSRange=QQ}16", addr(ms_deleteCharacters), false},
        {"replaceCharactersInRange:withString:", "v40@0:8{_NSRange=QQ}16@32", addr(ms_replaceCharactersRaw), true},
    });
    (void)ms_replaceCharacters;
    // ---- NSData / NSMutableData
    Class* d = r.declare_native_class("NSData", "NSObject", 16);
    classes().data = d;
    classes().extra_payload.push_back(d);
    Class* md = r.declare_native_class("NSMutableData", "NSData", 16);
    classes().mdata = md;
    r.add_native_methods(d, true, {
        {"data", "@16@0:8", addr(d_data), false},
        {"dataWithBytes:length:", "@32@0:8r^v16Q24", addr(d_dataWithBytesLength), false},
        {"dataWithData:", "@24@0:8@16", addr(d_dataWithData), false},
    });
    r.add_native_methods(d, false, {
        {"init", "@16@0:8", addr(d_init), false},
        {"initWithBytes:length:", "@32@0:8r^v16Q24", addr(d_initWithBytesLength), false},
        {"dealloc", "v16@0:8", addr(d_dealloc), false},
        {"length", "Q16@0:8", addr(d_length), false},
        {"bytes", "r^v16@0:8", addr(d_bytes), false},
        {"getBytes:length:", "v32@0:8^v16Q24", addr(d_getBytesLength), false},
        {"isEqualToData:", "B24@0:8@16", addr(d_isEqualToData), false},
        {"isEqual:", "B24@0:8@16", addr(d_isEqual), false},
        {"hash", "Q16@0:8", addr(d_hash), false},
        {"subdataWithRange:", "@32@0:8{_NSRange=QQ}16", addr(d_subdataWithRange), false},
        {"description", "@16@0:8", addr(d_description), false},
        {"copyWithZone:", "@24@0:8^{_NSZone=}16", addr(d_copyWithZone), false},
        {"mutableCopyWithZone:", "@24@0:8^{_NSZone=}16", addr(d_mutableCopyWithZone), false},
    });
    r.add_native_methods(md, true, {{"dataWithCapacity:", "@24@0:8Q16", addr(md_dataWithCapacity), false}, {"dataWithLength:", "@24@0:8Q16", addr(md_dataWithLength), false}});
    r.add_native_methods(md, false, {
        {"initWithCapacity:", "@24@0:8Q16", addr(md_initWithCapacity), false},
        {"mutableBytes", "^v16@0:8", addr(md_mutableBytes), false},
        {"appendBytes:length:", "v32@0:8r^v16Q24", addr(md_appendBytesLength), false},
        {"appendData:", "v24@0:8@16", addr(md_appendData), false},
        {"setLength:", "v24@0:8Q16", addr(md_setLength), false},
        {"increaseLengthBy:", "v24@0:8Q16", addr(md_increaseLengthBy), false},
        {"replaceBytesInRange:withBytes:", "v40@0:8{_NSRange=QQ}16r^v32", addr(md_replaceBytes), true},
    });
    // ---- NSDate
    Class* dt = r.declare_native_class("NSDate", "NSObject", 16);
    classes().date = dt;
    classes().extra_payload.push_back(dt);
    r.add_native_methods(dt, true, {
        {"date", "@16@0:8", addr(dt_date), true},
        {"dateWithTimeIntervalSince1970:", "@24@0:8d16", addr(dt_dateWithTimeIntervalSince1970), true},
        {"dateWithTimeIntervalSinceReferenceDate:", "@24@0:8d16", addr(dt_dateWithTimeIntervalSinceReferenceDate), true},
        {"dateWithTimeIntervalSinceNow:", "@24@0:8d16", addr(dt_dateWithTimeIntervalSinceNow), true},
    });
    r.add_native_methods(dt, false, {
        {"init", "@16@0:8", addr(dt_init), false},
        {"initWithTimeIntervalSince1970:", "@24@0:8d16", addr(dt_initWithTimeIntervalSince1970), true},
        {"dealloc", "v16@0:8", addr(dt_dealloc), false},
        {"timeIntervalSince1970", "d16@0:8", addr(dt_timeIntervalSince1970), true},
        {"timeIntervalSinceReferenceDate", "d16@0:8", addr(dt_timeIntervalSinceReferenceDate), true},
        {"timeIntervalSinceNow", "d16@0:8", addr(dt_timeIntervalSinceNow), true},
        {"timeIntervalSinceDate:", "d24@0:8@16", addr(dt_timeIntervalSinceDate), true},
        {"dateByAddingTimeInterval:", "@24@0:8d16", addr(dt_dateByAddingTimeInterval), true},
        {"compare:", "q24@0:8@16", addr(dt_compare), false},
        {"isEqualToDate:", "B24@0:8@16", addr(dt_isEqualToDate), false},
        {"isEqual:", "B24@0:8@16", addr(dt_isEqual), false},
        {"hash", "Q16@0:8", addr(dt_hash), false},
        {"earlierDate:", "@24@0:8@16", addr(dt_earlierDate), false},
        {"laterDate:", "@24@0:8@16", addr(dt_laterDate), false},
        {"description", "@16@0:8", addr(dt_description), false},
        {"copyWithZone:", "@24@0:8^{_NSZone=}16", addr(dt_copyWithZone), false},
    });
    // ---- NSURL
    Class* u = r.declare_native_class("NSURL", "NSObject", 16);
    classes().url = u;
    classes().extra_payload.push_back(u);
    r.add_native_methods(u, true, {{"URLWithString:", "@24@0:8@16", addr(u_URLWithString), false}, {"fileURLWithPath:", "@24@0:8@16", addr(u_fileURLWithPath), false}});
    r.add_native_methods(u, false, {
        {"dealloc", "v16@0:8", addr(u_dealloc), false},
        {"absoluteString", "@16@0:8", addr(u_absoluteString), false},
        {"relativeString", "@16@0:8", addr(u_absoluteString), false},
        {"path", "@16@0:8", addr(u_path), false},
        {"relativePath", "@16@0:8", addr(u_path), false},
        {"scheme", "@16@0:8", addr(u_scheme), false},
        {"host", "@16@0:8", addr(u_host), false},
        {"query", "@16@0:8", addr(u_query), false},
        {"isFileURL", "B16@0:8", addr(u_isFileURL), false},
        {"lastPathComponent", "@16@0:8", addr(u_lastPathComponent), false},
        {"pathExtension", "@16@0:8", addr(u_pathExtension), false},
        {"URLByAppendingPathComponent:", "@24@0:8@16", addr(u_URLByAppendingPathComponent), false},
        {"isEqual:", "B24@0:8@16", addr(u_isEqual), false},
        {"hash", "Q16@0:8", addr(u_hash), false},
        {"description", "@16@0:8", addr(u_description), false},
        {"copyWithZone:", "@24@0:8^{_NSZone=}16", addr(u_copyWithZone), false},
    });
    // ---- NSError
    Class* e = r.declare_native_class("NSError", "NSObject", 16);
    classes().error = e;
    classes().extra_payload.push_back(e);
    r.add_native_methods(e, true, {{"errorWithDomain:code:userInfo:", "@40@0:8@16q24@32", addr(e_errorWithDomainRaw), true}});
    r.add_native_methods(e, false, {
        {"initWithDomain:code:userInfo:", "@40@0:8@16q24@32", addr(e_initWithDomainRaw), true},
        {"dealloc", "v16@0:8", addr(e_dealloc), false},
        {"domain", "@16@0:8", addr(e_domain), false},
        {"code", "q16@0:8", addr(e_code), false},
        {"userInfo", "@16@0:8", addr(e_userInfo), false},
        {"localizedDescription", "@16@0:8", addr(e_localizedDescription), false},
        {"description", "@16@0:8", addr(e_description), false},
        {"copyWithZone:", "@24@0:8^{_NSZone=}16", addr(e_copyWithZone), false},
    });
    (void)e_errorWithDomain;
    // ---- NSFileManager (one immortal shared instance)
    Class* fm = r.declare_native_class("NSFileManager", "NSObject", 16);
    classes().filemgr = fm;
    g_default_manager = r.arena_alloc(16, 16);
    reinterpret_cast<Addr*>(g_default_manager)[0] = fm->addr;
    reinterpret_cast<Addr*>(g_default_manager)[1] = 0;
    r.add_native_methods(fm, true, {{"defaultManager", "@16@0:8", addr(fm_defaultManager), false}});
    r.add_native_methods(fm, false, {
        {"fileExistsAtPath:", "B24@0:8@16", addr(fm_fileExistsAtPath), false},
        {"fileExistsAtPath:isDirectory:", "B32@0:8@16^B24", addr(fm_fileExistsAtPathIsDirectory), false},
        {"contentsAtPath:", "@24@0:8@16", addr(fm_contentsAtPath), false},
        {"createFileAtPath:contents:attributes:", "B40@0:8@16@24@32", addr(fm_createFileAtPath), true},
        {"removeItemAtPath:error:", "B32@0:8@16^@24", addr(fm_removeItemAtPath), false},
        {"createDirectoryAtPath:withIntermediateDirectories:attributes:error:", "B48@0:8@16B24@32^@40", addr(fm_createDirectory), true},
        {"contentsOfDirectoryAtPath:error:", "@32@0:8@16^@24", addr(fm_contentsOfDirectory), false},
        {"currentDirectoryPath", "@16@0:8", addr(fm_currentDirectoryPath), false},
    });
    install_constants(r);
}

}  // namespace orvyn::foundation
