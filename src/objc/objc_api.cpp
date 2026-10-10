// The libobjc API a Mach-O program can import (libobjc.A.dylib). Every function does the operation it names on the runtime's real
// tables; invalid arguments yield nil/NULL/NO (never a fake success). Signatures follow <objc/runtime.h> of the Apple runtime.
#include <cstring>

#include "cider/libsystem.hpp"
#include "cider/log.hpp"
#include "cider/objc.hpp"
#include "../libsystem/internal.hpp"

extern "C" {
void cider_objc_msgSend();
void cider_objc_msgSend_stret();
void cider_objc_msgSendSuper();
void cider_objc_msgSendSuper2();
}

namespace cider::objc {
namespace {

Runtime* RT() { return Runtime::current(); }
template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }
bool valid_c_string(Addr a, std::string& out) { Runtime* rt = RT(); return rt && rt->read_cstring(a, out, 1024); }

// ---- classes -----------------------------------------------------------------------------------------------------------------
Addr w_objc_getClass(Addr name) {
    std::string n;
    if (!valid_c_string(name, n)) return 0;
    Class* c = RT()->class_named(n);
    return c ? c->addr : 0;
}
Addr w_objc_getMetaClass(Addr name) {
    std::string n;
    if (!valid_c_string(name, n)) return 0;
    Class* c = RT()->class_named(n);
    return c ? c->meta->addr : 0;
}
Addr w_objc_getRequiredClass(Addr name) {
    Addr c = w_objc_getClass(name);
    if (!c) libsystem::detail::trap("objc_getRequiredClass: class not found");
    return c;
}
Addr w_object_getClass(Addr obj) {
    Runtime* rt = RT();
    Class* c = rt ? rt->class_of_object(obj) : nullptr;
    return c ? c->addr : 0;
}
Addr w_object_setClass(Addr obj, Addr cls) {
    Runtime* rt = RT();
    Class* old = rt ? rt->class_of_object(obj) : nullptr;
    Class* c = rt ? rt->class_at(cls) : nullptr;
    if (!old || !c || !rt->readable(obj, 8)) return 0;
    *reinterpret_cast<Addr*>(obj) = cls;
    return old->addr;
}
Addr w_object_getClassName(Addr obj) {
    Runtime* rt = RT();
    Class* c = rt ? rt->class_of_object(obj) : nullptr;
    return c ? reinterpret_cast<Addr>((c->is_meta ? c->instance_class->name : c->name).c_str()) : reinterpret_cast<Addr>("nil");
}
Addr w_class_getName(Addr cls) {
    Class* c = RT() ? RT()->class_at(cls) : nullptr;
    return c ? reinterpret_cast<Addr>(c->name.c_str()) : reinterpret_cast<Addr>("nil");
}
Addr w_class_getSuperclass(Addr cls) {
    Class* c = RT() ? RT()->class_at(cls) : nullptr;
    return c && c->super ? c->super->addr : 0;
}
int w_class_isMetaClass(Addr cls) { Class* c = RT() ? RT()->class_at(cls) : nullptr; return c && c->is_meta ? 1 : 0; }
uint64_t w_class_getInstanceSize(Addr cls) { Class* c = RT() ? RT()->class_at(cls) : nullptr; return c && !c->is_meta ? ((uint64_t{c->instance_size} + 7) & ~uint64_t{7}) : 0; }   // word aligned, like the Apple runtime
int w_class_getVersion(Addr) { return 0; }

// ---- selectors ----------------------------------------------------------------------------------------------------------------
Addr w_sel_registerName(Addr name) {
    std::string n;
    if (!valid_c_string(name, n)) return 0;
    return RT()->sel(n);
}
Addr w_sel_getName(Addr s) {
    Runtime* rt = RT();
    if (!rt) return 0;
    const std::string_view n = rt->sel_name(s);
    return n.empty() ? reinterpret_cast<Addr>("<null selector>") : s;     // a SEL IS the pointer to its unique name string
}
int w_sel_isEqual(Addr a, Addr b) { return a == b ? 1 : 0; }

// ---- methods ----------------------------------------------------------------------------------------------------------------------
Addr w_class_getInstanceMethod(Addr cls, Addr s) {
    Runtime* rt = RT();
    Class* c = rt ? rt->class_at(cls) : nullptr;
    if (!c || c->is_meta) return 0;
    Method* m = rt->find_method(c, s);
    return m ? rt->method_handle(c, *m) : 0;
}
Addr w_class_getClassMethod(Addr cls, Addr s) {
    Runtime* rt = RT();
    Class* c = rt ? rt->class_at(cls) : nullptr;
    if (!c || c->is_meta) return 0;
    Method* m = rt->find_method(c->meta, s);
    return m ? rt->method_handle(c->meta, *m) : 0;
}
Addr w_class_getMethodImplementation(Addr cls, Addr s) {
    Runtime* rt = RT();
    Class* c = rt ? rt->class_at(cls) : nullptr;
    if (!c) return 0;
    const Addr imp = rt->find_imp(c, s);
    return imp ? imp : rt->forward_trap_imp();
}
int w_class_respondsToSelector(Addr cls, Addr s) {
    Runtime* rt = RT();
    Class* c = rt ? rt->class_at(cls) : nullptr;
    return c && rt->find_imp(c, s) ? 1 : 0;
}
Addr w_method_getImplementation(Addr h) { Method* m = RT() ? RT()->method_from_handle(h) : nullptr; return m ? m->imp : 0; }
Addr w_method_getName(Addr h) { Method* m = RT() ? RT()->method_from_handle(h) : nullptr; return m ? m->sel : 0; }
Addr w_method_getTypeEncoding(Addr h) { Method* m = RT() ? RT()->method_from_handle(h) : nullptr; return m ? reinterpret_cast<Addr>(m->types.c_str()) : 0; }
Addr w_method_setImplementation(Addr h, Addr imp) {
    Runtime* rt = RT();
    Method* m = rt ? rt->method_from_handle(h) : nullptr;
    if (!m || !rt->readable(imp, 1)) return 0;
    const Addr old = m->imp;
    m->imp = imp;
    rt->flush_caches_public();
    return old;
}
void w_method_exchangeImplementations(Addr a, Addr b) {
    Runtime* rt = RT();
    Method *x = rt ? rt->method_from_handle(a) : nullptr, *y = rt ? rt->method_from_handle(b) : nullptr;
    if (!x || !y) return;
    std::swap(x->imp, y->imp);
    rt->flush_caches_public();
}
int w_class_addMethod(Addr cls, Addr s, Addr imp, Addr types) {
    Runtime* rt = RT();
    Class* c = rt ? rt->class_at(cls) : nullptr;
    std::string t;
    if (types) valid_c_string(types, t);
    return c && rt->add_method(c, s, t, imp, /*replace=*/false) ? 1 : 0;      // like the real function: NO if the class already has it
}
Addr w_class_replaceMethod(Addr cls, Addr s, Addr imp, Addr types) {
    Runtime* rt = RT();
    Class* c = rt ? rt->class_at(cls) : nullptr;
    std::string t;
    if (types) valid_c_string(types, t);
    Addr old = 0;
    if (!c || !rt->add_method(c, s, t, imp, /*replace=*/true, &old)) return 0;
    return old;
}

// ---- instance variables ---------------------------------------------------------------------------------------------------------------
Addr w_class_getInstanceVariable(Addr cls, Addr name) {
    Runtime* rt = RT();
    Class* c = rt ? rt->class_at(cls) : nullptr;
    std::string n;
    if (!c || c->is_meta || !valid_c_string(name, n)) return 0;
    for (Class* k = c; k; k = k->super)
        for (auto& v : k->ivars)
            if (v.name == n) return rt->ivar_handle(k, v);
    return 0;
}
int64_t w_ivar_getOffset(Addr h) { Ivar* v = RT() ? RT()->ivar_from_handle(h) : nullptr; return v ? v->offset : 0; }
Addr w_ivar_getName(Addr h) { Ivar* v = RT() ? RT()->ivar_from_handle(h) : nullptr; return v ? reinterpret_cast<Addr>(v->name.c_str()) : 0; }
Addr w_ivar_getTypeEncoding(Addr h) { Ivar* v = RT() ? RT()->ivar_from_handle(h) : nullptr; return v ? reinterpret_cast<Addr>(v->type.c_str()) : 0; }
Addr w_object_getIvar(Addr obj, Addr h) {
    Runtime* rt = RT();
    Ivar* v = rt ? rt->ivar_from_handle(h) : nullptr;
    Class* c = rt ? rt->class_of_object(obj) : nullptr;
    if (!v || !c || v->size > 8 || !rt->readable(obj + v->offset, v->size ? v->size : 8)) return 0;
    Addr out = 0;
    std::memcpy(&out, reinterpret_cast<const void*>(obj + v->offset), v->size ? v->size : 8);
    return out;
}
void w_object_setIvar(Addr obj, Addr h, Addr value) {
    Runtime* rt = RT();
    Ivar* v = rt ? rt->ivar_from_handle(h) : nullptr;
    Class* c = rt ? rt->class_of_object(obj) : nullptr;
    if (!v || !c || v->size > 8 || !rt->readable(obj + v->offset, v->size ? v->size : 8)) return;
    std::memcpy(reinterpret_cast<void*>(obj + v->offset), &value, v->size ? v->size : 8);
}
int w_class_addIvar(Addr cls, Addr name, uint64_t size, uint64_t align_log2, Addr types) {
    Runtime* rt = RT();
    Class* c = rt ? rt->class_at(cls) : nullptr;
    std::string n, t;
    if (!c || !c->under_construction || c->is_meta || size == 0 || size > 4096 || align_log2 > 12) return 0;   // only between allocateClassPair and registerClassPair
    if (name && !valid_c_string(name, n)) return 0;
    if (types) valid_c_string(types, t);
    for (auto& v : c->ivars) if (v.name == n && !n.empty()) return 0;
    const uint64_t align = uint64_t{1} << align_log2;
    const uint64_t off = (c->instance_size + align - 1) & ~(align - 1);
    const Addr var = rt->arena_alloc(8, 8);
    if (!var || off + size > 0x7fffffff) return 0;
    Ivar v;
    v.name = n; v.type = t; v.offset_var = var; v.offset = static_cast<uint32_t>(off); v.size = static_cast<uint32_t>(size); v.align_log2 = static_cast<uint8_t>(align_log2);
    *reinterpret_cast<uint32_t*>(var) = v.offset;
    c->ivars.push_back(std::move(v));
    c->instance_size = static_cast<uint32_t>(off + size);
    return 1;
}

// ---- instances ----------------------------------------------------------------------------------------------------------------------------
Addr w_class_createInstance(Addr cls, uint64_t extra) {
    Runtime* rt = RT();
    Class* c = rt ? rt->class_at(cls) : nullptr;
    return c ? rt->create_instance(c, static_cast<size_t>(extra)) : 0;
}
Addr w_object_dispose(Addr obj) {
    Runtime* rt = RT();
    if (rt && rt->is_live_object(obj)) rt->dispose_instance(obj);
    return 0;
}

// ---- class construction -----------------------------------------------------------------------------------------------------------------------
Addr w_objc_allocateClassPair(Addr super, Addr name, uint64_t extra) {
    Runtime* rt = RT();
    std::string n;
    if (!rt || !valid_c_string(name, n)) return 0;
    Class* s = nullptr;
    if (super) { s = rt->class_at(super); if (!s) return 0; }
    Class* c = rt->allocate_class_pair(s, n, static_cast<size_t>(extra));
    return c ? c->addr : 0;
}
void w_objc_registerClassPair(Addr cls) { Runtime* rt = RT(); Class* c = rt ? rt->class_at(cls) : nullptr; if (c) rt->register_class_pair(c); }
void w_objc_disposeClassPair(Addr cls) { Runtime* rt = RT(); Class* c = rt ? rt->class_at(cls) : nullptr; if (c) rt->dispose_class_pair(c); }

// ---- protocols ---------------------------------------------------------------------------------------------------------------------------------
Addr w_objc_getProtocol(Addr name) {
    Runtime* rt = RT();
    std::string n;
    if (!rt || !valid_c_string(name, n)) return 0;
    return rt->protocol_handle(n);
}
Addr w_protocol_getName(Addr h) { Runtime* rt = RT(); const std::string* n = rt ? rt->protocol_name_of(h) : nullptr; return n ? reinterpret_cast<Addr>(n->c_str()) : 0; }
int w_class_conformsToProtocol(Addr cls, Addr proto) {
    Runtime* rt = RT();
    Class* c = rt ? rt->class_at(cls) : nullptr;
    const std::string* n = rt ? rt->protocol_name_of(proto) : nullptr;
    return c && n && rt->conforms_to(c, *n) ? 1 : 0;
}

}  // namespace

void add_objc_symbols(std::vector<libsystem::HostSymbol>& t) {
    using libsystem::HostSymbol;
    using libsystem::Lib;
    auto fn = [&](const char* name, void* f) { t.push_back(HostSymbol{name, f, false, false, false, Lib::Objc}); };
    auto direct = [&](const char* name, void (*f)()) { t.push_back(HostSymbol{name, reinterpret_cast<void*>(f), false, false, true, Lib::Objc}); };
    direct("_objc_msgSend", &cider_objc_msgSend);
    direct("_objc_msgSend_fpret", &cider_objc_msgSend);          // long double returns (x87) are not supported; double results travel in xmm0 like any other
    direct("_objc_msgSend_stret", &cider_objc_msgSend_stret);
    direct("_objc_msgSendSuper", &cider_objc_msgSendSuper);
    direct("_objc_msgSendSuper2", &cider_objc_msgSendSuper2);
    fn("_objc_getClass", addr(w_objc_getClass));
    fn("_objc_lookUpClass", addr(w_objc_getClass));
    fn("_objc_getMetaClass", addr(w_objc_getMetaClass));
    fn("_objc_getRequiredClass", addr(w_objc_getRequiredClass));
    fn("_object_getClass", addr(w_object_getClass));
    fn("_object_setClass", addr(w_object_setClass));
    fn("_object_getClassName", addr(w_object_getClassName));
    fn("_class_getName", addr(w_class_getName));
    fn("_class_getSuperclass", addr(w_class_getSuperclass));
    fn("_class_isMetaClass", addr(w_class_isMetaClass));
    fn("_class_getInstanceSize", addr(w_class_getInstanceSize));
    fn("_class_getVersion", addr(w_class_getVersion));
    fn("_sel_registerName", addr(w_sel_registerName));
    fn("_sel_getUid", addr(w_sel_registerName));
    fn("_sel_getName", addr(w_sel_getName));
    fn("_sel_isEqual", addr(w_sel_isEqual));
    fn("_class_getInstanceMethod", addr(w_class_getInstanceMethod));
    fn("_class_getClassMethod", addr(w_class_getClassMethod));
    fn("_class_getMethodImplementation", addr(w_class_getMethodImplementation));
    fn("_class_respondsToSelector", addr(w_class_respondsToSelector));
    fn("_method_getImplementation", addr(w_method_getImplementation));
    fn("_method_getName", addr(w_method_getName));
    fn("_method_getTypeEncoding", addr(w_method_getTypeEncoding));
    fn("_method_setImplementation", addr(w_method_setImplementation));
    fn("_method_exchangeImplementations", addr(w_method_exchangeImplementations));
    fn("_class_addMethod", addr(w_class_addMethod));
    fn("_class_replaceMethod", addr(w_class_replaceMethod));
    fn("_class_getInstanceVariable", addr(w_class_getInstanceVariable));
    fn("_ivar_getOffset", addr(w_ivar_getOffset));
    fn("_ivar_getName", addr(w_ivar_getName));
    fn("_ivar_getTypeEncoding", addr(w_ivar_getTypeEncoding));
    fn("_object_getIvar", addr(w_object_getIvar));
    fn("_object_setIvar", addr(w_object_setIvar));
    fn("_class_addIvar", addr(w_class_addIvar));
    fn("_class_createInstance", addr(w_class_createInstance));
    fn("_object_dispose", addr(w_object_dispose));
    fn("_objc_allocateClassPair", addr(w_objc_allocateClassPair));
    fn("_objc_registerClassPair", addr(w_objc_registerClassPair));
    fn("_objc_disposeClassPair", addr(w_objc_disposeClassPair));
    fn("_objc_getProtocol", addr(w_objc_getProtocol));
    fn("_protocol_getName", addr(w_protocol_getName));
    fn("_class_conformsToProtocol", addr(w_class_conformsToProtocol));
    add_objc_memory_symbols(t);
}

}  // namespace cider::objc
