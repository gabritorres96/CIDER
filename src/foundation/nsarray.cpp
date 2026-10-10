// NSArray / NSMutableArray. Elements are retained objects; the vector lives in a host payload, the object is [isa][payload id].
// Not implemented: sorting, enumeration blocks, KVC, predicates, subarrays, mutation detection during fast enumeration, binary search.
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
using abi::RegsReader;
constexpr uint64_t kNotFound = INT64_MAX;                     // NSNotFound

// The payload of `obj`, created empty on first use so that [[NSMutableArray alloc] init] and subclasses work.
ArrayPayload* arr(Addr obj) {
    uint64_t* slot = payload_slot(obj);
    if (!slot || !rt().is_kind_of(rt().class_of_object(obj), classes().array)) return nullptr;
    if (!*slot) *slot = store_add(std::make_unique<ArrayPayload>());
    return dynamic_cast<ArrayPayload*>(store_get(*slot));
}
bool is_mutable(Addr obj) { return is_kind(obj, classes().marray); }

bool equal_objects(Addr a, Addr b) { return a == b || (a && b && rt().send(a, "isEqual:", {b}) != 0); }

// Releases the elements of a removed/cleared payload. Releasing may run guest -dealloc code.
void release_all(const std::vector<Addr>& items) { for (Addr o : items) if (o) rt().release(o); }

bool check_items(const Addr* v, uint64_t n, const char* who) {
    for (uint64_t i = 0; i < n; ++i)
        if (!v[i]) { raise("NSInvalidArgumentException", std::format("*** {}: attempt to insert nil object from objects[{}]", who, i)); return false; }
    return true;
}

Addr init_items(Addr self, const Addr* v, uint64_t n, const char* who) {
    ArrayPayload* p = arr(self);
    if (!p) { raise("NSInvalidArgumentException", std::string(who) + ": receiver is not an NSArray"); return 0; }
    if (!check_items(v, n, who)) return 0;
    release_all(p->items);
    p->items.assign(v, v + n);
    for (Addr o : p->items) rt().retain(o);
    return self;
}

Addr make_array(Addr cls_self, const Addr* v, uint64_t n, const char* who) {
    Class* c = rt().class_at(cls_self);
    if (!c || c->is_meta || !rt().is_kind_of(c, classes().array)) c = classes().array;
    const Addr o = rt().create_instance(c, 0);
    if (!o) return 0;
    if (!init_items(o, v, n, who)) { rt().release(o); return 0; }
    return autoreleased(o);
}

// ---- NSArray class methods ---------------------------------------------------------------------------------------------------------
Addr c_array(Addr self, Addr) { return make_array(self, nullptr, 0, "+[NSArray array]"); }
Addr c_arrayWithObject(Addr self, Addr, Addr o) { return make_array(self, &o, 1, "+[NSArray arrayWithObject:]"); }
Addr c_arrayWithObjectsCount(Addr self, Addr, Addr objs, uint64_t n) {
    if (n > (1u << 26) || (n && !rt().readable(objs, n * 8))) { raise("NSInvalidArgumentException", "+[NSArray arrayWithObjects:count:]: invalid objects pointer or count"); return 0; }
    return make_array(self, reinterpret_cast<const Addr*>(objs), n, "+[NSArray arrayWithObjects:count:]");
}
Addr c_arrayWithArray(Addr self, Addr, Addr other) {
    ArrayPayload* p = arr(other);
    if (!p) { raise("NSInvalidArgumentException", "+[NSArray arrayWithArray:]: argument is not an NSArray"); return 0; }
    std::vector<Addr> copy = p->items;
    return make_array(self, copy.data(), copy.size(), "+[NSArray arrayWithArray:]");
}
bool collect_variadic(RegsReader& a, std::vector<Addr>& out) {      // nil-terminated list of objects
    for (;;) {
        const Addr o = a.next_int();
        if (!o) return true;
        out.push_back(o);
        if (out.size() > (1u << 22)) { raise("NSInvalidArgumentException", "arrayWithObjects:: the list is not nil-terminated"); return false; }
    }
}
void c_arrayWithObjects(GuestRegs* r) {
    RegsReader a(*r);
    const Addr self = a.next_int();
    a.next_int();
    std::vector<Addr> v;
    if (!collect_variadic(a, v)) return;
    r->ret_rax = make_array(self, v.data(), v.size(), "+[NSArray arrayWithObjects:]");
}

// ---- NSArray initialisers --------------------------------------------------------------------------------------------------------
Addr i_init(Addr self, Addr) { return init_items(self, nullptr, 0, "-[NSArray init]"); }
Addr i_initWithCapacity(Addr self, Addr, uint64_t) { return init_items(self, nullptr, 0, "-[NSMutableArray initWithCapacity:]"); }
Addr i_initWithObjectsCount(Addr self, Addr, Addr objs, uint64_t n) {
    if (n > (1u << 26) || (n && !rt().readable(objs, n * 8))) { raise("NSInvalidArgumentException", "-[NSArray initWithObjects:count:]: invalid objects pointer or count"); return 0; }
    return init_items(self, reinterpret_cast<const Addr*>(objs), n, "-[NSArray initWithObjects:count:]");
}
void i_initWithObjects(GuestRegs* r) {
    RegsReader a(*r);
    const Addr self = a.next_int();
    a.next_int();
    std::vector<Addr> v;
    if (!collect_variadic(a, v)) return;
    r->ret_rax = init_items(self, v.data(), v.size(), "-[NSArray initWithObjects:]");
}
Addr i_dealloc(Addr self, Addr) {
    if (uint64_t* slot = payload_slot(self)) {
        std::vector<Addr> items;
        if (auto* p = dynamic_cast<ArrayPayload*>(store_get(*slot))) items = std::move(p->items);
        store_drop(*slot);
        *slot = 0;
        release_all(items);
    }
    rt().destruct_instance(self);
    rt().dispose_instance(self);
    return 0;
}

// ---- NSArray queries -------------------------------------------------------------------------------------------------------------------
uint64_t i_count(Addr self, Addr) { auto* p = arr(self); return p ? p->items.size() : 0; }
Addr i_objectAtIndex(Addr self, Addr, uint64_t idx) {
    auto* p = arr(self);
    if (!p) return 0;
    if (idx >= p->items.size()) {
        raise("NSRangeException", std::format("*** -[NSArray objectAtIndex:]: index {} beyond bounds {}", idx, p->items.empty() ? std::string("for empty array") : std::format("[0 .. {}]", p->items.size() - 1)));
        return 0;
    }
    return p->items[idx];
}
Addr i_firstObject(Addr self, Addr) { auto* p = arr(self); return p && !p->items.empty() ? p->items.front() : 0; }
Addr i_lastObject(Addr self, Addr) { auto* p = arr(self); return p && !p->items.empty() ? p->items.back() : 0; }
uint64_t index_of(Addr self, Addr o) {
    auto* p = arr(self);
    if (!p) return kNotFound;
    for (size_t i = 0; i < p->items.size(); ++i) {
        const Addr item = p->items[i];
        if (equal_objects(item, o)) return i;
        if (!arr(self)) return kNotFound;                        // the guest -isEqual: freed us (cannot happen with correct retains)
    }
    return kNotFound;
}
uint64_t i_indexOfObject(Addr self, Addr, Addr o) { return index_of(self, o); }
uint64_t i_containsObject(Addr self, Addr, Addr o) { return index_of(self, o) != kNotFound ? 1 : 0; }
uint64_t i_isEqualToArray(Addr self, Addr, Addr other) {
    auto *a = arr(self), *b = arr(other);
    if (!a || !b || a->items.size() != b->items.size()) return 0;
    for (size_t i = 0; i < a->items.size(); ++i) if (!equal_objects(a->items[i], b->items[i])) return 0;
    return 1;
}
uint64_t i_isEqual(Addr self, Addr, Addr other) { return self == other || (is_kind(other, classes().array) && i_isEqualToArray(self, 0, other)) ? 1 : 0; }
uint64_t i_hash(Addr self, Addr) { auto* p = arr(self); return p ? p->items.size() : 0; }

std::string indent(std::string s) {
    std::string out;
    for (char c : s) { out += c; if (c == '\n') out += "    "; }
    return out;
}
Addr i_description(Addr self, Addr) {
    auto* p = arr(self);
    if (!p) return new_string_autoreleased("(null)");
    if (p->items.empty()) return new_string_autoreleased("(\n)");
    const std::vector<Addr> items = p->items;
    std::string out = "(\n";
    for (size_t i = 0; i < items.size(); ++i) {
        out += "    " + indent(describe(items[i]));
        out += i + 1 < items.size() ? ",\n" : "\n";
    }
    return new_string_autoreleased(out + ")");
}
Addr i_componentsJoinedByString(Addr self, Addr, Addr sep) {
    auto* p = arr(self);
    std::string_view s;
    if (!p || !string_view_of(sep, s)) { raise("NSInvalidArgumentException", "-[NSArray componentsJoinedByString:]: separator is not an NSString"); return 0; }
    const std::string separator(s);
    const std::vector<Addr> items = p->items;
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) { if (i) out += separator; out += describe(items[i]); }
    return new_string_autoreleased(out);
}
Addr i_arrayByAddingObject(Addr self, Addr, Addr o) {
    auto* p = arr(self);
    if (!p) return 0;
    if (!o) { raise("NSInvalidArgumentException", "-[NSArray arrayByAddingObject:]: attempt to insert nil object"); return 0; }
    std::vector<Addr> v = p->items;
    v.push_back(o);
    return make_array(classes().array->addr, v.data(), v.size(), "-[NSArray arrayByAddingObject:]");
}

// for (id x in array): NSFastEnumerationState { unsigned long state; id* itemsPtr; unsigned long* mutationsPtr; unsigned long extra[5]; }
void i_countByEnumerating(GuestRegs* r) {
    const Addr self = r->gp[0], state = r->gp[2], buf = r->gp[3];
    const uint64_t len = r->gp[4];
    auto* p = arr(self);
    if (!p || !state || !rt().readable(state, 64) || (len && !rt().readable(buf, len * 8))) { raise("NSInvalidArgumentException", "countByEnumeratingWithState:objects:count: invalid arguments"); return; }
    auto* st = reinterpret_cast<uint64_t*>(state);
    const uint64_t pos = st[0];
    const uint64_t total = p->items.size();
    if (pos >= total || len == 0) { r->ret_rax = 0; return; }
    const uint64_t n = std::min<uint64_t>(len, total - pos);
    auto* out = reinterpret_cast<Addr*>(buf);
    for (uint64_t i = 0; i < n; ++i) out[i] = p->items[pos + i];
    st[0] = pos + n;
    st[1] = buf;
    st[2] = state + 24;                                          // extra[0]: stays 0, so the compiler's mutation check never fires
    r->ret_rax = n;
}

// ---- mutable copies --------------------------------------------------------------------------------------------------------------------------
Addr i_copyWithZone(Addr self, Addr, Addr) {
    if (!is_mutable(self)) return rt().retain(self);
    auto* p = arr(self);
    if (!p) return 0;
    std::vector<Addr> v = p->items;
    const Addr o = make_array(classes().array->addr, v.data(), v.size(), "-[NSArray copy]");
    return o ? rt().retain(o) : 0;                               // copy returns +1
}
Addr i_mutableCopyWithZone(Addr self, Addr, Addr) {
    auto* p = arr(self);
    if (!p) return 0;
    std::vector<Addr> v = p->items;
    const Addr o = make_array(classes().marray->addr, v.data(), v.size(), "-[NSArray mutableCopy]");
    return o ? rt().retain(o) : 0;
}

// ---- NSMutableArray ------------------------------------------------------------------------------------------------------------------------
Addr i_addObject(Addr self, Addr, Addr o) {
    auto* p = arr(self);
    if (!p) return 0;
    if (!o) { raise("NSInvalidArgumentException", "*** -[NSMutableArray addObject:]: object cannot be nil"); return 0; }
    rt().retain(o);
    p->items.push_back(o);
    return 0;
}
Addr i_insertObject(Addr self, Addr, Addr o, uint64_t idx) {
    auto* p = arr(self);
    if (!p) return 0;
    if (!o) { raise("NSInvalidArgumentException", "*** -[NSMutableArray insertObject:atIndex:]: object cannot be nil"); return 0; }
    if (idx > p->items.size()) { raise("NSRangeException", std::format("*** -[NSMutableArray insertObject:atIndex:]: index {} beyond bounds [0 .. {}]", idx, p->items.size())); return 0; }
    rt().retain(o);
    p->items.insert(p->items.begin() + static_cast<ptrdiff_t>(idx), o);
    return 0;
}
void remove_at(Addr self, uint64_t idx, const char* who) {
    auto* p = arr(self);
    if (!p) return;
    if (idx >= p->items.size()) { raise("NSRangeException", std::format("*** -[NSMutableArray {}]: index {} beyond bounds {}", who, idx, p->items.empty() ? std::string("for empty array") : std::format("[0 .. {}]", p->items.size() - 1))); return; }
    const Addr o = p->items[idx];
    p->items.erase(p->items.begin() + static_cast<ptrdiff_t>(idx));
    rt().release(o);                                             // last: may run guest dealloc code
}
Addr i_removeObjectAtIndex(Addr self, Addr, uint64_t idx) { remove_at(self, idx, "removeObjectAtIndex:"); return 0; }
Addr i_removeLastObject(Addr self, Addr) {
    auto* p = arr(self);
    if (p && p->items.empty()) { raise("NSRangeException", "*** -[NSMutableArray removeLastObject]: index 0 beyond bounds for empty array"); return 0; }
    if (p) remove_at(self, p->items.size() - 1, "removeLastObject");
    return 0;
}
Addr i_removeAllObjects(Addr self, Addr) {
    auto* p = arr(self);
    if (!p) return 0;
    std::vector<Addr> old = std::move(p->items);
    p->items.clear();
    release_all(old);
    return 0;
}
Addr i_removeObject(Addr self, Addr, Addr o) {
    for (;;) {
        const uint64_t i = index_of(self, o);
        if (i == kNotFound) return 0;
        remove_at(self, i, "removeObject:");
    }
}
Addr i_replaceObject(Addr self, Addr, uint64_t idx, Addr o) {
    auto* p = arr(self);
    if (!p) return 0;
    if (!o) { raise("NSInvalidArgumentException", "*** -[NSMutableArray replaceObjectAtIndex:withObject:]: object cannot be nil"); return 0; }
    if (idx >= p->items.size()) { raise("NSRangeException", std::format("*** -[NSMutableArray replaceObjectAtIndex:withObject:]: index {} beyond bounds {}", idx, p->items.empty() ? std::string("for empty array") : std::format("[0 .. {}]", p->items.size() - 1))); return 0; }
    rt().retain(o);
    const Addr old = p->items[idx];
    p->items[idx] = o;
    rt().release(old);
    return 0;
}
Addr i_setObjectAtIndexedSubscript(Addr self, Addr, Addr o, uint64_t idx) {
    auto* p = arr(self);
    if (p && idx == p->items.size()) return i_addObject(self, 0, o);          // arr[arr.count] = x appends, like Apple
    return i_replaceObject(self, 0, idx, o);
}

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

ArrayPayload* array_of(Addr obj) { return arr(obj); }

Addr new_array(std::vector<Addr> items, bool mutable_, bool retain_items) {
    Class* c = mutable_ ? classes().marray : classes().array;
    const Addr o = rt().create_instance(c, 0);
    if (!o) return 0;
    uint64_t* slot = payload_slot(o);
    auto p = std::make_unique<ArrayPayload>();
    p->items = std::move(items);
    if (retain_items) for (Addr i : p->items) rt().retain(i);
    *slot = store_add(std::move(p));
    return autoreleased(o);
}

void install_nsarray(Runtime& r) {
    Class* a = r.declare_native_class("NSArray", "NSObject", 16);
    classes().array = a;
    Class* m = r.declare_native_class("NSMutableArray", "NSArray", 16);
    classes().marray = m;
    r.add_native_methods(a, true, {
        {"array", "@16@0:8", addr(c_array), false},
        {"arrayWithObject:", "@24@0:8@16", addr(c_arrayWithObject), false},
        {"arrayWithObjects:count:", "@32@0:8r^@16Q24", addr(c_arrayWithObjectsCount), false},
        {"arrayWithObjects:", "@24@0:8@16", addr(c_arrayWithObjects), true},
        {"arrayWithArray:", "@24@0:8@16", addr(c_arrayWithArray), false},
    });
    r.add_native_methods(a, false, {
        {"init", "@16@0:8", addr(i_init), false},
        {"initWithObjects:count:", "@32@0:8r^@16Q24", addr(i_initWithObjectsCount), false},
        {"initWithObjects:", "@24@0:8@16", addr(i_initWithObjects), true},
        {"dealloc", "v16@0:8", addr(i_dealloc), false},
        {"count", "Q16@0:8", addr(i_count), false},
        {"objectAtIndex:", "@24@0:8Q16", addr(i_objectAtIndex), false},
        {"objectAtIndexedSubscript:", "@24@0:8Q16", addr(i_objectAtIndex), false},
        {"firstObject", "@16@0:8", addr(i_firstObject), false},
        {"lastObject", "@16@0:8", addr(i_lastObject), false},
        {"indexOfObject:", "Q24@0:8@16", addr(i_indexOfObject), false},
        {"containsObject:", "B24@0:8@16", addr(i_containsObject), false},
        {"isEqualToArray:", "B24@0:8@16", addr(i_isEqualToArray), false},
        {"isEqual:", "B24@0:8@16", addr(i_isEqual), false},
        {"hash", "Q16@0:8", addr(i_hash), false},
        {"description", "@16@0:8", addr(i_description), false},
        {"componentsJoinedByString:", "@24@0:8@16", addr(i_componentsJoinedByString), false},
        {"arrayByAddingObject:", "@24@0:8@16", addr(i_arrayByAddingObject), false},
        {"countByEnumeratingWithState:objects:count:", "Q40@0:8^{_NSFastEnumerationState=Q^@^Q[5Q]}16^@24Q32", addr(i_countByEnumerating), true},
        {"copyWithZone:", "@24@0:8^{_NSZone=}16", addr(i_copyWithZone), false},
        {"mutableCopyWithZone:", "@24@0:8^{_NSZone=}16", addr(i_mutableCopyWithZone), false},
    });
    r.add_native_methods(m, false, {
        {"initWithCapacity:", "@24@0:8Q16", addr(i_initWithCapacity), false},
        {"addObject:", "v24@0:8@16", addr(i_addObject), false},
        {"insertObject:atIndex:", "v32@0:8@16Q24", addr(i_insertObject), false},
        {"removeObjectAtIndex:", "v24@0:8Q16", addr(i_removeObjectAtIndex), false},
        {"removeLastObject", "v16@0:8", addr(i_removeLastObject), false},
        {"removeAllObjects", "v16@0:8", addr(i_removeAllObjects), false},
        {"removeObject:", "v24@0:8@16", addr(i_removeObject), false},
        {"replaceObjectAtIndex:withObject:", "v32@0:8Q16@24", addr(i_replaceObject), false},
        {"setObject:atIndexedSubscript:", "v32@0:8@16Q24", addr(i_setObjectAtIndexedSubscript), false},
    });
    r.add_native_methods(m, true, {
        {"arrayWithCapacity:", "@24@0:8Q16", addr(c_array), false},
    });
}

}  // namespace cider::foundation
