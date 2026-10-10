// Foundation subset: object plumbing, NSLog and the free functions, NSAutoreleasePool, and the registration entry points.
#include <cstring>
#include <format>
#include <unordered_map>

#include "cider/appkit.hpp"
#include "cider/dispatch.hpp"
#include "../graphics/cg_bridge.hpp"
#include "../corefoundation/cf_bridge.hpp"
#include "cider/runloop.hpp"
#include "cider/libsystem.hpp"
#include "cider/log.hpp"
#include "cider/objc.hpp"
#include "cider/printf_format.hpp"
#include "../libsystem/internal.hpp"
#include "foundation_internal.hpp"

namespace cider::foundation {
namespace {
Runtime* g_rt = nullptr;
Classes g_classes;
std::vector<std::unique_ptr<Payload>> g_slots;
std::vector<uint64_t> g_free;
}  // namespace

void g_rt_set(Runtime* rt) { g_rt = rt; g_classes = Classes{}; g_slots.clear(); g_free.clear(); }
Classes& classes() { return g_classes; }
Runtime& rt() { return *g_rt; }

// ---- payload store ------------------------------------------------------------------------------------------------------------
uint64_t store_add(std::unique_ptr<Payload> p) {
    if (!g_free.empty()) {
        const uint64_t id = g_free.back();
        g_free.pop_back();
        g_slots[id - 1] = std::move(p);
        return id;
    }
    g_slots.push_back(std::move(p));
    return g_slots.size();
}
Payload* store_get(uint64_t id) { return id == 0 || id > g_slots.size() ? nullptr : g_slots[id - 1].get(); }
void store_drop(uint64_t id) {
    if (id == 0 || id > g_slots.size() || !g_slots[id - 1]) return;
    g_slots[id - 1].reset();
    g_free.push_back(id);
}
size_t store_size() { return g_slots.size() - g_free.size(); }

// ---- objects ------------------------------------------------------------------------------------------------------------------
Addr make_object(Class* cls, std::unique_ptr<Payload> payload) {
    const Addr obj = rt().create_instance(cls, 0);
    if (!obj) return 0;
    uint64_t* slot = payload_slot(obj);
    if (slot && payload) *slot = store_add(std::move(payload));
    return obj;
}
Addr autoreleased(Addr obj) { return obj ? rt().autorelease(obj) : 0; }

uint64_t* payload_slot(Addr obj) {
    Class* c = rt().class_of_object(obj);
    if (!c || c->is_meta || c->instance_size < 16 || !rt().readable(obj + 8, 8)) return nullptr;
    // only classes derived from the payload classes keep the payload id at +8
    const Classes& k = g_classes;
    if (k.constant_string && rt().is_kind_of(c, k.constant_string)) return nullptr;      // compiler-emitted literal: no payload
    bool known = rt().is_kind_of(c, k.string) || rt().is_kind_of(c, k.number) || rt().is_kind_of(c, k.array) || rt().is_kind_of(c, k.dict) || rt().is_kind_of(c, k.pool);
    for (Class* x : k.extra_payload) known = known || rt().is_kind_of(c, x);
    if (!known) return nullptr;
    return reinterpret_cast<uint64_t*>(obj + 8);
}
Payload* payload_of(Addr obj) {
    if (!obj) return nullptr;
    uint64_t* s = payload_slot(obj);
    return s ? store_get(*s) : nullptr;
}
bool is_kind(Addr obj, Class* cls) {
    Class* c = rt().class_of_object(obj);
    return c && cls && rt().is_kind_of(c, cls);
}

void raise(const std::string& name, const std::string& reason) {
    libsystem::detail::trap("*** Terminating app due to uncaught exception '" + name + "', reason: '" + reason + "'");
}

// ---- strings (core helpers; the class itself is in nsstring.cpp) --------------------------------------------------------------
std::u16string to_u16(std::string_view s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        i += n;
        if (cp >= 0x10000) { cp -= 0x10000; out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10))); out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF))); }
        else out.push_back(static_cast<char16_t>(cp));
    }
    return out;
}
std::string from_u16(const std::u16string& u) {
    std::string out;
    for (size_t i = 0; i < u.size(); ++i) {
        uint32_t cp = u[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < u.size() && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00); ++i; }
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else { out.push_back(static_cast<char>(0xF0 | (cp >> 18))); out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    }
    return out;
}
bool valid_utf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + n > s.size()) return false;
        uint32_t cp = n == 1 ? c : c & (0xff >> (n + 1));
        for (int k = 1; k < n; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += n;
    }
    return true;
}
size_t utf16_length(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) == 0x80) continue;                    // continuation byte
        n += (c >= 0xF0) ? 2 : 1;                            // 4-byte sequences are surrogate pairs
    }
    return n;
}

bool string_view_of(Addr obj, std::string_view& out) {
    if (!obj) return false;
    Class* c = rt().class_of_object(obj);
    if (!c || !g_classes.string || !rt().is_kind_of(c, g_classes.string)) return false;
    if (rt().is_kind_of(c, g_classes.constant_string)) {
        // struct __NSConstantString { Class isa; int flags; const char* str; long length; } emitted by the compiler (@"...")
        if (!rt().readable(obj, 32)) return false;
        const char* str = *reinterpret_cast<const char* const*>(obj + 16);
        const int64_t len = *reinterpret_cast<const int64_t*>(obj + 24);
        if (*reinterpret_cast<const uint32_t*>(obj + 8) == 0x07D0) {              // non-ASCII literal: UTF-16LE, `len` code units
            if (len < 0 || len > (32 << 20) || !rt().readable(reinterpret_cast<Addr>(str), static_cast<size_t>(len) * 2)) return false;
            static std::unordered_map<Addr, std::string> cache;                    // literals are immortal: convert once
            auto it = cache.find(obj);
            if (it == cache.end()) it = cache.emplace(obj, from_u16(std::u16string(reinterpret_cast<const char16_t*>(str), static_cast<size_t>(len)))).first;
            out = it->second;
            return true;
        }
        if (len < 0 || len > (64 << 20) || !rt().readable(reinterpret_cast<Addr>(str), static_cast<size_t>(len) + 1)) return false;
        out = std::string_view(str, static_cast<size_t>(len));
        return true;
    }
    auto* sp = dynamic_cast<StringPayload*>(payload_of(obj));
    static const std::string empty;
    out = sp ? std::string_view(sp->utf8) : std::string_view(empty);
    return true;
}
Addr new_string(std::string utf8) {
    auto p = std::make_unique<StringPayload>();
    p->utf8 = std::move(utf8);
    return make_object(g_classes.string, std::move(p));
}
Addr new_string_autoreleased(std::string utf8) { return autoreleased(new_string(std::move(utf8))); }

std::string describe(Addr obj) {
    if (!obj) return "(null)";
    const Addr d = rt().send(obj, "description");
    std::string_view sv;
    if (d && string_view_of(d, sv)) return std::string(sv);
    return "<unprintable object>";
}

bool format_with_objects(const std::string& format, abi::ArgReader& args, std::string& out) {
    libsystem::ObjectFormatter objects = [](uint64_t obj, std::string& text) { text = describe(obj); return true; };
    auto st = libsystem::format_printf(format.c_str(), args, out, 16u << 20, 8u << 20, &objects);
    if (!st.ok) { libsystem::detail::trap("format: " + st.error); return false; }
    return true;
}
Addr immortal_string(const std::string& utf8) {
    const Addr obj = rt().arena_alloc(16, 16);
    if (!obj) return 0;
    auto p = std::make_unique<StringPayload>();
    p->utf8 = utf8;
    *reinterpret_cast<Addr*>(obj) = g_classes.string->addr;
    *reinterpret_cast<uint64_t*>(obj + 8) = store_add(std::move(p));
    return obj;
}

// ---- Foundation free functions -------------------------------------------------------------------------------------------------------
namespace {
using abi::GuestRegs;
using abi::RegsReader;

// void NSLog(NSString* format, ...): formats with %@ support and writes "message\n" to stderr. Apple's NSLog also prefixes a timestamp,
// the process name, pid and thread id; CIDER writes the bare message (documented deviation).
void w_NSLog(GuestRegs* r) {
    RegsReader a(*r);
    const Addr fmt = a.next_int();
    std::string_view f;
    if (!string_view_of(fmt, f)) { raise("NSInvalidArgumentException", "NSLog: the format is not an NSString"); return; }
    std::string format(f), out;
    libsystem::ObjectFormatter objects = [](uint64_t obj, std::string& text) { text = describe(obj); return true; };
    auto st = libsystem::format_printf(format.c_str(), a, out, 16u << 20, 8u << 20, &objects);
    if (!st.ok) { libsystem::detail::trap("NSLog: " + st.error); return; }
    out.push_back('\n');
    libsystem::console_write(2, out.data(), out.size());
}
Addr w_NSStringFromClass(Addr cls) {
    Class* c = rt().class_at(cls);
    return c ? new_string_autoreleased(c->is_meta ? c->instance_class->name : c->name) : 0;
}
Addr w_NSClassFromString(Addr s) {
    std::string_view sv;
    if (!string_view_of(s, sv)) return 0;
    Class* c = rt().class_named(sv);
    return c ? c->addr : 0;
}
Addr w_NSStringFromSelector(Addr sel) {
    const auto n = rt().sel_name(sel);
    return n.empty() ? 0 : new_string_autoreleased(std::string(n));
}
Addr w_NSSelectorFromString(Addr s) {
    std::string_view sv;
    return string_view_of(s, sv) ? rt().sel(sv) : 0;
}
template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

// ---- NSAutoreleasePool ------------------------------------------------------------------------------------------------------------------
Addr pool_init(Addr self, Addr) {
    uint64_t* slot = payload_slot(self);
    if (!slot) return self;
    auto p = std::make_unique<PoolPayload>();
    p->token = rt().pool_push();
    *slot = store_add(std::move(p));
    return self;
}
void pool_drain(Addr self, Addr) {
    uint64_t* slot = payload_slot(self);
    auto* p = slot ? dynamic_cast<PoolPayload*>(store_get(*slot)) : nullptr;
    if (p) {
        const Addr token = p->token;
        store_drop(*slot);
        *slot = 0;
        rt().pool_pop(token);
    }
    rt().release(self);
}
}  // namespace

void install_misc(Runtime& r) {
    Class* pool = r.declare_native_class("NSAutoreleasePool", "NSObject", 16);
    g_classes.pool = pool;
    r.add_native_methods(pool, false, {{"init", "@16@0:8", addr(pool_init), false}, {"drain", "v16@0:8", addr(pool_drain), false}, {"release", "Vv16@0:8", addr(pool_drain), false}});
}

}  // namespace cider::foundation

namespace cider::objc {
void add_foundation_symbols(std::vector<libsystem::HostSymbol>& t) {
    using libsystem::HostSymbol;
    using libsystem::Lib;
    auto fn = [&](const char* name, void* f, bool raw = false) { t.push_back(HostSymbol{name, f, raw, false, false, Lib::Foundation}); };
    fn("_NSLog", reinterpret_cast<void*>(&foundation::w_NSLog), true);
    fn("_NSStringFromClass", reinterpret_cast<void*>(&foundation::w_NSStringFromClass));
    fn("_NSClassFromString", reinterpret_cast<void*>(&foundation::w_NSClassFromString));
    fn("_NSStringFromSelector", reinterpret_cast<void*>(&foundation::w_NSStringFromSelector));
    fn("_NSSelectorFromString", reinterpret_cast<void*>(&foundation::w_NSSelectorFromString));
}

void install_foundation(Runtime& rt) {
    foundation::g_rt_set(&rt);
    // `@[]` and `@{}` compile to loads of the global pointers ___NSArray0__ / ___NSDictionary0__: immortal empty collections in the arena
    auto made_ptr = std::make_shared<std::unordered_map<std::string, uint64_t>>();
    rt.add_dynamic_provider([&rt, made_ptr](std::string_view name) -> std::optional<uint64_t> {
        // block literals bind their isa to these symbols: the address of the matching native class is the isa value
        if (name == "__NSConcreteStackBlock" || name == "__NSConcreteGlobalBlock" || name == "__NSConcreteMallocBlock") {
            foundation::Class* c = rt.class_named(name == "__NSConcreteStackBlock" ? "__NSStackBlock__" : name == "__NSConcreteGlobalBlock" ? "__NSGlobalBlock__" : "__NSMallocBlock__");
            return c ? std::optional<uint64_t>(c->addr) : std::nullopt;
        }
        if (auto d = dispatch::data_symbol(name)) return d;
        const bool arr = name == "___NSArray0__", dic = name == "___NSDictionary0__";
        if (!arr && !dic) return std::nullopt;
        auto& made = *made_ptr;
        if (auto it = made.find(std::string(name)); it != made.end()) return it->second;
        foundation::Class* cls = arr ? foundation::classes().array : foundation::classes().dict;
        if (!cls) return std::nullopt;
        const auto obj = rt.arena_alloc(16, 16), var = rt.arena_alloc(8, 8);
        std::unique_ptr<foundation::Payload> p = arr ? std::unique_ptr<foundation::Payload>(new foundation::ArrayPayload) : std::unique_ptr<foundation::Payload>(new foundation::DictPayload);
        *reinterpret_cast<uint64_t*>(obj) = cls->addr;
        *reinterpret_cast<uint64_t*>(obj + 8) = foundation::store_add(std::move(p));
        *reinterpret_cast<uint64_t*>(var) = obj;
        made[std::string(name)] = var;
        return var;
    });
    rt.set_native_owner("libobjc");               // Apple exports NSObject from libobjc
    foundation::install_nsobject(rt);
    libsystem::detail::install_block_classes(rt);
    dispatch::install(rt);
    rt.set_native_owner("Foundation");
    foundation::install_nsstring(rt);
    foundation::install_nsnumber(rt);
    foundation::install_nsarray(rt);
    foundation::install_nsdictionary(rt);
    foundation::install_misc(rt);
    foundation::install_nsextras(rt);
    foundation::install_nsnotification(rt);
    graphics::install_cg(rt);
    corefoundation::install(rt);
    runloop::install(rt);
    appkit::install(rt);
}
}  // namespace cider::objc
