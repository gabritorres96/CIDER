// NSNumber (immutable) holding a BOOL, a 64-bit integer or a double. Not implemented: unsigned 64-bit values above INT64_MAX keep their
// bit pattern as a signed value, NSDecimalNumber, number formatters, class clusters (all instances are plain NSNumber).
#include <cstring>
#include <format>

#include "cider/libsystem.hpp"
#include "cider/objc.hpp"
#include "../libsystem/internal.hpp"
#include "foundation_internal.hpp"

namespace cider::foundation {
namespace {

using abi::GuestRegs;

NumberPayload* num_of(Addr obj) { return dynamic_cast<NumberPayload*>(payload_of(obj)); }

double as_double(const NumberPayload& n) { return n.kind == NumberPayload::Kind::Double ? n.d : static_cast<double>(n.i); }
int64_t as_int(const NumberPayload& n) {
    if (n.kind != NumberPayload::Kind::Double) return n.i;
    if (n.d != n.d) return 0;
    if (n.d >= 9.2233720368547758e18) return INT64_MAX;
    if (n.d <= -9.2233720368547758e18) return INT64_MIN;
    return static_cast<int64_t>(n.d);
}
int compare_numbers(const NumberPayload& a, const NumberPayload& b) {
    if (a.kind != NumberPayload::Kind::Double && b.kind != NumberPayload::Kind::Double) return a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
    const double x = as_double(a), y = as_double(b);
    return x < y ? -1 : (x > y ? 1 : 0);
}

Addr make_number(Addr cls_self, const NumberPayload& v) {
    Class* c = rt().class_at(cls_self);
    if (!c || c->is_meta || !rt().is_kind_of(c, classes().number)) c = classes().number;
    auto p = std::make_unique<NumberPayload>(v);
    return autoreleased(make_object(c, std::move(p)));
}
NumberPayload as_int_payload(int64_t i) { NumberPayload n; n.kind = NumberPayload::Kind::Int; n.i = i; return n; }

// ---- constructors ----------------------------------------------------------------------------------------------------------------
Addr c_numberWithInt(Addr self, Addr, uint64_t v) { return make_number(self, as_int_payload(static_cast<int32_t>(v))); }
Addr c_numberWithUnsignedInt(Addr self, Addr, uint64_t v) { return make_number(self, as_int_payload(static_cast<uint32_t>(v))); }
Addr c_numberWithLong(Addr self, Addr, uint64_t v) { return make_number(self, as_int_payload(static_cast<int64_t>(v))); }
Addr c_numberWithChar(Addr self, Addr, uint64_t v) { return make_number(self, as_int_payload(static_cast<int8_t>(v))); }
Addr c_numberWithUChar(Addr self, Addr, uint64_t v) { return make_number(self, as_int_payload(static_cast<uint8_t>(v))); }
Addr c_numberWithShort(Addr self, Addr, uint64_t v) { return make_number(self, as_int_payload(static_cast<int16_t>(v))); }
Addr c_numberWithBool(Addr self, Addr, uint64_t v) {
    NumberPayload n;
    n.kind = NumberPayload::Kind::Bool;
    n.i = (v & 0xff) != 0;
    return make_number(self, n);
}
void c_numberWithDouble(GuestRegs* r) {
    double d;
    std::memcpy(&d, &r->xmm[0], 8);
    NumberPayload n;
    n.kind = NumberPayload::Kind::Double;
    n.d = d;
    r->ret_rax = make_number(r->gp[0], n);
}
void c_numberWithFloat(GuestRegs* r) {
    float f;
    std::memcpy(&f, &r->xmm[0], 4);
    NumberPayload n;
    n.kind = NumberPayload::Kind::Double;
    n.d = f;
    r->ret_rax = make_number(r->gp[0], n);
}

// ---- instance methods ------------------------------------------------------------------------------------------------------------
Addr i_dealloc(Addr self, Addr) {
    if (uint64_t* slot = payload_slot(self)) { store_drop(*slot); *slot = 0; }
    rt().destruct_instance(self);
    rt().dispose_instance(self);
    return 0;
}
int64_t i_longLongValue(Addr self, Addr) { auto* n = num_of(self); return n ? as_int(*n) : 0; }
int32_t i_intValue(Addr self, Addr) { auto* n = num_of(self); return n ? static_cast<int32_t>(as_int(*n)) : 0; }
uint64_t i_boolValue(Addr self, Addr) {
    auto* n = num_of(self);
    if (!n) return 0;
    return n->kind == NumberPayload::Kind::Double ? (n->d != 0) : (n->i != 0);
}
void i_doubleValue(GuestRegs* r) {
    auto* n = num_of(r->gp[0]);
    const double d = n ? as_double(*n) : 0.0;
    std::memcpy(&r->ret_xmm0, &d, 8);
}
void i_floatValue(GuestRegs* r) {
    auto* n = num_of(r->gp[0]);
    const float f = n ? static_cast<float>(as_double(*n)) : 0.f;
    r->ret_xmm0 = 0;
    std::memcpy(&r->ret_xmm0, &f, 4);
}
uint64_t i_isEqualToNumber(Addr self, Addr, Addr other) {
    auto *a = num_of(self), *b = num_of(other);
    return a && b && compare_numbers(*a, *b) == 0 ? 1 : 0;
}
uint64_t i_isEqual(Addr self, Addr, Addr other) { return i_isEqualToNumber(self, 0, other); }
uint64_t i_hash(Addr self, Addr) {
    auto* n = num_of(self);
    if (!n) return 0;
    // numerically equal values hash equally, whatever their stored kind
    if (n->kind == NumberPayload::Kind::Double) {
        const double d = n->d;
        if (d == static_cast<double>(static_cast<int64_t>(d))) return static_cast<uint64_t>(static_cast<int64_t>(d));
        uint64_t bits;
        std::memcpy(&bits, &d, 8);
        return bits;
    }
    return static_cast<uint64_t>(n->i);
}
int64_t i_compare(Addr self, Addr, Addr other) {
    auto *a = num_of(self), *b = num_of(other);
    if (!a || !b) { raise("NSInvalidArgumentException", "-[NSNumber compare:]: argument is not an NSNumber"); return 0; }
    return compare_numbers(*a, *b);
}
std::string text_of(const NumberPayload& n) {
    switch (n.kind) {
        case NumberPayload::Kind::Bool: return n.i ? "1" : "0";
        case NumberPayload::Kind::Int: return std::to_string(n.i);
        case NumberPayload::Kind::Double: return std::format("{}", n.d);
    }
    return {};
}
Addr i_description(Addr self, Addr) { auto* n = num_of(self); return new_string_autoreleased(n ? text_of(*n) : "(null)"); }
Addr i_copyWithZone(Addr self, Addr, Addr) { return rt().retain(self); }

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

bool number_value(Addr obj, NumberPayload& out) {
    auto* n = num_of(obj);
    if (!n) return false;
    out = *n;
    return true;
}
Addr new_number(const NumberPayload& n, bool autorelease) {
    Addr o = make_object(classes().number, std::make_unique<NumberPayload>(n));
    return autorelease ? autoreleased(o) : o;
}

void install_nsnumber(Runtime& r) {
    Class* n = r.declare_native_class("NSNumber", "NSObject", 16);
    classes().number = n;
    r.add_native_methods(n, true, {
        {"numberWithInt:", "@20@0:8i16", addr(c_numberWithInt), false},
        {"numberWithInteger:", "@24@0:8q16", addr(c_numberWithLong), false},
        {"numberWithLong:", "@24@0:8q16", addr(c_numberWithLong), false},
        {"numberWithLongLong:", "@24@0:8q16", addr(c_numberWithLong), false},
        {"numberWithUnsignedInt:", "@20@0:8I16", addr(c_numberWithUnsignedInt), false},
        {"numberWithUnsignedInteger:", "@24@0:8Q16", addr(c_numberWithLong), false},
        {"numberWithUnsignedLong:", "@24@0:8Q16", addr(c_numberWithLong), false},
        {"numberWithUnsignedLongLong:", "@24@0:8Q16", addr(c_numberWithLong), false},
        {"numberWithChar:", "@20@0:8c16", addr(c_numberWithChar), false},
        {"numberWithUnsignedChar:", "@20@0:8C16", addr(c_numberWithUChar), false},
        {"numberWithShort:", "@20@0:8s16", addr(c_numberWithShort), false},
        {"numberWithBool:", "@20@0:8B16", addr(c_numberWithBool), false},
        {"numberWithDouble:", "@24@0:8d16", addr(c_numberWithDouble), true},
        {"numberWithFloat:", "@20@0:8f16", addr(c_numberWithFloat), true},
    });
    r.add_native_methods(n, false, {
        {"dealloc", "v16@0:8", addr(i_dealloc), false},
        {"intValue", "i16@0:8", addr(i_intValue), false},
        {"integerValue", "q16@0:8", addr(i_longLongValue), false},
        {"longValue", "q16@0:8", addr(i_longLongValue), false},
        {"longLongValue", "q16@0:8", addr(i_longLongValue), false},
        {"unsignedIntegerValue", "Q16@0:8", addr(i_longLongValue), false},
        {"boolValue", "B16@0:8", addr(i_boolValue), false},
        {"doubleValue", "d16@0:8", addr(i_doubleValue), true},
        {"floatValue", "f16@0:8", addr(i_floatValue), true},
        {"isEqualToNumber:", "B24@0:8@16", addr(i_isEqualToNumber), false},
        {"isEqual:", "B24@0:8@16", addr(i_isEqual), false},
        {"hash", "Q16@0:8", addr(i_hash), false},
        {"compare:", "q24@0:8@16", addr(i_compare), false},
        {"description", "@16@0:8", addr(i_description), false},
        {"stringValue", "@16@0:8", addr(i_description), false},
        {"copyWithZone:", "@24@0:8^{_NSZone=}16", addr(i_copyWithZone), false},
    });
}

}  // namespace cider::foundation
