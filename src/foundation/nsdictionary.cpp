// NSDictionary / NSMutableDictionary. Keys are copied (-copy) and retained, values retained. Lookup is a hash-filtered linear scan over an
// insertion-ordered vector (O(n), documented). Not implemented: enumeration blocks, key sets, sorting, writeToFile, KVC.
#include <algorithm>
#include <cstring>
#include <format>

#include "cider/libsystem.hpp"
#include "cider/objc.hpp"
#include "../libsystem/internal.hpp"
#include "foundation_internal.hpp"

namespace cider::foundation {
namespace {

using abi::GuestRegs;
using Entry = DictPayload::Entry;

DictPayload* dic(Addr obj) {
    uint64_t* slot = payload_slot(obj);
    if (!slot || !rt().is_kind_of(rt().class_of_object(obj), classes().dict)) return nullptr;
    if (!*slot) *slot = store_add(std::make_unique<DictPayload>());
    return dynamic_cast<DictPayload*>(store_get(*slot));
}

bool equal_objects(Addr a, Addr b) { return a == b || (a && b && rt().send(a, "isEqual:", {b}) != 0); }

// index of `key` in `p`, or -1. The guest -hash / -isEqual: may run; callers re-fetch the payload afterwards if they continue to use it.
ptrdiff_t find_entry(Addr self, Addr key, uint64_t* hash_out = nullptr) {
    const uint64_t h = rt().send(key, "hash");
    if (hash_out) *hash_out = h;
    DictPayload* p = dic(self);
    if (!p) return -1;
    for (size_t i = 0; i < p->entries.size(); ++i) {
        if (p->entries[i].hash != h) continue;
        if (equal_objects(p->entries[i].key, key)) return static_cast<ptrdiff_t>(i);
        p = dic(self);
        if (!p) return -1;
    }
    return -1;
}

bool responds(Addr obj, const char* sel) {
    Class* c = rt().class_of_object(obj);
    return c && rt().find_imp(c, rt().sel(sel)) != 0;
}

// Inserts or replaces. `key` is copied when new. Returns false after raising.
bool put(Addr self, Addr key, Addr value, const char* who) {
    if (!key) { raise("NSInvalidArgumentException", std::format("*** {}: key cannot be nil", who)); return false; }
    if (!value) { raise("NSInvalidArgumentException", std::format("*** {}: object cannot be nil (key: {})", who, describe(key))); return false; }
    uint64_t h = 0;
    const ptrdiff_t at = find_entry(self, key, &h);
    DictPayload* p = dic(self);
    if (!p) return false;
    rt().retain(value);
    if (at >= 0) {
        const Addr old = p->entries[static_cast<size_t>(at)].value;
        p->entries[static_cast<size_t>(at)].value = value;
        rt().release(old);
        return true;
    }
    Addr stored = key;
    if (responds(key, "copyWithZone:")) {
        stored = rt().send(key, "copy");
        if (!stored) { rt().release(value); return false; }
    } else {
        raise("NSInvalidArgumentException", std::format("*** {}: key of class {} does not respond to copyWithZone:", who, rt().class_of_object(key) ? rt().class_of_object(key)->name : "?"));
        rt().release(value);
        return false;
    }
    p = dic(self);
    if (!p) return false;
    p->entries.push_back(Entry{h, stored, value});
    return true;
}

void release_entries(const std::vector<Entry>& es) {
    for (const Entry& e : es) { if (e.key) rt().release(e.key); if (e.value) rt().release(e.value); }
}

Addr make_dict(Addr cls_self, bool want_mutable) {
    Class* c = rt().class_at(cls_self);
    if (!c || c->is_meta || !rt().is_kind_of(c, classes().dict)) c = want_mutable ? classes().mdict : classes().dict;
    return rt().create_instance(c, 0);
}

// ---- class methods ---------------------------------------------------------------------------------------------------------------------
Addr c_dictionary(Addr self, Addr) {
    const Addr d = make_dict(self, false);
    if (d && !dic(d)) { rt().release(d); return 0; }
    return autoreleased(d);
}
Addr c_dictionaryWithObjectForKey(Addr self, Addr, Addr obj, Addr key) {
    const Addr d = make_dict(self, false);
    if (!d) return 0;
    if (!put(d, key, obj, "+[NSDictionary dictionaryWithObject:forKey:]")) { rt().release(d); return 0; }
    return autoreleased(d);
}
void c_dictionaryWithObjectsForKeysCount(GuestRegs* r) {
    const Addr self = r->gp[0], objs = r->gp[2], keys = r->gp[3];
    const uint64_t n = r->gp[4];
    if (n > (1u << 24) || (n && (!rt().readable(objs, n * 8) || !rt().readable(keys, n * 8)))) { raise("NSInvalidArgumentException", "+[NSDictionary dictionaryWithObjects:forKeys:count:]: invalid pointers or count"); return; }
    const Addr d = make_dict(self, false);
    if (!d) return;
    for (uint64_t i = 0; i < n; ++i)
        if (!put(d, reinterpret_cast<const Addr*>(keys)[i], reinterpret_cast<const Addr*>(objs)[i], "+[NSDictionary dictionaryWithObjects:forKeys:count:]")) { rt().release(d); return; }
    r->ret_rax = autoreleased(d);
}
Addr c_dictionaryWithDictionary(Addr self, Addr, Addr other) {
    auto* src = dic(other);
    if (!src) { raise("NSInvalidArgumentException", "+[NSDictionary dictionaryWithDictionary:]: argument is not an NSDictionary"); return 0; }
    const std::vector<Entry> es = src->entries;
    const Addr d = make_dict(self, false);
    if (!d) return 0;
    for (const Entry& e : es) if (!put(d, e.key, e.value, "+[NSDictionary dictionaryWithDictionary:]")) { rt().release(d); return 0; }
    return autoreleased(d);
}
Addr c_dictionaryWithCapacity(Addr self, Addr, uint64_t) { return c_dictionary(self, 0); }

// ---- initialisers -----------------------------------------------------------------------------------------------------------------------
Addr i_init(Addr self, Addr) { return dic(self) ? self : (raise("NSInvalidArgumentException", "-[NSDictionary init]: receiver is not an NSDictionary"), Addr{0}); }
Addr i_initWithCapacity(Addr self, Addr, uint64_t) { return i_init(self, 0); }
void i_initWithObjectsForKeysCount(GuestRegs* r) {
    const Addr self = r->gp[0], objs = r->gp[2], keys = r->gp[3];
    const uint64_t n = r->gp[4];
    if (!dic(self)) { raise("NSInvalidArgumentException", "-[NSDictionary initWithObjects:forKeys:count:]: receiver is not an NSDictionary"); return; }
    if (n > (1u << 24) || (n && (!rt().readable(objs, n * 8) || !rt().readable(keys, n * 8)))) { raise("NSInvalidArgumentException", "-[NSDictionary initWithObjects:forKeys:count:]: invalid pointers or count"); return; }
    for (uint64_t i = 0; i < n; ++i)
        if (!put(self, reinterpret_cast<const Addr*>(keys)[i], reinterpret_cast<const Addr*>(objs)[i], "-[NSDictionary initWithObjects:forKeys:count:]")) return;
    r->ret_rax = self;
}
Addr i_dealloc(Addr self, Addr) {
    if (uint64_t* slot = payload_slot(self)) {
        std::vector<Entry> es;
        if (auto* p = dynamic_cast<DictPayload*>(store_get(*slot))) es = std::move(p->entries);
        store_drop(*slot);
        *slot = 0;
        release_entries(es);
    }
    rt().destruct_instance(self);
    rt().dispose_instance(self);
    return 0;
}

// ---- queries ------------------------------------------------------------------------------------------------------------------------------------
uint64_t i_count(Addr self, Addr) { auto* p = dic(self); return p ? p->entries.size() : 0; }
Addr i_objectForKey(Addr self, Addr, Addr key) {
    if (!key) return 0;                                           // Apple raises for nil keys on lookup in newer SDKs; nil result here
    const ptrdiff_t at = find_entry(self, key);
    auto* p = dic(self);
    return at >= 0 && p ? p->entries[static_cast<size_t>(at)].value : 0;
}
Addr i_allKeys(Addr self, Addr) {
    auto* p = dic(self);
    if (!p) return 0;
    std::vector<Addr> v;
    for (const Entry& e : p->entries) v.push_back(e.key);
    return new_array(std::move(v), false, true);
}
Addr i_allValues(Addr self, Addr) {
    auto* p = dic(self);
    if (!p) return 0;
    std::vector<Addr> v;
    for (const Entry& e : p->entries) v.push_back(e.value);
    return new_array(std::move(v), false, true);
}
uint64_t i_isEqualToDictionary(Addr self, Addr, Addr other) {
    auto *a = dic(self), *b = dic(other);
    if (!a || !b || a->entries.size() != b->entries.size()) return 0;
    const std::vector<Entry> es = a->entries;
    for (const Entry& e : es) {
        const Addr v = i_objectForKey(other, 0, e.key);
        if (!v || !equal_objects(e.value, v)) return 0;
    }
    return 1;
}
uint64_t i_isEqual(Addr self, Addr, Addr other) { return self == other || (is_kind(other, classes().dict) && i_isEqualToDictionary(self, 0, other)) ? 1 : 0; }
uint64_t i_hash(Addr self, Addr) { auto* p = dic(self); return p ? p->entries.size() : 0; }

std::string indent(const std::string& s) {
    std::string out;
    for (char c : s) { out += c; if (c == '\n') out += "    "; }
    return out;
}
Addr i_description(Addr self, Addr) {
    auto* p = dic(self);
    if (!p) return new_string_autoreleased("(null)");
    const std::vector<Entry> es = p->entries;
    std::string out = "{\n";
    for (const Entry& e : es) out += "    " + indent(describe(e.key)) + " = " + indent(describe(e.value)) + ";\n";
    return new_string_autoreleased(out + "}");
}

// for (id key in dict): enumerates the keys
void i_countByEnumerating(GuestRegs* r) {
    const Addr self = r->gp[0], state = r->gp[2], buf = r->gp[3];
    const uint64_t len = r->gp[4];
    auto* p = dic(self);
    if (!p || !state || !rt().readable(state, 64) || (len && !rt().readable(buf, len * 8))) { raise("NSInvalidArgumentException", "countByEnumeratingWithState:objects:count: invalid arguments"); return; }
    auto* st = reinterpret_cast<uint64_t*>(state);
    const uint64_t pos = st[0], total = p->entries.size();
    if (pos >= total || len == 0) { r->ret_rax = 0; return; }
    const uint64_t n = std::min<uint64_t>(len, total - pos);
    auto* out = reinterpret_cast<Addr*>(buf);
    for (uint64_t i = 0; i < n; ++i) out[i] = p->entries[pos + i].key;
    st[0] = pos + n;
    st[1] = buf;
    st[2] = state + 24;
    r->ret_rax = n;
}

// -enumerateKeysAndObjectsUsingBlock: void (^)(id key, id obj, BOOL *stop)
Addr i_enumerateKeysAndObjects(Addr self, Addr, Addr block) {
    auto* p = dic(self);
    if (!p) return 0;
    if (!block || !rt().readable(block, 24)) { raise("NSInvalidArgumentException", "enumerateKeysAndObjectsUsingBlock: the block is nil or unreadable"); return 0; }
    const uint64_t fn = *reinterpret_cast<const uint64_t*>(block + 16);
    if (!libsystem::is_valid_callback(fn)) { raise("NSInvalidArgumentException", "block invoke function is not executable guest code"); return 0; }
    const std::vector<Entry> snapshot = p->entries;
    for (const Entry& e : snapshot) { rt().retain(e.key); rt().retain(e.value); }
    uint8_t stop = 0;
    for (size_t i = 0; i < snapshot.size() && !stop && !rt().failed(); ++i)
        rt().call_imp(fn, {block, snapshot[i].key, snapshot[i].value, reinterpret_cast<uint64_t>(&stop)});
    for (const Entry& e : snapshot) { rt().release(e.key); rt().release(e.value); }
    return 0;
}

Addr clone(Addr self, bool want_mutable) {
    auto* src = dic(self);
    if (!src) return 0;
    const std::vector<Entry> es = src->entries;
    const Addr d = rt().create_instance(want_mutable ? classes().mdict : classes().dict, 0);
    if (!d || !dic(d)) return 0;
    for (const Entry& e : es) if (!put(d, e.key, e.value, "-[NSDictionary copy]")) { rt().release(d); return 0; }
    return d;                                                   // +1: copy / mutableCopy return owned objects
}
Addr i_copyWithZone(Addr self, Addr, Addr) { return is_kind(self, classes().mdict) ? clone(self, false) : rt().retain(self); }
Addr i_mutableCopyWithZone(Addr self, Addr, Addr) { return clone(self, true); }

// ---- NSMutableDictionary ------------------------------------------------------------------------------------------------------------------
Addr i_setObjectForKey(Addr self, Addr, Addr obj, Addr key) { put(self, key, obj, "-[NSMutableDictionary setObject:forKey:]"); return 0; }
Addr i_removeObjectForKey(Addr self, Addr, Addr key) {
    if (!key) { raise("NSInvalidArgumentException", "*** -[NSMutableDictionary removeObjectForKey:]: key cannot be nil"); return 0; }
    const ptrdiff_t at = find_entry(self, key);
    auto* p = dic(self);
    if (at < 0 || !p) return 0;
    const Entry e = p->entries[static_cast<size_t>(at)];
    p->entries.erase(p->entries.begin() + at);
    release_entries({e});
    return 0;
}
Addr i_setObjectForKeyedSubscript(Addr self, Addr cmd, Addr obj, Addr key) {
    if (!obj) return i_removeObjectForKey(self, cmd, key);       // dict[key] = nil removes, like Apple
    return i_setObjectForKey(self, cmd, obj, key);
}
Addr i_removeAllObjects(Addr self, Addr) {
    auto* p = dic(self);
    if (!p) return 0;
    std::vector<Entry> old = std::move(p->entries);
    p->entries.clear();
    release_entries(old);
    return 0;
}

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

DictPayload* dict_of(Addr obj) { return dic(obj); }

void install_nsdictionary(Runtime& r) {
    Class* d = r.declare_native_class("NSDictionary", "NSObject", 16);
    classes().dict = d;
    Class* m = r.declare_native_class("NSMutableDictionary", "NSDictionary", 16);
    classes().mdict = m;
    r.add_native_methods(d, true, {
        {"dictionary", "@16@0:8", addr(c_dictionary), false},
        {"dictionaryWithObject:forKey:", "@32@0:8@16@24", addr(c_dictionaryWithObjectForKey), false},
        {"dictionaryWithObjects:forKeys:count:", "@40@0:8r^@16r^@24Q32", addr(c_dictionaryWithObjectsForKeysCount), true},
        {"dictionaryWithDictionary:", "@24@0:8@16", addr(c_dictionaryWithDictionary), false},
        {"dictionaryWithCapacity:", "@24@0:8Q16", addr(c_dictionaryWithCapacity), false},
    });
    r.add_native_methods(d, false, {
        {"init", "@16@0:8", addr(i_init), false},
        {"initWithCapacity:", "@24@0:8Q16", addr(i_initWithCapacity), false},
        {"initWithObjects:forKeys:count:", "@40@0:8r^@16r^@24Q32", addr(i_initWithObjectsForKeysCount), true},
        {"dealloc", "v16@0:8", addr(i_dealloc), false},
        {"count", "Q16@0:8", addr(i_count), false},
        {"objectForKey:", "@24@0:8@16", addr(i_objectForKey), false},
        {"objectForKeyedSubscript:", "@24@0:8@16", addr(i_objectForKey), false},
        {"allKeys", "@16@0:8", addr(i_allKeys), false},
        {"allValues", "@16@0:8", addr(i_allValues), false},
        {"isEqualToDictionary:", "B24@0:8@16", addr(i_isEqualToDictionary), false},
        {"isEqual:", "B24@0:8@16", addr(i_isEqual), false},
        {"hash", "Q16@0:8", addr(i_hash), false},
        {"description", "@16@0:8", addr(i_description), false},
        {"countByEnumeratingWithState:objects:count:", "Q40@0:8^{_NSFastEnumerationState=Q^@^Q[5Q]}16^@24Q32", addr(i_countByEnumerating), true},
        {"copyWithZone:", "@24@0:8^{_NSZone=}16", addr(i_copyWithZone), false},
        {"mutableCopyWithZone:", "@24@0:8^{_NSZone=}16", addr(i_mutableCopyWithZone), false},
        {"enumerateKeysAndObjectsUsingBlock:", "v24@0:8@?16", addr(i_enumerateKeysAndObjects), false},
    });
    r.add_native_methods(m, false, {
        {"setObject:forKey:", "v32@0:8@16@24", addr(i_setObjectForKey), false},
        {"setObject:forKeyedSubscript:", "v32@0:8@16@24", addr(i_setObjectForKeyedSubscript), false},
        {"removeObjectForKey:", "v24@0:8@16", addr(i_removeObjectForKey), false},
        {"removeAllObjects", "v16@0:8", addr(i_removeAllObjects), false},
    });
}

}  // namespace cider::foundation
