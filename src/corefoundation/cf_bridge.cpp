// CoreFoundation subset (ORVYN 0.5.5). Design: the CF types ORVYN knows are the Foundation objects themselves ("toll-free bridged by
// construction"): a CFStringRef IS an NSString, a CFArrayRef an NSArray, a CFDictionaryRef an NSDictionary. The functions below are thin,
// validated wrappers; they only accept objects created by this runtime (a pointer that is not a live object of the expected class is
// refused). There is no general bridging layer: other CF types (CFData, CFDate, CFURL, CFSet...) do not exist yet.
//
// Not implemented: custom array/dictionary callbacks (only kCFTypeArrayCallBacks / kCFTypeDictionary*CallBacks; NULL callbacks are refused for
// non-empty collections because they imply non-object values), CFString encodings other than UTF-8/ASCII/ISO Latin-1, CFNumber types beyond
// the integer and floating-point ones, CFRunLoop modes (there is one mode), version-1 (Mach port) sources, observers, CFRunLoopTimer
// flags/order, CFAllocator (the allocator argument is ignored), CFPropertyList, CFPreferences.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <unordered_map>

#include "orvyn/abi.hpp"
#include "orvyn/libsystem.hpp"
#include "orvyn/log.hpp"
#include "orvyn/objc.hpp"
#include "orvyn/runloop.hpp"
#include "../foundation/foundation_internal.hpp"
#include "../libsystem/internal.hpp"
#include "cf_bridge.hpp"
#include "runloop_internal.hpp"

namespace orvyn::corefoundation {
namespace {

using abi::GuestRegs;
using abi::RegsReader;
using objc::Addr;
using foundation::classes;
using foundation::rt;
using libsystem::detail::trap;

constexpr uint32_t kEncUTF8 = 0x08000100, kEncASCII = 0x0600, kEncLatin1 = 0x0201, kEncMacRoman = 0;
constexpr double kUnixToAbsolute = 978307200.0;

// CFTypeIDs are small numbers per class family (values are ORVYN's own; Apple's change between releases).
enum TypeId : uint64_t { kTypeNull = 1, kTypeString = 7, kTypeNumber = 22, kTypeArray = 19, kTypeDictionary = 18, kTypeRunLoop = 11, kTypeSource = 12, kTypeTimer = 13, kTypeOther = 99 };

Addr g_array_callbacks = 0, g_dict_key_callbacks = 0, g_dict_value_callbacks = 0, g_runloop = 0;
Addr g_allocator_default = 0, g_allocator_null = 0, g_mode_default = 0, g_mode_common = 0;
Addr g_bool_true = 0, g_bool_false = 0;

bool is_string(Addr o) { return foundation::is_kind(o, classes().string); }
bool is_array(Addr o) { return foundation::is_kind(o, classes().array); }
bool is_dict(Addr o) { return foundation::is_kind(o, classes().dict); }
bool live(Addr o) { return o && rt().class_of_object(o) != nullptr; }

// ---- encodings ----------------------------------------------------------------------------------------------------------------------------
bool decode(const char* bytes, uint32_t enc, std::string& utf8) {
    if (!bytes) return false;
    if (enc == kEncUTF8) { utf8 = bytes; return foundation::valid_utf8(utf8); }
    std::string raw = bytes;
    if (enc == kEncASCII) { for (unsigned char c : raw) if (c >= 0x80) return false; utf8 = raw; return true; }
    if (enc == kEncLatin1 || enc == kEncMacRoman) {                      // MacRoman treated as Latin-1 for the ASCII range, which is all that is verified
        utf8.clear();
        for (unsigned char c : raw) { if (c < 0x80) utf8.push_back(static_cast<char>(c)); else { utf8.push_back(static_cast<char>(0xC0 | (c >> 6))); utf8.push_back(static_cast<char>(0x80 | (c & 0x3F))); } }
        return true;
    }
    return false;
}

// ---- retain / release / identity -----------------------------------------------------------------------------------------------------------
Addr w_CFRetain(Addr o) { return o ? rt().retain(o) : 0; }
void w_CFRelease(Addr o) { if (o) rt().release(o); }
int64_t w_CFGetRetainCount(Addr o) {
    if (auto* st = rt().object_state(o)) return st->rc;
    return 0x7fffffffffffffffll;                                                // immortal objects
}
uint64_t w_CFGetTypeID(Addr o) {
    if (!live(o)) { trap("CFGetTypeID: not a CoreFoundation object"); return 0; }
    if (is_string(o)) return kTypeString;
    if (is_array(o)) return kTypeArray;
    if (is_dict(o)) return kTypeDictionary;
    if (foundation::is_kind(o, classes().number)) return kTypeNumber;
    if (o == g_runloop) return kTypeRunLoop;
    const std::string& n = rt().class_of_object(o)->name;
    if (n == "__NSCFRunLoopSource") return kTypeSource;
    if (n == "__NSCFRunLoopTimer") return kTypeTimer;
    return kTypeOther;
}
uint64_t w_CFStringGetTypeID() { return kTypeString; }
uint64_t w_CFArrayGetTypeID() { return kTypeArray; }
uint64_t w_CFDictionaryGetTypeID() { return kTypeDictionary; }
uint64_t w_CFNumberGetTypeID() { return kTypeNumber; }
uint64_t w_CFRunLoopGetTypeID() { return kTypeRunLoop; }
uint64_t w_CFEqual(Addr a, Addr b) { return a == b || (live(a) && live(b) && (rt().send(a, "isEqual:", {b}) & 0xff)) ? 1 : 0; }
uint64_t w_CFHash(Addr o) { return live(o) ? rt().send(o, "hash") : 0; }
Addr w_CFCopyDescription(Addr o) {
    if (!live(o)) return 0;
    return rt().retain(foundation::new_string_autoreleased(foundation::describe(o)));
}
void w_CFShow(Addr o) {
    const std::string s = (o ? foundation::describe(o) : std::string("(null)")) + "\n";
    libsystem::console_write(2, s.data(), s.size());
}

// ---- CFString -------------------------------------------------------------------------------------------------------------------------------------
Addr w_CFStringCreateWithCString(Addr, const char* cstr, uint32_t enc) {
    std::string utf8;
    if (!decode(cstr, enc, utf8)) return 0;
    return foundation::new_string(std::move(utf8));                             // +1
}
void w_CFStringCreateWithFormat(GuestRegs* r) {                                  // (allocator, formatOptions, format, ...)
    RegsReader a(*r);
    a.next_int();
    a.next_int();
    std::string_view fmt;
    if (!foundation::string_view_of(a.next_int(), fmt)) { trap("CFStringCreateWithFormat: the format is not a CFString"); return; }
    std::string out;
    if (!foundation::format_with_objects(std::string(fmt), a, out)) return;
    r->ret_rax = foundation::new_string(std::move(out));
}
int64_t w_CFStringGetLength(Addr s) {
    std::string_view sv;
    if (!foundation::string_view_of(s, sv)) { trap("CFStringGetLength: not a CFString"); return 0; }
    return static_cast<int64_t>(foundation::utf16_length(sv));
}
uint32_t w_CFStringGetCharacterAtIndex(Addr s, int64_t idx) {
    std::string_view sv;
    if (!foundation::string_view_of(s, sv)) { trap("CFStringGetCharacterAtIndex: not a CFString"); return 0; }
    const auto u = foundation::to_u16(sv);
    if (idx < 0 || static_cast<size_t>(idx) >= u.size()) { trap("CFStringGetCharacterAtIndex: index out of range"); return 0; }
    return u[static_cast<size_t>(idx)];
}
uint64_t w_CFStringGetCString(Addr s, uint64_t buf, int64_t size, uint32_t enc) {
    std::string_view sv;
    if (!foundation::string_view_of(s, sv) || size <= 0 || !buf || !rt().readable(buf, static_cast<size_t>(size))) return 0;
    std::string out;
    if (enc == kEncUTF8) out = std::string(sv);
    else if (enc == kEncASCII) { for (unsigned char c : sv) if (c >= 0x80) return 0; out = std::string(sv); }
    else return 0;
    if (out.size() + 1 > static_cast<size_t>(size)) return 0;                  // does not fit: false, like CF
    std::memcpy(reinterpret_cast<void*>(buf), out.c_str(), out.size() + 1);
    return 1;
}
Addr w_CFStringGetCStringPtr(Addr s, uint32_t enc) {
    std::string_view sv;
    if (!foundation::string_view_of(s, sv) || (enc != kEncUTF8 && enc != kEncASCII)) return 0;
    if (enc == kEncASCII) for (unsigned char c : sv) if (c >= 0x80) return 0;
    return reinterpret_cast<Addr>(sv.data());                                   // NUL-terminated by construction
}
int64_t w_CFStringCompare(Addr a, Addr b, uint64_t flags) {
    std::string_view x, y;
    if (!foundation::string_view_of(a, x) || !foundation::string_view_of(b, y)) { trap("CFStringCompare: not CFStrings"); return 0; }
    std::string p(x), q(y);
    if (flags & 1) { for (auto& c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); for (auto& c : q) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    const int c = p.compare(q);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}
Addr w_CFStringCreateCopy(Addr, Addr s) {
    std::string_view sv;
    return foundation::string_view_of(s, sv) ? foundation::new_string(std::string(sv)) : 0;
}
Addr w_CFStringCreateMutable(Addr, int64_t) { return rt().create_instance(classes().mstring, 0); }
void w_CFStringAppend(Addr s, Addr t) { rt().send(s, "appendString:", {t}); }
void w_CFStringAppendCString(Addr s, const char* cstr, uint32_t enc) {
    std::string utf8;
    if (!decode(cstr, enc, utf8)) { trap("CFStringAppendCString: invalid string or unsupported encoding"); return; }
    const Addr t = foundation::new_string(std::move(utf8));
    rt().send(s, "appendString:", {t});
    rt().release(t);
}

// ---- CFArray ------------------------------------------------------------------------------------------------------------------------------------------
bool check_callbacks(Addr cb, Addr expected, int64_t count, const char* who) {
    if (cb == expected) return true;
    if (!cb && count == 0) return true;
    trap(std::string(who) + ": only the kCFType...CallBacks are supported (NULL and custom callbacks imply non-object values)");
    return false;
}
Addr new_array(const Addr* values, int64_t n, bool mut) {
    Addr a = rt().create_instance(mut ? classes().marray : classes().array, 0);
    if (!a) return 0;
    foundation::ArrayPayload* p = foundation::array_of(a);
    if (!p) { rt().release(a); return 0; }
    for (int64_t i = 0; i < n; ++i) { rt().retain(values[i]); p->items.push_back(values[i]); }
    return a;
}
Addr w_CFArrayCreate(Addr, uint64_t values, int64_t n, Addr cb) {
    if (n < 0 || n > (1 << 26) || (n && (!values || !rt().readable(values, static_cast<size_t>(n) * 8)))) { trap("CFArrayCreate: invalid values pointer or count"); return 0; }
    if (!check_callbacks(cb, g_array_callbacks, n, "CFArrayCreate")) return 0;
    return new_array(reinterpret_cast<const Addr*>(values), n, false);
}
Addr w_CFArrayCreateMutable(Addr, int64_t, Addr cb) {
    if (!check_callbacks(cb, g_array_callbacks, 0, "CFArrayCreateMutable")) return 0;
    return new_array(nullptr, 0, true);
}
int64_t w_CFArrayGetCount(Addr a) {
    foundation::ArrayPayload* p = is_array(a) ? foundation::array_of(a) : nullptr;
    if (!p) { trap("CFArrayGetCount: not a CFArray"); return 0; }
    return static_cast<int64_t>(p->items.size());
}
Addr w_CFArrayGetValueAtIndex(Addr a, int64_t i) {
    foundation::ArrayPayload* p = is_array(a) ? foundation::array_of(a) : nullptr;
    if (!p) { trap("CFArrayGetValueAtIndex: not a CFArray"); return 0; }
    if (i < 0 || static_cast<size_t>(i) >= p->items.size()) { trap(std::format("CFArrayGetValueAtIndex: index {} out of range (count {})", i, p->items.size())); return 0; }
    return p->items[static_cast<size_t>(i)];
}
void w_CFArrayAppendValue(Addr a, Addr v) {
    if (!foundation::is_kind(a, classes().marray)) { trap("CFArrayAppendValue: not a mutable CFArray"); return; }
    if (!v) { trap("CFArrayAppendValue: NULL value"); return; }
    rt().send(a, "addObject:", {v});
}
void w_CFArrayRemoveValueAtIndex(Addr a, int64_t i) {
    if (!foundation::is_kind(a, classes().marray)) { trap("CFArrayRemoveValueAtIndex: not a mutable CFArray"); return; }
    rt().send(a, "removeObjectAtIndex:", {static_cast<uint64_t>(i)});
}

// ---- CFDictionary ---------------------------------------------------------------------------------------------------------------------------------------
Addr new_dict(bool mut) { return rt().create_instance(mut ? classes().mdict : classes().dict, 0); }
Addr w_CFDictionaryCreate(Addr, uint64_t keys, uint64_t values, int64_t n, Addr kcb, Addr vcb) {
    if (n < 0 || n > (1 << 24) || (n && (!keys || !values || !rt().readable(keys, static_cast<size_t>(n) * 8) || !rt().readable(values, static_cast<size_t>(n) * 8)))) { trap("CFDictionaryCreate: invalid keys/values pointers or count"); return 0; }
    if (!check_callbacks(kcb, g_dict_key_callbacks, n, "CFDictionaryCreate") || !check_callbacks(vcb, g_dict_value_callbacks, n, "CFDictionaryCreate")) return 0;
    Addr d = new_dict(false);
    if (!d || !foundation::dict_of(d)) return 0;
    for (int64_t i = 0; i < n; ++i) foundation::dict_set(d, reinterpret_cast<const Addr*>(keys)[i], reinterpret_cast<const Addr*>(values)[i]);
    return d;
}
Addr w_CFDictionaryCreateMutable(Addr, int64_t, Addr kcb, Addr vcb) {
    if (!check_callbacks(kcb, g_dict_key_callbacks, 0, "CFDictionaryCreateMutable") || !check_callbacks(vcb, g_dict_value_callbacks, 0, "CFDictionaryCreateMutable")) return 0;
    Addr d = new_dict(true);
    return d && foundation::dict_of(d) ? d : 0;
}
int64_t w_CFDictionaryGetCount(Addr d) {
    foundation::DictPayload* p = is_dict(d) ? foundation::dict_of(d) : nullptr;
    if (!p) { trap("CFDictionaryGetCount: not a CFDictionary"); return 0; }
    return static_cast<int64_t>(p->entries.size());
}
Addr w_CFDictionaryGetValue(Addr d, Addr key) {
    if (!is_dict(d)) { trap("CFDictionaryGetValue: not a CFDictionary"); return 0; }
    return rt().send(d, "objectForKey:", {key});
}
uint64_t w_CFDictionaryContainsKey(Addr d, Addr key) { return w_CFDictionaryGetValue(d, key) != 0 ? 1 : 0; }
void w_CFDictionarySetValue(Addr d, Addr key, Addr value) {
    if (!foundation::is_kind(d, classes().mdict)) { trap("CFDictionarySetValue: not a mutable CFDictionary"); return; }
    rt().send(d, "setObject:forKey:", {value, key});
}
void w_CFDictionaryAddValue(Addr d, Addr key, Addr value) { if (!w_CFDictionaryGetValue(d, key)) w_CFDictionarySetValue(d, key, value); }
void w_CFDictionaryRemoveValue(Addr d, Addr key) {
    if (!foundation::is_kind(d, classes().mdict)) { trap("CFDictionaryRemoveValue: not a mutable CFDictionary"); return; }
    rt().send(d, "removeObjectForKey:", {key});
}
void w_CFDictionaryGetKeysAndValues(Addr d, uint64_t keys, uint64_t values) {
    foundation::DictPayload* p = is_dict(d) ? foundation::dict_of(d) : nullptr;
    if (!p) { trap("CFDictionaryGetKeysAndValues: not a CFDictionary"); return; }
    const size_t n = p->entries.size();
    if ((keys && !rt().readable(keys, n * 8)) || (values && !rt().readable(values, n * 8))) { trap("CFDictionaryGetKeysAndValues: output buffers are not writable"); return; }
    for (size_t i = 0; i < n; ++i) {
        if (keys) reinterpret_cast<Addr*>(keys)[i] = p->entries[i].key;
        if (values) reinterpret_cast<Addr*>(values)[i] = p->entries[i].value;
    }
}

// ---- CFNumber -----------------------------------------------------------------------------------------------------------------------------------------------
// kCFNumberSInt8Type=1 SInt16=2 SInt32=3 SInt64=4 Float32=5 Float64=6 Char=7 Short=8 Int=9 Long=10 LongLong=11 Float=12 Double=13 CFIndex=14 NSInteger=15
int type_size(int t) {
    switch (t) { case 1: case 7: return 1; case 2: case 8: return 2; case 3: case 5: case 9: case 12: return 4; case 4: case 6: case 10: case 11: case 13: case 14: case 15: return 8; default: return 0; }
}
bool type_is_float(int t) { return t == 5 || t == 6 || t == 12 || t == 13; }
Addr w_CFNumberCreate(Addr, int type, uint64_t ptr) {
    const int sz = type_size(type);
    if (!sz || !ptr || !rt().readable(ptr, static_cast<size_t>(sz))) { trap("CFNumberCreate: unsupported number type or invalid value pointer"); return 0; }
    foundation::NumberPayload n;
    if (type_is_float(type)) {
        n.kind = foundation::NumberPayload::Kind::Double;
        if (sz == 4) { float f; std::memcpy(&f, reinterpret_cast<void*>(ptr), 4); n.d = f; } else std::memcpy(&n.d, reinterpret_cast<void*>(ptr), 8);
    } else {
        n.kind = foundation::NumberPayload::Kind::Int;
        switch (sz) { case 1: n.i = *reinterpret_cast<int8_t*>(ptr); break; case 2: n.i = *reinterpret_cast<int16_t*>(ptr); break; case 4: n.i = *reinterpret_cast<int32_t*>(ptr); break; default: n.i = *reinterpret_cast<int64_t*>(ptr); }
    }
    return foundation::make_object(classes().number, std::make_unique<foundation::NumberPayload>(n));
}
uint64_t w_CFNumberGetValue(Addr num, int type, uint64_t ptr) {
    foundation::NumberPayload n;
    const int sz = type_size(type);
    if (!foundation::number_value(num, n) || !sz || !ptr || !rt().readable(ptr, static_cast<size_t>(sz))) return 0;
    if (type_is_float(type)) {
        const double d = n.kind == foundation::NumberPayload::Kind::Double ? n.d : static_cast<double>(n.i);
        if (sz == 4) { const float f = static_cast<float>(d); std::memcpy(reinterpret_cast<void*>(ptr), &f, 4); } else std::memcpy(reinterpret_cast<void*>(ptr), &d, 8);
    } else {
        const int64_t v = n.kind == foundation::NumberPayload::Kind::Double ? static_cast<int64_t>(n.d) : n.i;
        std::memcpy(reinterpret_cast<void*>(ptr), &v, static_cast<size_t>(sz));      // little endian: the low bytes are the narrower value
    }
    return 1;
}
uint64_t w_CFBooleanGetValue(Addr b) { foundation::NumberPayload n; return foundation::number_value(b, n) && n.i != 0 ? 1 : 0; }

// ---- time ------------------------------------------------------------------------------------------------------------------------------------------------------
void w_CFAbsoluteTimeGetCurrent(GuestRegs* r) {
    const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() - kUnixToAbsolute;
    std::memcpy(&r->ret_xmm0, &now, 8);
}

// ---- run loop objects ---------------------------------------------------------------------------------------------------------------------------------------
std::unordered_map<Addr, std::unique_ptr<runloop::cf::Source>> g_sources;
struct TimerRec { uint64_t host_id = 0; Addr callout = 0, info = 0; bool valid = true; double interval = 0; double fire_abs = 0; };
std::unordered_map<Addr, TimerRec> g_timers;
objc::Class *g_cls_source = nullptr, *g_cls_timer = nullptr;

double now_abs() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() - kUnixToAbsolute; }

Addr w_CFRunLoopGetMain() { return g_runloop; }
Addr w_CFRunLoopGetCurrent() {
    if (libsystem::GuestThreads::current() && !libsystem::GuestThreads::current()->is_main) { trap("CFRunLoopGetCurrent: run loops of secondary threads are not implemented"); return 0; }
    return g_runloop;
}
void w_CFRunLoopRun() { runloop::run(-1, false); }
void w_CFRunLoopRunInMode(GuestRegs* r) {                                          // (mode, seconds, returnAfterSourceHandled)
    RegsReader a(*r);
    a.next_int();
    const double seconds = a.next_double();
    const bool after = (a.next_int() & 0xff) != 0;
    r->ret_rax = static_cast<uint64_t>(static_cast<int32_t>(runloop::run(seconds, after)));
}
void w_CFRunLoopStop(Addr) { runloop::stop(); }
void w_CFRunLoopWakeUp(Addr) { runloop::wake(); }
uint64_t w_CFRunLoopIsWaiting(Addr) { return 0; }

Addr w_CFRunLoopSourceCreate(Addr, int64_t order, uint64_t ctx) {
    if (!ctx || !rt().readable(ctx, 80)) { trap("CFRunLoopSourceCreate: the context is NULL or unreadable"); return 0; }
    const auto* c = reinterpret_cast<const uint64_t*>(ctx);
    if (c[0] != 0) { trap("CFRunLoopSourceCreate: only version-0 sources are supported (version 1 uses Mach ports)"); return 0; }
    Addr obj = rt().create_instance(g_cls_source, 0);
    if (!obj) return 0;
    auto s = std::make_unique<runloop::cf::Source>();
    s->obj = obj;
    s->order = order;
    s->info = c[1]; s->retain_fn = c[2]; s->release_fn = c[3]; s->schedule_fn = c[7]; s->cancel_fn = c[8]; s->perform_fn = c[9];
    if (s->retain_fn && libsystem::is_valid_callback(s->retain_fn)) rt().call_imp(s->retain_fn, {s->info});
    g_sources[obj] = std::move(s);
    return obj;
}
runloop::cf::Source* source_of(Addr s) {
    auto it = g_sources.find(s);
    return it == g_sources.end() ? nullptr : it->second.get();
}
void w_CFRunLoopSourceSignal(Addr s) { if (auto* x = source_of(s)) { x->signaled = true; runloop::wake(); } }
uint64_t w_CFRunLoopSourceIsValid(Addr s) { auto* x = source_of(s); return x && x->valid ? 1 : 0; }
void w_CFRunLoopSourceInvalidate(Addr s) {
    auto* x = source_of(s);
    if (!x || !x->valid) return;
    x->valid = false;
    auto& list = runloop::cf::added_sources();
    list.erase(std::remove(list.begin(), list.end(), x), list.end());
    if (x->cancel_fn && libsystem::is_valid_callback(x->cancel_fn)) rt().call_imp(x->cancel_fn, {x->info, g_runloop, g_mode_default});
}
void w_CFRunLoopAddSource(Addr, Addr s, Addr) {
    auto* x = source_of(s);
    if (!x || !x->valid) { trap("CFRunLoopAddSource: not a valid run loop source"); return; }
    auto& list = runloop::cf::added_sources();
    if (std::find(list.begin(), list.end(), x) != list.end()) return;
    list.push_back(x);
    x->added = true;
    if (x->schedule_fn && libsystem::is_valid_callback(x->schedule_fn)) rt().call_imp(x->schedule_fn, {x->info, g_runloop, g_mode_default});
    runloop::wake();
}
void w_CFRunLoopRemoveSource(Addr, Addr s, Addr) {
    auto* x = source_of(s);
    if (!x) return;
    auto& list = runloop::cf::added_sources();
    list.erase(std::remove(list.begin(), list.end(), x), list.end());
    x->added = false;
}
uint64_t w_CFRunLoopContainsSource(Addr, Addr s, Addr) {
    auto* x = source_of(s);
    auto& list = runloop::cf::added_sources();
    return x && std::find(list.begin(), list.end(), x) != list.end() ? 1 : 0;
}

void w_CFRunLoopTimerCreate(GuestRegs* r) {      // (allocator, fireDate, interval, flags, order, callout, context)
    RegsReader a(*r);
    a.next_int();
    const double fire = a.next_double(), interval = a.next_double();
    a.next_int();
    a.next_int();
    const Addr callout = a.next_int();
    const uint64_t ctx = a.next_int();
    if (!libsystem::is_valid_callback(callout)) { trap("CFRunLoopTimerCreate: the callout is not executable guest code"); return; }
    TimerRec t;
    t.callout = callout;
    t.interval = interval;
    t.fire_abs = fire;
    if (ctx && rt().readable(ctx, 40)) t.info = reinterpret_cast<const uint64_t*>(ctx)[1];
    Addr obj = rt().create_instance(g_cls_timer, 0);
    if (!obj) return;
    g_timers[obj] = t;
    r->ret_rax = obj;
}
void w_CFRunLoopAddTimer(Addr, Addr timer, Addr) {
    auto it = g_timers.find(timer);
    if (it == g_timers.end() || !it->second.valid) { trap("CFRunLoopAddTimer: not a valid timer"); return; }
    TimerRec& t = it->second;
    if (t.host_id) return;
    const int64_t delay_ms = static_cast<int64_t>(std::max(0.0, (t.fire_abs - now_abs()) * 1000.0));
    const int64_t interval_ms = t.interval > 0 ? std::max<int64_t>(1, static_cast<int64_t>(t.interval * 1000.0)) : 0;
    rt().retain(timer);                                                      // the run loop keeps the timer alive while it is scheduled
    t.host_id = runloop::add_host_timer(delay_ms, interval_ms, [timer] {
        auto f = g_timers.find(timer);
        if (f == g_timers.end() || !f->second.valid) return;
        const Addr callout = f->second.callout, info = f->second.info;
        const bool once = f->second.interval <= 0;
        if (once) { f->second.valid = false; f->second.host_id = 0; }
        rt().call_imp(callout, {timer, info});
        if (once) rt().release(timer);
    });
}
void w_CFRunLoopTimerInvalidate(Addr timer) {
    auto it = g_timers.find(timer);
    if (it == g_timers.end() || !it->second.valid) return;
    it->second.valid = false;
    if (it->second.host_id) { runloop::cancel_timer(it->second.host_id); it->second.host_id = 0; rt().release(timer); }
}
void w_CFRunLoopRemoveTimer(Addr, Addr timer, Addr) { w_CFRunLoopTimerInvalidate(timer); }
uint64_t w_CFRunLoopTimerIsValid(Addr timer) { auto it = g_timers.find(timer); return it != g_timers.end() && it->second.valid ? 1 : 0; }

uint64_t m_dealloc(Addr self, Addr) {
    if (auto it = g_sources.find(self); it != g_sources.end()) {
        runloop::cf::Source& s = *it->second;
        auto& list = runloop::cf::added_sources();
        list.erase(std::remove(list.begin(), list.end(), &s), list.end());
        if (s.release_fn && libsystem::is_valid_callback(s.release_fn)) rt().call_imp(s.release_fn, {s.info});
        g_sources.erase(it);
    }
    if (auto it = g_timers.find(self); it != g_timers.end()) {
        if (it->second.host_id) runloop::cancel_timer(it->second.host_id);
        g_timers.erase(it);
    }
    rt().destruct_instance(self);
    rt().dispose_instance(self);
    return 0;
}

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace
}  // namespace orvyn::corefoundation

namespace orvyn::runloop::cf {
void perform_source(Source& s) {
    if (s.perform_fn && libsystem::is_valid_callback(s.perform_fn)) corefoundation::rt_call(s.perform_fn, s.info);
}
}  // namespace orvyn::runloop::cf

namespace orvyn::corefoundation {

void rt_call(uint64_t fn, uint64_t arg) { foundation::rt().call_imp(fn, {arg}); }

void add_cf_symbols(std::vector<libsystem::HostSymbol>& t) {
    using libsystem::HostSymbol;
    using libsystem::Lib;
    auto fn = [&](const char* name, void* f, bool raw = false) { t.push_back(HostSymbol{name, f, raw, false, false, Lib::CoreFoundation}); };
    fn("_CFRetain", addr(w_CFRetain));
    fn("_CFRelease", addr(w_CFRelease));
    fn("_CFGetRetainCount", addr(w_CFGetRetainCount));
    fn("_CFGetTypeID", addr(w_CFGetTypeID));
    fn("_CFStringGetTypeID", addr(w_CFStringGetTypeID));
    fn("_CFArrayGetTypeID", addr(w_CFArrayGetTypeID));
    fn("_CFDictionaryGetTypeID", addr(w_CFDictionaryGetTypeID));
    fn("_CFNumberGetTypeID", addr(w_CFNumberGetTypeID));
    fn("_CFRunLoopGetTypeID", addr(w_CFRunLoopGetTypeID));
    fn("_CFEqual", addr(w_CFEqual));
    fn("_CFHash", addr(w_CFHash));
    fn("_CFCopyDescription", addr(w_CFCopyDescription));
    fn("_CFShow", addr(w_CFShow));
    fn("_CFStringCreateWithCString", addr(w_CFStringCreateWithCString));
    fn("_CFStringCreateWithFormat", addr(w_CFStringCreateWithFormat), true);
    fn("_CFStringGetLength", addr(w_CFStringGetLength));
    fn("_CFStringGetCharacterAtIndex", addr(w_CFStringGetCharacterAtIndex));
    fn("_CFStringGetCString", addr(w_CFStringGetCString));
    fn("_CFStringGetCStringPtr", addr(w_CFStringGetCStringPtr));
    fn("_CFStringCompare", addr(w_CFStringCompare));
    fn("_CFStringCreateCopy", addr(w_CFStringCreateCopy));
    fn("_CFStringCreateMutable", addr(w_CFStringCreateMutable));
    fn("_CFStringAppend", addr(w_CFStringAppend));
    fn("_CFStringAppendCString", addr(w_CFStringAppendCString));
    fn("_CFArrayCreate", addr(w_CFArrayCreate));
    fn("_CFArrayCreateMutable", addr(w_CFArrayCreateMutable));
    fn("_CFArrayGetCount", addr(w_CFArrayGetCount));
    fn("_CFArrayGetValueAtIndex", addr(w_CFArrayGetValueAtIndex));
    fn("_CFArrayAppendValue", addr(w_CFArrayAppendValue));
    fn("_CFArrayRemoveValueAtIndex", addr(w_CFArrayRemoveValueAtIndex));
    fn("_CFDictionaryCreate", addr(w_CFDictionaryCreate));
    fn("_CFDictionaryCreateMutable", addr(w_CFDictionaryCreateMutable));
    fn("_CFDictionaryGetCount", addr(w_CFDictionaryGetCount));
    fn("_CFDictionaryGetValue", addr(w_CFDictionaryGetValue));
    fn("_CFDictionaryContainsKey", addr(w_CFDictionaryContainsKey));
    fn("_CFDictionarySetValue", addr(w_CFDictionarySetValue));
    fn("_CFDictionaryAddValue", addr(w_CFDictionaryAddValue));
    fn("_CFDictionaryRemoveValue", addr(w_CFDictionaryRemoveValue));
    fn("_CFDictionaryGetKeysAndValues", addr(w_CFDictionaryGetKeysAndValues));
    fn("_CFNumberCreate", addr(w_CFNumberCreate));
    fn("_CFNumberGetValue", addr(w_CFNumberGetValue));
    fn("_CFBooleanGetValue", addr(w_CFBooleanGetValue));
    fn("_CFAbsoluteTimeGetCurrent", addr(w_CFAbsoluteTimeGetCurrent), true);
    fn("_CFRunLoopGetMain", addr(w_CFRunLoopGetMain));
    fn("_CFRunLoopGetCurrent", addr(w_CFRunLoopGetCurrent));
    fn("_CFRunLoopRun", addr(w_CFRunLoopRun));
    fn("_CFRunLoopRunInMode", addr(w_CFRunLoopRunInMode), true);
    fn("_CFRunLoopStop", addr(w_CFRunLoopStop));
    fn("_CFRunLoopWakeUp", addr(w_CFRunLoopWakeUp));
    fn("_CFRunLoopIsWaiting", addr(w_CFRunLoopIsWaiting));
    fn("_CFRunLoopSourceCreate", addr(w_CFRunLoopSourceCreate));
    fn("_CFRunLoopSourceSignal", addr(w_CFRunLoopSourceSignal));
    fn("_CFRunLoopSourceIsValid", addr(w_CFRunLoopSourceIsValid));
    fn("_CFRunLoopSourceInvalidate", addr(w_CFRunLoopSourceInvalidate));
    fn("_CFRunLoopAddSource", addr(w_CFRunLoopAddSource));
    fn("_CFRunLoopRemoveSource", addr(w_CFRunLoopRemoveSource));
    fn("_CFRunLoopContainsSource", addr(w_CFRunLoopContainsSource));
    fn("_CFRunLoopTimerCreate", addr(w_CFRunLoopTimerCreate), true);
    fn("_CFRunLoopAddTimer", addr(w_CFRunLoopAddTimer));
    fn("_CFRunLoopRemoveTimer", addr(w_CFRunLoopRemoveTimer));
    fn("_CFRunLoopTimerInvalidate", addr(w_CFRunLoopTimerInvalidate));
    fn("_CFRunLoopTimerIsValid", addr(w_CFRunLoopTimerIsValid));
}

void install(objc::Runtime& rt) {
    rt.set_native_owner("CoreFoundation");
    g_cls_source = rt.declare_native_class("__NSCFRunLoopSource", "NSObject", 16);
    g_cls_timer = rt.declare_native_class("__NSCFRunLoopTimer", "NSObject", 16);
    objc::Class* loop = rt.declare_native_class("__NSCFRunLoop", "NSObject", 16);
    if (!g_cls_source || !g_cls_timer || !loop) return;
    rt.add_native_methods(g_cls_source, false, {{"dealloc", "v16@0:8", addr(m_dealloc), false}});
    rt.add_native_methods(g_cls_timer, false, {{"dealloc", "v16@0:8", addr(m_dealloc), false}});
    g_runloop = rt.arena_alloc(16, 16);                                       // the one run loop: immortal
    reinterpret_cast<Addr*>(g_runloop)[0] = loop->addr;
    reinterpret_cast<Addr*>(g_runloop)[1] = 0;
    g_sources.clear();
    g_timers.clear();
    // constant structures and variables
    g_array_callbacks = rt.arena_alloc(40, 16);
    g_dict_key_callbacks = rt.arena_alloc(56, 16);
    g_dict_value_callbacks = rt.arena_alloc(48, 16);
    g_mode_default = foundation::immortal_string("kCFRunLoopDefaultMode");
    g_mode_common = foundation::immortal_string("kCFRunLoopCommonModes");
    g_allocator_default = rt.arena_alloc(16, 16);
    g_allocator_null = rt.arena_alloc(16, 16);
    auto var = [&](Addr value) { const Addr v = rt.arena_alloc(8, 8); *reinterpret_cast<Addr*>(v) = value; return v; };
    auto table = std::make_shared<std::unordered_map<std::string, Addr>>();
    (*table)["_kCFTypeArrayCallBacks"] = g_array_callbacks;
    (*table)["_kCFTypeDictionaryKeyCallBacks"] = g_dict_key_callbacks;
    (*table)["_kCFTypeDictionaryValueCallBacks"] = g_dict_value_callbacks;
    (*table)["_kCFAllocatorDefault"] = var(0);                                 // kCFAllocatorDefault is a variable holding NULL: the allocator is ignored
    (*table)["_kCFAllocatorNull"] = var(g_allocator_null);
    (*table)["_kCFRunLoopDefaultMode"] = var(g_mode_default);
    (*table)["_kCFRunLoopCommonModes"] = var(g_mode_common);
    (*table)["_NSDefaultRunLoopMode"] = var(g_mode_default);
    (*table)["_NSRunLoopCommonModes"] = var(g_mode_common);
    (*table)["_NSEventTrackingRunLoopMode"] = var(foundation::immortal_string("NSEventTrackingRunLoopMode"));
    (*table)["_NSModalPanelRunLoopMode"] = var(foundation::immortal_string("NSModalPanelRunLoopMode"));
    rt.add_dynamic_provider([table](std::string_view name) -> std::optional<uint64_t> {
        auto it = table->find(std::string(name));
        return it == table->end() ? std::nullopt : std::optional<uint64_t>(it->second);
    });
}

}  // namespace orvyn::corefoundation
