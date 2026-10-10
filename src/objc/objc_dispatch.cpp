// Message dispatch. objc_msgSend itself is machine code (objc_msgsend.asm) because it must TAIL-JUMP to the method implementation with
// every argument register, the stack arguments and AL intact (a Windows-ABI thunk that calls and returns cannot do that). The stub
// calls lookup_for_message(self, _cmd) through a thunk, then jumps to the IMP it returns.
#include <format>

#include "cider/libsystem.hpp"
#include "cider/log.hpp"
#include "cider/objc.hpp"
#include "../libsystem/internal.hpp"

namespace cider::objc {

Addr Runtime::lookup_for_message(Addr self, Addr s) {
    ++messages_sent;
    ++lookups;
    Class* c = class_of_object(self);
    if (!c) {
        libsystem::detail::trap(std::format("objc: message {} sent to an invalid object {:#x} (its isa is not a registered class)", sel_name(s), self));
        return forward_imp_;
    }
    const Addr imp = find_imp(c, s);
    if (imp) return imp;
    return forward_imp_;                       // traps with "unrecognized selector sent to instance|class"
}

Addr Runtime::lookup_for_super(Addr super_struct, Addr s, int mode) {
    ++messages_sent;
    ++lookups;
    // struct objc_super { id receiver; Class super_class; }  (mode 1)   or  { id receiver; Class current_class; } (mode 2: search starts in its superclass)
    if (!readable(super_struct, 16)) {
        libsystem::detail::trap("objc: objc_msgSendSuper with an invalid objc_super structure");
        return forward_imp_;
    }
    const Addr receiver = reinterpret_cast<const Addr*>(super_struct)[0];
    const Addr cls = reinterpret_cast<const Addr*>(super_struct)[1];
    if (!receiver) return nil_imp_;
    Class* c = class_at(cls);
    if (!c) {
        libsystem::detail::trap(std::format("objc: objc_msgSendSuper with an unknown class {:#x}", cls));
        return forward_imp_;
    }
    if (mode == 2) c = c->super;                  // objc_msgSendSuper2: lookup starts above the class that contains the calling method
    if (!c) return forward_imp_;
    const Addr imp = find_imp(c, s);
    return imp ? imp : forward_imp_;
}

}  // namespace cider::objc
