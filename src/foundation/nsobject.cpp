// NSObject: the root class of the Foundation subset. Native methods; guest classes subclass it normally (instance layout: isa only).
#include <format>

#include "orvyn/libsystem.hpp"
#include "orvyn/objc.hpp"
#include "../libsystem/internal.hpp"
#include "foundation_internal.hpp"

namespace orvyn::foundation {
namespace {

Class* cls_of(Addr cls) { return rt().class_at(cls); }

// ---- class methods ---------------------------------------------------------------------------------------------------------------
Addr c_alloc(Addr self, Addr) {
    Class* c = cls_of(self);
    if (!c || c->is_meta) return 0;
    return rt().create_instance(c, 0);
}
Addr c_allocWithZone(Addr self, Addr, Addr) { return c_alloc(self, 0); }
Addr c_new(Addr self, Addr) {
    const Addr o = rt().send(self, "alloc");
    return o ? rt().send(o, "init") : 0;
}
Addr c_class(Addr self, Addr) { return self; }
Addr c_superclass(Addr self, Addr) { Class* c = cls_of(self); return c && c->super ? c->super->addr : 0; }
Addr c_initialize(Addr self, Addr) { return self; }
uint64_t c_instancesRespondToSelector(Addr self, Addr, Addr sel) { Class* c = cls_of(self); return c && !c->is_meta && rt().find_imp(c, sel) ? 1 : 0; }
uint64_t c_isSubclassOfClass(Addr self, Addr, Addr other) {
    Class *c = cls_of(self), *o = cls_of(other);
    return c && o && rt().is_kind_of(c, o) ? 1 : 0;
}
Addr c_description(Addr self, Addr) { Class* c = cls_of(self); return c ? new_string_autoreleased(c->name) : 0; }

// ---- instance methods ---------------------------------------------------------------------------------------------------------------
Addr i_init(Addr self, Addr) { return self; }

// Common tail of every -dealloc in the Foundation subset: ARC ivar cleanup of the guest subclass chain, then the memory.
void finish_dealloc(Addr self) {
    rt().destruct_instance(self);
    rt().dispose_instance(self);
}
Addr i_dealloc(Addr self, Addr) {
    // payload classes free their payload in their own -dealloc, which then calls this through [super dealloc]
    finish_dealloc(self);
    return 0;
}
Addr i_retain(Addr self, Addr) { return rt().retain(self); }
void i_release(Addr self, Addr) { rt().release(self); }
Addr i_autorelease(Addr self, Addr) { return rt().autorelease(self); }
uint64_t i_retainCount(Addr self, Addr) {
    if (auto* st = rt().object_state(self)) return st->rc;
    return ~0ull;                                                 // immortal (static) objects: NSUIntegerMax like Apple's constant strings
}
Addr i_class(Addr self, Addr) { Class* c = rt().class_of_object(self); return c ? (c->is_meta ? c->instance_class->addr : c->addr) : 0; }
Addr i_self(Addr self, Addr) { return self; }
uint64_t i_isEqual(Addr self, Addr, Addr other) { return self == other ? 1 : 0; }
uint64_t i_hash(Addr self, Addr) { return self >> 4; }
uint64_t i_isKindOfClass(Addr self, Addr, Addr cls) {
    Class *c = rt().class_of_object(self), *k = cls_of(cls);
    return c && k && rt().is_kind_of(c, k) ? 1 : 0;
}
uint64_t i_isMemberOfClass(Addr self, Addr, Addr cls) { Class* c = rt().class_of_object(self); return c && c->addr == cls ? 1 : 0; }
uint64_t i_respondsToSelector(Addr self, Addr, Addr sel) { Class* c = rt().class_of_object(self); return c && rt().find_imp(c, sel) ? 1 : 0; }
uint64_t i_conformsToProtocol(Addr self, Addr, Addr proto) {
    Class* c = rt().class_of_object(self);
    if (c && c->is_meta) c = c->instance_class;
    const std::string* n = rt().protocol_name_of(proto);
    return c && n && rt().conforms_to(c, *n) ? 1 : 0;
}
Addr i_description(Addr self, Addr) {
    Class* c = rt().class_of_object(self);
    const std::string name = c ? (c->is_meta ? c->instance_class->name : c->name) : "?";
    return new_string_autoreleased(std::format("<{}: {:#x}>", name, self));
}
Addr i_performSelector(Addr self, Addr, Addr sel) { return rt().send(self, sel); }
Addr i_performSelector1(Addr self, Addr, Addr sel, Addr a) { return rt().send(self, sel, {a}); }
Addr i_performSelector2(Addr self, Addr, Addr sel, Addr a) { return rt().send(self, sel, {a}); }   // (the 2-object form needs a 5th register: see raw below)
Addr i_copy(Addr self, Addr) {
    Class* c = rt().class_of_object(self);
    if (!c || !rt().find_imp(c, rt().sel("copyWithZone:"))) {
        raise("NSInvalidArgumentException", std::format("-[{} copyWithZone:]: unrecognized selector sent to instance {:#x}", c ? c->name : "?", self));
        return 0;
    }
    return rt().send(self, "copyWithZone:", {0});
}
Addr i_mutableCopy(Addr self, Addr) {
    Class* c = rt().class_of_object(self);
    if (!c || !rt().find_imp(c, rt().sel("mutableCopyWithZone:"))) {
        raise("NSInvalidArgumentException", std::format("-[{} mutableCopyWithZone:]: unrecognized selector sent to instance {:#x}", c ? c->name : "?", self));
        return 0;
    }
    return rt().send(self, "mutableCopyWithZone:", {0});
}
uint64_t i_isProxy(Addr, Addr) { return 0; }

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

void install_nsobject(Runtime& r) {
    Class* o = r.declare_native_class("NSObject", "", 8);
    classes().object = o;
    r.add_native_methods(o, true, {
        {"alloc", "@16@0:8", addr(c_alloc), false},
        {"allocWithZone:", "@24@0:8^{_NSZone=}16", addr(c_allocWithZone), false},
        {"new", "@16@0:8", addr(c_new), false},
        {"class", "#16@0:8", addr(c_class), false},
        {"superclass", "#16@0:8", addr(c_superclass), false},
        {"initialize", "v16@0:8", addr(c_initialize), false},
        {"instancesRespondToSelector:", "B24@0:8:16", addr(c_instancesRespondToSelector), false},
        {"isSubclassOfClass:", "B24@0:8#16", addr(c_isSubclassOfClass), false},
        {"description", "@16@0:8", addr(c_description), false},
    });
    r.add_native_methods(o, false, {
        {"init", "@16@0:8", addr(i_init), false},
        {"dealloc", "v16@0:8", addr(i_dealloc), false},
        {"retain", "@16@0:8", addr(i_retain), false},
        {"release", "Vv16@0:8", addr(i_release), false},
        {"autorelease", "@16@0:8", addr(i_autorelease), false},
        {"retainCount", "Q16@0:8", addr(i_retainCount), false},
        {"class", "#16@0:8", addr(i_class), false},
        {"self", "@16@0:8", addr(i_self), false},
        {"isEqual:", "B24@0:8@16", addr(i_isEqual), false},
        {"hash", "Q16@0:8", addr(i_hash), false},
        {"isKindOfClass:", "B24@0:8#16", addr(i_isKindOfClass), false},
        {"isMemberOfClass:", "B24@0:8#16", addr(i_isMemberOfClass), false},
        {"respondsToSelector:", "B24@0:8:16", addr(i_respondsToSelector), false},
        {"conformsToProtocol:", "B24@0:8@16", addr(i_conformsToProtocol), false},
        {"description", "@16@0:8", addr(i_description), false},
        {"debugDescription", "@16@0:8", addr(i_description), false},
        {"performSelector:", "@24@0:8:16", addr(i_performSelector), false},
        {"performSelector:withObject:", "@32@0:8:16@24", addr(i_performSelector1), false},
        {"copy", "@16@0:8", addr(i_copy), false},
        {"mutableCopy", "@16@0:8", addr(i_mutableCopy), false},
        {"isProxy", "B16@0:8", addr(i_isProxy), false},
    });
}

}  // namespace orvyn::foundation
