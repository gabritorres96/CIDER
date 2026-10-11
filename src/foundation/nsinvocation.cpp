// Message forwarding (ORVYN 0.5.4): +resolveInstanceMethod: / +resolveClassMethod:, -forwardingTargetForSelector:, -forwardInvocation: with NSMethodSignature and NSInvocation.
//
// When a message finds no method, the dispatch stub jumps to objc_forward_message (a RAW thunk: it receives every argument register and the stack-argument area):
//   1. -forwardingTargetForSelector: returns another receiver: the call is replayed on it with the original registers (and stack arguments);
//   2. -methodSignatureForSelector: returns a signature: an NSInvocation capturing the arguments is passed to -forwardInvocation:, and its return value becomes
//      the result of the original message;
//   3. otherwise the guest ends with "unrecognized selector sent to instance|class".
// Supported argument/return types: integers, BOOL, pointers, objects, classes, selectors, float and double, void. NOT supported (the forwarding ends the guest
// with a message naming the type): structures and unions by value, arrays, bit fields, long double, variadic arguments beyond the declared ones.
#include <cstring>
#include <format>

#include "orvyn/abi.hpp"
#include "orvyn/libsystem.hpp"
#include "orvyn/objc.hpp"
#include "../libsystem/internal.hpp"
#include "foundation_internal.hpp"

namespace orvyn::objc {
void objc_forward_message(abi::GuestRegs* r);      // referenced by the Runtime constructor
}

namespace orvyn::foundation {
namespace {

using abi::GuestRegs;

struct Slot {
    char code = 'v';
    unsigned size = 0;
    bool is_signed = false, is_float = false;     // float: 'f' (4 bytes) or 'd' (8 bytes)
};

// "v24@0:8@16" -> return + arguments. Returns false (with `why`) for unsupported encodings.
bool parse_encoding(const std::string& types, Slot& ret, std::vector<Slot>& args, std::string& why) {
    size_t i = 0;
    std::vector<Slot> all;
    while (i < types.size()) {
        while (i < types.size() && std::strchr("rnNoORV", types[i])) ++i;       // qualifiers
        if (i >= types.size()) break;
        Slot s;
        s.code = types[i];
        switch (s.code) {
            case 'c': s.size = 1; s.is_signed = true; break;
            case 'C': case 'B': s.size = 1; break;
            case 's': s.size = 2; s.is_signed = true; break;
            case 'S': s.size = 2; break;
            case 'i': s.size = 4; s.is_signed = true; break;
            case 'I': s.size = 4; break;
            case 'l': case 'q': s.size = 8; s.is_signed = true; break;
            case 'L': case 'Q': s.size = 8; break;
            case '@': case '#': case ':': case '*': case '^': s.size = 8; break;
            case 'f': s.size = 4; s.is_float = true; break;
            case 'd': s.size = 8; s.is_float = true; break;
            case 'v': s.size = 0; break;
            default: why = std::string("the type '") + s.code + "' (structures, unions, arrays, bit fields and long double) is not supported by message forwarding"; return false;
        }
        if (s.code == '@' && i + 1 < types.size() && types[i + 1] == '?') ++i;    // block
        if (s.code == '^') {                                                       // pointer to anything: skip the pointee encoding
            int depth = 0;
            size_t j = i + 1;
            for (; j < types.size(); ++j) {
                const char ch = types[j];
                if (std::strchr("{([", ch)) ++depth;
                else if (std::strchr("})]", ch)) --depth;
                else if (depth == 0 && std::isdigit(static_cast<unsigned char>(ch))) break;
            }
            i = j;
        } else {
            ++i;
        }
        while (i < types.size() && std::isdigit(static_cast<unsigned char>(types[i]))) ++i;   // frame offset
        all.push_back(s);
    }
    if (all.empty()) { why = "empty method signature"; return false; }
    ret = all[0];
    args.assign(all.begin() + 1, all.end());
    return true;
}

struct SignaturePayload : Payload {
    std::string types;
    Slot ret;
    std::vector<Slot> args;
    std::vector<std::string> arg_types;          // per-argument encodings (for getArgumentTypeAtIndex:)
    std::string ret_type;
    bool supported = true;
    std::string why;
};
struct InvocationPayload : Payload {
    Addr signature = 0;                          // retained NSMethodSignature
    Addr target = 0;                             // not retained
    Addr selector = 0;
    std::vector<uint64_t> args;                  // one 8-byte slot per argument (index 0 = self, 1 = _cmd)
    uint64_t ret = 0;
    std::vector<uint64_t> stack;                 // stack-passed arguments of the original call (replayed verbatim)
};

Class *g_sig_class = nullptr, *g_inv_class = nullptr;

SignaturePayload* sig_of(Addr o) {
    uint64_t* slot = payload_slot(o);
    if (!slot || !is_kind(o, g_sig_class)) return nullptr;
    if (!*slot) *slot = store_add(std::make_unique<SignaturePayload>());
    return dynamic_cast<SignaturePayload*>(store_get(*slot));
}
InvocationPayload* inv_of(Addr o) {
    uint64_t* slot = payload_slot(o);
    if (!slot || !is_kind(o, g_inv_class)) return nullptr;
    if (!*slot) *slot = store_add(std::make_unique<InvocationPayload>());
    return dynamic_cast<InvocationPayload*>(store_get(*slot));
}

void split_types(const std::string& types, SignaturePayload& p) {
    p.types = types;
    p.supported = parse_encoding(types, p.ret, p.args, p.why);
    // per-argument substrings for getArgumentTypeAtIndex: (qualifiers dropped, offsets dropped)
    size_t i = 0;
    std::vector<std::string> parts;
    while (i < types.size()) {
        size_t j = i;
        while (j < types.size() && std::strchr("rnNoORV", types[j])) ++j;
        size_t k = j + 1;
        if (j < types.size() && (types[j] == '{' || types[j] == '(' || types[j] == '[')) {
            int depth = 0;
            for (k = j; k < types.size(); ++k) { if (std::strchr("{([", types[k])) ++depth; else if (std::strchr("})]", types[k])) { if (--depth == 0) { ++k; break; } } }
        } else if (j < types.size() && (types[j] == '^' || types[j] == '@')) {
            k = j + 1;
            while (k < types.size() && (types[k] == '^' || types[k] == '?')) ++k;
            if (k < types.size() && types[j] == '^' && !std::isdigit(static_cast<unsigned char>(types[k]))) ++k;
        }
        parts.push_back(types.substr(j, k - j));
        i = k;
        while (i < types.size() && std::isdigit(static_cast<unsigned char>(types[i]))) ++i;
    }
    if (!parts.empty()) { p.ret_type = parts[0]; p.arg_types.assign(parts.begin() + 1, parts.end()); }
}

Addr make_signature(const std::string& types) {
    const Addr o = rt().create_instance(g_sig_class, 0);
    if (!o) return 0;
    SignaturePayload* p = sig_of(o);
    if (!p) { rt().release(o); return 0; }
    split_types(types, *p);
    return o;
}

// ---- NSMethodSignature ----------------------------------------------------------------------------------------------------------------------
Addr s_signatureWithObjCTypes(Addr, Addr, Addr cstr) {
    std::string t;
    if (!rt().read_cstring(cstr, t, 4096) || t.empty()) { raise("NSInvalidArgumentException", "+[NSMethodSignature signatureWithObjCTypes:]: invalid type string"); return 0; }
    const Addr o = make_signature(t);
    return o ? autoreleased(o) : 0;
}
uint64_t s_numberOfArguments(Addr self, Addr) { SignaturePayload* p = sig_of(self); return p ? p->arg_types.size() : 0; }
Addr s_getArgumentTypeAtIndex(Addr self, Addr, uint64_t idx) {
    SignaturePayload* p = sig_of(self);
    if (!p || idx >= p->arg_types.size()) { raise("NSRangeException", "-[NSMethodSignature getArgumentTypeAtIndex:]: index out of range"); return 0; }
    return reinterpret_cast<Addr>(p->arg_types[idx].c_str());
}
Addr s_methodReturnType(Addr self, Addr) { SignaturePayload* p = sig_of(self); return p ? reinterpret_cast<Addr>(p->ret_type.c_str()) : 0; }
uint64_t s_methodReturnLength(Addr self, Addr) { SignaturePayload* p = sig_of(self); return p ? p->ret.size : 0; }
Addr s_dealloc(Addr self, Addr) {
    if (uint64_t* slot = payload_slot(self)) { store_drop(*slot); *slot = 0; }
    rt().destruct_instance(self);
    rt().dispose_instance(self);
    return 0;
}

// ---- invocation helpers ----------------------------------------------------------------------------------------------------------------------------
uint64_t extend(const Slot& s, uint64_t raw) {
    switch (s.size) {
        case 1: return s.is_signed ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(raw))) : (raw & 0xff);
        case 2: return s.is_signed ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(raw))) : (raw & 0xffff);
        case 4: return s.is_signed ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(raw))) : (raw & 0xffffffffu);
        default: return raw;
    }
}

// Calls `imp` with the captured arguments. Returns false when the call was refused.
bool replay(Addr imp, const SignaturePayload& sig, const std::vector<uint64_t>& args, const std::vector<uint64_t>& stack, abi::CallOutcome& out) {
    abi::GuestCallArgs a;
    unsigned ints = 0, flts = 0;
    for (size_t i = 0; i < args.size(); ++i) {
        const Slot s = i < 2 ? Slot{i == 0 ? '@' : ':', 8, false, false} : sig.args[i];
        if (s.is_float) {
            if (s.size == 4) { float f; std::memcpy(&f, &args[i], 4); a.push_float(f); } else { double d; std::memcpy(&d, &args[i], 8); a.push_double(d); }
            ++flts;
        } else {
            a.push_int(extend(s, args[i]));
            ++ints;
        }
    }
    (void)stack;
    out = abi::call_guest_nested(imp, a);
    return !out.refused;
}

void store_result(const Slot& ret, const abi::CallOutcome& o, uint64_t& slot) {
    if (ret.code == 'v') return;
    if (ret.is_float) slot = ret.size == 4 ? (o.xmm0 & 0xffffffffu) : o.xmm0;
    else slot = extend(ret, o.value);
}

// ---- NSInvocation -----------------------------------------------------------------------------------------------------------------------------------
Addr i_invocationWithMethodSignature(Addr, Addr, Addr sig) {
    SignaturePayload* sp = sig_of(sig);
    if (!sp) { raise("NSInvalidArgumentException", "+[NSInvocation invocationWithMethodSignature:]: argument is not an NSMethodSignature"); return 0; }
    if (!sp->supported) { raise("NSInvalidArgumentException", "+[NSInvocation invocationWithMethodSignature:]: " + sp->why); return 0; }
    const Addr o = rt().create_instance(g_inv_class, 0);
    InvocationPayload* p = o ? inv_of(o) : nullptr;
    if (!p) return 0;
    p->signature = rt().retain(sig);
    p->args.assign(sp->args.size(), 0);
    return autoreleased(o);
}
Addr i_dealloc(Addr self, Addr) {
    if (InvocationPayload* p = inv_of(self)) if (p->signature) { rt().release(p->signature); p->signature = 0; }
    if (uint64_t* slot = payload_slot(self)) { store_drop(*slot); *slot = 0; }
    rt().destruct_instance(self);
    rt().dispose_instance(self);
    return 0;
}
Addr i_target(Addr self, Addr) { InvocationPayload* p = inv_of(self); return p ? p->target : 0; }
Addr i_setTarget(Addr self, Addr, Addr t) { if (InvocationPayload* p = inv_of(self)) { p->target = t; if (!p->args.empty()) p->args[0] = t; } return 0; }
Addr i_selector(Addr self, Addr) { InvocationPayload* p = inv_of(self); return p ? p->selector : 0; }
Addr i_setSelector(Addr self, Addr, Addr s) { if (InvocationPayload* p = inv_of(self)) { p->selector = s; if (p->args.size() > 1) p->args[1] = s; } return 0; }
Addr i_methodSignature(Addr self, Addr) { InvocationPayload* p = inv_of(self); return p ? p->signature : 0; }
Addr i_getArgument(Addr self, Addr, Addr buffer, int64_t idx) {
    InvocationPayload* p = inv_of(self);
    SignaturePayload* sp = p ? sig_of(p->signature) : nullptr;
    if (!p || !sp) return 0;
    if (idx < 0 || static_cast<size_t>(idx) >= p->args.size()) { raise("NSInvalidArgumentException", std::format("-[NSInvocation getArgument:atIndex:]: index {} out of bounds", idx)); return 0; }
    const Slot& s = sp->args[static_cast<size_t>(idx)];
    if (!rt().readable(buffer, s.size)) { raise("NSInvalidArgumentException", "-[NSInvocation getArgument:atIndex:]: the buffer is not writable"); return 0; }
    std::memcpy(reinterpret_cast<void*>(buffer), &p->args[static_cast<size_t>(idx)], s.size);
    return 0;
}
Addr i_setArgument(Addr self, Addr, Addr buffer, int64_t idx) {
    InvocationPayload* p = inv_of(self);
    SignaturePayload* sp = p ? sig_of(p->signature) : nullptr;
    if (!p || !sp) return 0;
    if (idx < 0 || static_cast<size_t>(idx) >= p->args.size()) { raise("NSInvalidArgumentException", std::format("-[NSInvocation setArgument:atIndex:]: index {} out of bounds", idx)); return 0; }
    const Slot& s = sp->args[static_cast<size_t>(idx)];
    if (!rt().readable(buffer, s.size)) { raise("NSInvalidArgumentException", "-[NSInvocation setArgument:atIndex:]: the buffer is not readable"); return 0; }
    uint64_t v = 0;
    std::memcpy(&v, reinterpret_cast<void*>(buffer), s.size);
    p->args[static_cast<size_t>(idx)] = v;
    if (idx == 0) p->target = v;
    if (idx == 1) p->selector = v;
    return 0;
}
Addr i_getReturnValue(Addr self, Addr, Addr buffer) {
    InvocationPayload* p = inv_of(self);
    SignaturePayload* sp = p ? sig_of(p->signature) : nullptr;
    if (!p || !sp || sp->ret.size == 0) return 0;
    if (!rt().readable(buffer, sp->ret.size)) { raise("NSInvalidArgumentException", "-[NSInvocation getReturnValue:]: the buffer is not writable"); return 0; }
    std::memcpy(reinterpret_cast<void*>(buffer), &p->ret, sp->ret.size);
    return 0;
}
Addr i_setReturnValue(Addr self, Addr, Addr buffer) {
    InvocationPayload* p = inv_of(self);
    SignaturePayload* sp = p ? sig_of(p->signature) : nullptr;
    if (!p || !sp || sp->ret.size == 0) return 0;
    if (!rt().readable(buffer, sp->ret.size)) { raise("NSInvalidArgumentException", "-[NSInvocation setReturnValue:]: the buffer is not readable"); return 0; }
    p->ret = 0;
    std::memcpy(&p->ret, reinterpret_cast<void*>(buffer), sp->ret.size);
    return 0;
}
Addr i_invokeWithTarget(Addr self, Addr, Addr target) {
    InvocationPayload* p = inv_of(self);
    SignaturePayload* sp = p ? sig_of(p->signature) : nullptr;
    if (!p || !sp) return 0;
    if (!target) { raise("NSInvalidArgumentException", "-[NSInvocation invokeWithTarget:]: the target is nil"); return 0; }
    Class* c = rt().class_of_object(target);
    const Addr imp = c ? rt().find_imp(c, p->selector) : 0;
    if (!imp) {
        raise("NSInvalidArgumentException", std::format("-[NSInvocation invoke]: {} does not respond to {}", describe(target), rt().sel_name(p->selector)));
        return 0;
    }
    p->target = target;
    p->args[0] = target;
    abi::CallOutcome out;
    if (!replay(imp, *sp, p->args, p->stack, out)) { raise("NSInternalInconsistencyException", "-[NSInvocation invoke]: " + out.refusal); return 0; }
    store_result(sp->ret, out, p->ret);
    return 0;
}
Addr i_invoke(Addr self, Addr) { InvocationPayload* p = inv_of(self); return p ? i_invokeWithTarget(self, 0, p->target) : 0; }

// ---- NSObject defaults ---------------------------------------------------------------------------------------------------------------------------------
uint64_t o_resolveMethod(Addr, Addr, Addr) { return 0; }
Addr o_forwardingTargetForSelector(Addr, Addr, Addr) { return 0; }
Addr o_methodSignatureForSelector(Addr self, Addr, Addr s) {
    Class* c = rt().class_of_object(self);
    if (!c) return 0;
    objc::Method* m = rt().find_method(c, s);
    if (!m || m->types.empty()) return 0;
    const Addr o = make_signature(m->types);
    return o ? autoreleased(o) : 0;
}
Addr o_class_methodSignatureForSelector(Addr self, Addr cmd, Addr s) { return o_methodSignatureForSelector(self, cmd, s); }
std::string unrecognized(Addr self, Addr s) {
    Class* c = rt().class_of_object(self);
    return std::format("objc: {}[{} {}]: unrecognized selector sent to {} {:#x}", c && c->is_meta ? '+' : '-', c ? (c->is_meta ? c->instance_class->name : c->name) : "?", rt().sel_name(s),
                       c && c->is_meta ? "class" : "instance", self);
}
Addr o_doesNotRecognizeSelector(Addr self, Addr, Addr s) { libsystem::detail::trap(unrecognized(self, s)); return 0; }
Addr o_forwardInvocation(Addr self, Addr, Addr inv) {
    InvocationPayload* p = inv_of(inv);
    libsystem::detail::trap(unrecognized(self, p ? p->selector : 0));
    return 0;
}

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

void install_nsinvocation(Runtime& r) {
    Class* sig = r.declare_native_class("NSMethodSignature", "NSObject", 16);
    g_sig_class = sig;
    classes().extra_payload.push_back(sig);
    r.add_native_methods(sig, true, {{"signatureWithObjCTypes:", "@24@0:8r*16", addr(s_signatureWithObjCTypes), false}});
    r.add_native_methods(sig, false, {
        {"dealloc", "v16@0:8", addr(s_dealloc), false},
        {"numberOfArguments", "Q16@0:8", addr(s_numberOfArguments), false},
        {"getArgumentTypeAtIndex:", "r*24@0:8Q16", addr(s_getArgumentTypeAtIndex), false},
        {"methodReturnType", "r*16@0:8", addr(s_methodReturnType), false},
        {"methodReturnLength", "Q16@0:8", addr(s_methodReturnLength), false},
    });
    Class* inv = r.declare_native_class("NSInvocation", "NSObject", 16);
    g_inv_class = inv;
    classes().extra_payload.push_back(inv);
    r.add_native_methods(inv, true, {{"invocationWithMethodSignature:", "@24@0:8@16", addr(i_invocationWithMethodSignature), false}});
    r.add_native_methods(inv, false, {
        {"dealloc", "v16@0:8", addr(i_dealloc), false},
        {"target", "@16@0:8", addr(i_target), false},
        {"setTarget:", "v24@0:8@16", addr(i_setTarget), false},
        {"selector", ":16@0:8", addr(i_selector), false},
        {"setSelector:", "v24@0:8:16", addr(i_setSelector), false},
        {"methodSignature", "@16@0:8", addr(i_methodSignature), false},
        {"getArgument:atIndex:", "v32@0:8^v16q24", addr(i_getArgument), false},
        {"setArgument:atIndex:", "v32@0:8^v16q24", addr(i_setArgument), false},
        {"getReturnValue:", "v24@0:8^v16", addr(i_getReturnValue), false},
        {"setReturnValue:", "v24@0:8^v16", addr(i_setReturnValue), false},
        {"invoke", "v16@0:8", addr(i_invoke), false},
        {"invokeWithTarget:", "v24@0:8@16", addr(i_invokeWithTarget), false},
    });
    Class* o = classes().object;
    r.add_native_methods(o, true, {
        {"resolveInstanceMethod:", "B24@0:8:16", addr(o_resolveMethod), false},
        {"resolveClassMethod:", "B24@0:8:16", addr(o_resolveMethod), false},
        {"methodSignatureForSelector:", "@24@0:8:16", addr(o_class_methodSignatureForSelector), false},
        {"doesNotRecognizeSelector:", "v24@0:8:16", addr(o_doesNotRecognizeSelector), false},
    });
    r.add_native_methods(o, false, {
        {"forwardingTargetForSelector:", "@24@0:8:16", addr(o_forwardingTargetForSelector), false},
        {"methodSignatureForSelector:", "@24@0:8:16", addr(o_methodSignatureForSelector), false},
        {"forwardInvocation:", "v24@0:8@16", addr(o_forwardInvocation), false},
        {"doesNotRecognizeSelector:", "v24@0:8:16", addr(o_doesNotRecognizeSelector), false},
    });
}

}  // namespace orvyn::foundation

namespace orvyn::objc {

// The raw thunk behind forward_imp_: see the file comment.
void objc_forward_message(abi::GuestRegs* r) {
    Runtime* rt = Runtime::current();
    if (!rt) return;
    const Addr self = r->gp[0], s = r->gp[1];
    auto finish_trap = [&] {
        Class* c = rt->class_of_object(self);
        libsystem::detail::trap(std::format("objc: {}[{} {}]: unrecognized selector sent to {} {:#x}", c && c->is_meta ? '+' : '-', c ? (c->is_meta ? c->instance_class->name : c->name) : "?",
                                            rt->sel_name(s), c && c->is_meta ? "class" : "instance", self));
    };
    Class* c = rt->class_of_object(self);
    if (!c || !rt->sel_name(s).size()) { finish_trap(); return; }

    // 1. -forwardingTargetForSelector:
    const Addr fts = rt->sel("forwardingTargetForSelector:");
    if (!c->is_meta && rt->find_imp(c, fts)) {
        const Addr target = rt->send(self, fts, {s});
        if (target && target != self) {
            Class* tc = rt->class_of_object(target);
            Method* m = tc ? rt->find_method(tc, s) : nullptr;
            if (m && m->imp) {
                foundation::SignaturePayload sig;
                std::vector<uint64_t> args;
                std::string why;
                foundation::split_types(m->types, sig);
                if (!sig.supported) { libsystem::detail::trap("objc: forwarding target: " + sig.why); return; }
                for (int i = 0; i < 6; ++i) args.push_back(r->gp[i]);
                args.resize(2 + sig.args.size() > 6 ? 6 : 2 + sig.args.size());
                abi::GuestCallArgs a;
                // replay every register class exactly as received, with the new receiver
                a.push_int(target);
                for (int i = 1; i < 6; ++i) a.push_int(r->gp[i]);
                for (int i = 0; i < 8; ++i) { double d; std::memcpy(&d, &r->xmm[i], 8); a.push_double(d); }
                unsigned ints = 0, flts = 0;
                for (size_t i = 0; i < sig.args.size(); ++i) (sig.args[i].is_float ? flts : ints)++;
                ints += 2;
                if (ints > 6 || flts > 8) {
                    // stack-passed arguments: copy the slots the target's signature needs (one 8-byte slot each, in call order)
                    const unsigned need = (ints > 6 ? ints - 6 : 0) + (flts > 8 ? flts - 8 : 0);
                    if (need > 64) { libsystem::detail::trap("objc: forwarding target: too many stack arguments"); return; }
                    const auto* stack = reinterpret_cast<const uint64_t*>(r->stack_args);
                    for (unsigned i = 0; i < need; ++i) a.stack.push_back(stack[i]);
                }
                a.variadic_ = false;
                abi::CallOutcome o = abi::call_guest_nested(m->imp, a);
                if (o.refused) { libsystem::detail::trap("objc: forwarding target call refused: " + o.refusal); return; }
                r->ret_rax = o.value; r->ret_rdx = o.rdx; r->ret_xmm0 = o.xmm0; r->ret_xmm1 = o.xmm1;
                return;
            }
        }
    }

    // 2. -methodSignatureForSelector: + -forwardInvocation:
    const Addr sigsel = rt->sel("methodSignatureForSelector:"), fisel = rt->sel("forwardInvocation:");
    if (!c->is_meta && rt->find_imp(c, sigsel) && rt->find_imp(c, fisel)) {
        const Addr sig_obj = rt->send(self, sigsel, {s});
        foundation::SignaturePayload* sp = sig_obj ? foundation::sig_of(sig_obj) : nullptr;
        if (sp) {
            if (!sp->supported) { libsystem::detail::trap("objc: forwardInvocation: " + sp->why); return; }
            const Addr inv = rt->create_instance(foundation::g_inv_class, 0);
            foundation::InvocationPayload* ip = inv ? foundation::inv_of(inv) : nullptr;
            if (!ip) return;
            ip->signature = rt->retain(sig_obj);
            ip->target = self;
            ip->selector = s;
            ip->args.assign(sp->args.size(), 0);
            unsigned ints = 0, flts = 0;
            for (size_t i = 0; i < sp->args.size(); ++i) {
                if (i == 0) { ip->args[0] = self; ++ints; continue; }
                if (i == 1) { ip->args[1] = s; ++ints; continue; }
                if (sp->args[i].is_float) { ip->args[i] = r->xmm[flts < 8 ? flts : 0]; ++flts; }
                else if (ints < 6) { ip->args[i] = foundation::extend(sp->args[i], r->gp[ints]); ++ints; }
                else { libsystem::detail::trap("objc: forwardInvocation: stack-passed arguments are not supported"); rt->release(inv); return; }
            }
            rt->send(self, fisel, {inv});
            const foundation::Slot ret = sp->ret;
            if (ret.code != 'v') {
                if (ret.is_float) r->ret_xmm0 = ret.size == 4 ? (ip->ret & 0xffffffffu) : ip->ret;
                else r->ret_rax = foundation::extend(ret, ip->ret);
            }
            rt->release(inv);
            return;
        }
    }
    finish_trap();                                              // 3.
}

}  // namespace orvyn::objc
