// NSException and objc_exception_throw (ORVYN 0.5.4) -- LIMITED: exceptions can be created and thrown, they cannot be caught.
//
// What exists: NSException objects (+exceptionWithName:reason:userInfo:, +raise:format:, -raise, name/reason/userInfo) and objc_exception_throw. A throw ends the guest
// with exactly what an UNCAUGHT exception does on macOS: "*** Terminating app due to uncaught exception 'Name', reason: 'Reason'" (the process does not continue).
// What does not exist: @try/@catch/@finally. They need the Darwin zero-cost unwinder (__Unwind_RaiseException, __objc_personality_v0, objc_begin_catch/end_catch, DWARF
// call-frame information of every guest and host frame in between). Unwinding through the host frames of the ABI bridge is not implemented and nothing can be
// verified there. Since 0.6 a program that merely CONTAINS handlers still loads (the entry points are honest stubs, see libsystem/frameworks.cpp) and runs normally while
// nothing is thrown; a throw in such a program stops it with an explicit "no unwinder" diagnostic instead of guessing whether a handler would have caught it.
#include <format>

#include "orvyn/libsystem.hpp"
#include "orvyn/objc.hpp"
#include "../libsystem/internal.hpp"
#include "foundation_internal.hpp"

namespace orvyn::foundation {
namespace {

struct ExceptionPayload : Payload { Addr name = 0, reason = 0, userinfo = 0; };
Class* g_cls = nullptr;

ExceptionPayload* ex_of(Addr o) {
    uint64_t* slot = payload_slot(o);
    if (!slot || !is_kind(o, g_cls)) return nullptr;
    if (!*slot) *slot = store_add(std::make_unique<ExceptionPayload>());
    return dynamic_cast<ExceptionPayload*>(store_get(*slot));
}
void keep(Addr& slot, Addr v) {
    if (v) rt().retain(v);
    const Addr old = slot;
    slot = v;
    if (old) rt().release(old);
}
Addr make(Addr name, Addr reason, Addr userinfo) {
    const Addr o = rt().create_instance(g_cls, 0);
    ExceptionPayload* p = o ? ex_of(o) : nullptr;
    if (!p) return 0;
    keep(p->name, name);
    keep(p->reason, reason);
    keep(p->userinfo, userinfo);
    return o;
}

void throw_exception(Addr ex) {
    ExceptionPayload* p = ex_of(ex);
    const std::string name = p ? describe(p->name) : "(unknown)";
    const std::string reason = p && p->reason ? describe(p->reason) : "(null)";
    if (rt().ctx().exception_handlers_linked) {
        libsystem::detail::trap("*** Objective-C exception '" + name + "', reason: '" + reason + "' was thrown, but the program contains @catch/@finally handlers and ORVYN has no unwinder to reach them "
                                "(on macOS one of them might have caught it): the program was stopped, not continued");
        return;
    }
    libsystem::detail::trap("*** Terminating app due to uncaught exception '" + name + "', reason: '" + reason + "'");
}

void e_exceptionWithName(abi::GuestRegs* r) { const Addr o = make(r->gp[2], r->gp[3], r->gp[4]); if (o) r->ret_rax = autoreleased(o); }
void e_initWithName(abi::GuestRegs* r) {
    ExceptionPayload* p = ex_of(r->gp[0]);
    if (!p) return;
    keep(p->name, r->gp[2]);
    keep(p->reason, r->gp[3]);
    keep(p->userinfo, r->gp[4]);
    r->ret_rax = r->gp[0];
}
void e_raiseFormat(abi::GuestRegs* r) {                         // +raise:(NSString*)name format:(NSString*)format, ...
    abi::RegsReader a(*r);
    a.next_int();
    a.next_int();
    const Addr name = a.next_int();
    std::string_view f;
    if (!string_view_of(a.next_int(), f)) { raise("NSInvalidArgumentException", "+[NSException raise:format:]: the format is not an NSString"); return; }
    std::string text;
    if (!format_with_objects(std::string(f), a, text)) return;
    const Addr reason = new_string(text);
    const Addr ex = make(name, reason, 0);
    rt().release(reason);
    if (ex) throw_exception(ex);
}
Addr e_raise(Addr self, Addr) { throw_exception(self); return 0; }
Addr e_name(Addr self, Addr) { ExceptionPayload* p = ex_of(self); return p ? p->name : 0; }
Addr e_reason(Addr self, Addr) { ExceptionPayload* p = ex_of(self); return p ? p->reason : 0; }
Addr e_userInfo(Addr self, Addr) { ExceptionPayload* p = ex_of(self); return p ? p->userinfo : 0; }
Addr e_description(Addr self, Addr) {
    ExceptionPayload* p = ex_of(self);
    return p && p->reason ? p->reason : (p ? p->name : 0);
}
Addr e_dealloc(Addr self, Addr) {
    if (ExceptionPayload* p = ex_of(self)) {
        const Addr a = p->name, b = p->reason, c = p->userinfo;
        p->name = p->reason = p->userinfo = 0;
        if (a) rt().release(a);
        if (b) rt().release(b);
        if (c) rt().release(c);
    }
    if (uint64_t* slot = payload_slot(self)) { store_drop(*slot); *slot = 0; }
    rt().destruct_instance(self);
    rt().dispose_instance(self);
    return 0;
}
void w_objc_exception_throw(Addr ex) {
    if (ex) throw_exception(ex);
    else libsystem::detail::trap(rt().ctx().exception_handlers_linked ? "*** nil exception thrown in a program with @catch/@finally handlers; ORVYN has no unwinder to reach them"
                                                                      : "*** Terminating app due to uncaught exception: nil exception thrown");
}

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

void install_nsexception(Runtime& r) {
    Class* c = r.declare_native_class("NSException", "NSObject", 16);
    g_cls = c;
    classes().extra_payload.push_back(c);
    r.add_native_methods(c, true, {
        {"exceptionWithName:reason:userInfo:", "@40@0:8@16@24@32", addr(e_exceptionWithName), true},
        {"raise:format:", "v32@0:8@16@24", addr(e_raiseFormat), true},
    });
    r.add_native_methods(c, false, {
        {"initWithName:reason:userInfo:", "@40@0:8@16@24@32", addr(e_initWithName), true},
        {"dealloc", "v16@0:8", addr(e_dealloc), false},
        {"raise", "v16@0:8", addr(e_raise), false},
        {"name", "@16@0:8", addr(e_name), false},
        {"reason", "@16@0:8", addr(e_reason), false},
        {"userInfo", "@16@0:8", addr(e_userInfo), false},
        {"description", "@16@0:8", addr(e_description), false},
    });
}

}  // namespace orvyn::foundation

namespace orvyn::objc {
void add_exception_symbols(std::vector<libsystem::HostSymbol>& t) {
    t.push_back(libsystem::HostSymbol{"_objc_exception_throw", reinterpret_cast<void*>(&foundation::w_objc_exception_throw), false, false, false, libsystem::Lib::Objc});
}
}  // namespace orvyn::objc
