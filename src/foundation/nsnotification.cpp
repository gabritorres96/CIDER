// NSNotification and NSNotificationCenter (CIDER 0.5): selector- and block-based observers, synchronous delivery on the posting thread.
// Observers are NOT retained (as in Cocoa); blocks are copied. Not implemented: operation queues (the queue argument is ignored: blocks run on the
// posting thread), notification coalescing, distributed notifications.
#include <algorithm>

#include "cider/libsystem.hpp"
#include "cider/objc.hpp"
#include "../libsystem/internal.hpp"
#include "foundation_internal.hpp"

namespace cider::foundation {
namespace {

struct NotificationPayload : Payload { Addr name = 0, object = 0, userinfo = 0; };
struct Observer {
    uint64_t token = 0;
    Addr observer = 0;          // weak
    Addr selector = 0;
    Addr name = 0;              // retained NSString or 0 (any name)
    Addr object = 0;            // NOT retained (Cocoa does not retain the sender filter), or 0 (any sender)
    Addr block = 0;             // retained heap block (block-based observers)
};

Class* g_cls_notification = nullptr;
Class* g_cls_center = nullptr;
Addr g_center = 0;
std::vector<Observer> g_observers;
uint64_t g_next_token = 1;
std::unordered_map<Addr, uint64_t> g_tokens;     // token object -> observer token

NotificationPayload* note_of(Addr obj) {
    uint64_t* slot = payload_slot(obj);
    if (!slot || !is_kind(obj, g_cls_notification)) return nullptr;
    if (!*slot) *slot = store_add(std::make_unique<NotificationPayload>());
    return dynamic_cast<NotificationPayload*>(store_get(*slot));
}
void set_retained(Addr& slot, Addr value) {
    if (value) rt().retain(value);
    const Addr old = slot;
    slot = value;
    if (old) rt().release(old);
}

Addr make_notification(Addr name, Addr object, Addr userinfo) {
    const Addr o = rt().create_instance(g_cls_notification, 0);
    if (!o) return 0;
    NotificationPayload* p = note_of(o);
    if (!p) { rt().release(o); return 0; }
    set_retained(p->name, name);
    set_retained(p->object, object);
    set_retained(p->userinfo, userinfo);
    return autoreleased(o);
}

Addr n_notificationWithName(Addr, Addr, Addr name, Addr object) { return make_notification(name, object, 0); }
void n_notificationWithNameUserInfo(abi::GuestRegs* r) { r->ret_rax = make_notification(r->gp[2], r->gp[3], r->gp[4]); }
Addr n_dealloc(Addr self, Addr) {
    if (NotificationPayload* p = note_of(self)) {
        const Addr a = p->name, b = p->object, c = p->userinfo;
        p->name = p->object = p->userinfo = 0;
        if (a) rt().release(a);
        if (b) rt().release(b);
        if (c) rt().release(c);
    }
    if (uint64_t* slot = payload_slot(self)) { store_drop(*slot); *slot = 0; }
    rt().destruct_instance(self);
    rt().dispose_instance(self);
    return 0;
}
Addr n_name(Addr self, Addr) { NotificationPayload* p = note_of(self); return p ? p->name : 0; }
Addr n_object(Addr self, Addr) { NotificationPayload* p = note_of(self); return p ? p->object : 0; }
Addr n_userInfo(Addr self, Addr) { NotificationPayload* p = note_of(self); return p ? p->userinfo : 0; }
Addr n_description(Addr self, Addr) {
    NotificationPayload* p = note_of(self);
    return p ? new_string_autoreleased("NSConcreteNotification {name = " + describe(p->name) + "; object = " + describe(p->object) + "}") : 0;
}

bool same_string(Addr a, Addr b) {
    std::string_view x, y;
    return string_view_of(a, x) && string_view_of(b, y) && x == y;
}

// ---- NSNotificationCenter ------------------------------------------------------------------------------------------------------------------
Addr c_defaultCenter(Addr, Addr) { return g_center; }
void c_addObserver(abi::GuestRegs* r) {                      // addObserver:(id) selector:(SEL) name:(NSString*) object:(id)
    Observer o;
    o.token = g_next_token++;
    o.observer = r->gp[2];
    o.selector = r->gp[3];
    if (!o.observer || !o.selector) { raise("NSInvalidArgumentException", "-[NSNotificationCenter addObserver:selector:name:object:]: observer and selector must not be nil"); return; }
    set_retained(o.name, r->gp[4]);
    o.object = r->gp[5];
    g_observers.push_back(o);
}
void c_addObserverForName(abi::GuestRegs* r) {               // addObserverForName:(NSString*) object:(id) queue:(id) usingBlock:(void(^)(NSNotification*))
    const Addr block = r->gp[5];
    if (!block || !rt().readable(block, 24)) { raise("NSInvalidArgumentException", "-[NSNotificationCenter addObserverForName:object:queue:usingBlock:]: the block is nil"); return; }
    Observer o;
    o.token = g_next_token++;
    set_retained(o.name, r->gp[2]);
    o.object = r->gp[3];
    o.block = libsystem::detail::block_retain(block);
    const Addr tok = rt().create_instance(classes().object, 0);
    if (!tok) return;
    g_tokens[tok] = o.token;
    g_observers.push_back(o);
    r->ret_rax = autoreleased(tok);
}
void drop_observer(const Observer& o) {
    if (o.name) rt().release(o.name);
    if (o.block) libsystem::detail::block_release(o.block);
}
Addr c_removeObserver(Addr, Addr, Addr who) {
    auto tok = g_tokens.find(who);
    for (size_t i = 0; i < g_observers.size();) {
        const bool match = (tok != g_tokens.end() && g_observers[i].token == tok->second) || (tok == g_tokens.end() && g_observers[i].observer == who && !g_observers[i].block);
        if (match) { drop_observer(g_observers[i]); g_observers.erase(g_observers.begin() + static_cast<ptrdiff_t>(i)); } else ++i;
    }
    if (tok != g_tokens.end()) g_tokens.erase(tok);
    return 0;
}
void deliver(Addr notification) {
    NotificationPayload* p = note_of(notification);
    if (!p) return;
    const Addr name = p->name, object = p->object;
    const std::vector<Observer> snapshot = g_observers;            // observers may add/remove observers while being called
    for (const Observer& o : snapshot) {
        if (o.name && !same_string(o.name, name)) continue;
        if (o.object && o.object != object) continue;
        // the observer may have been removed by an earlier callback
        if (std::none_of(g_observers.begin(), g_observers.end(), [&](const Observer& x) { return x.token == o.token; })) continue;
        if (o.block) {
            const uint64_t invoke = rt().readable(o.block, 24) ? *reinterpret_cast<const uint64_t*>(o.block + 16) : 0;
            if (libsystem::is_valid_callback(invoke)) rt().call_imp(invoke, {o.block, notification});
        } else {
            Class* c = rt().class_of_object(o.observer);
            if (c && rt().find_imp(c, o.selector)) rt().send(o.observer, o.selector, {notification});
        }
        if (rt().failed()) return;
    }
}
Addr c_postNotification(Addr, Addr, Addr notification) {
    if (!note_of(notification)) { raise("NSInvalidArgumentException", "-[NSNotificationCenter postNotification:]: argument is not an NSNotification"); return 0; }
    deliver(notification);
    return 0;
}
Addr c_postNotificationName(Addr, Addr, Addr name, Addr object) {
    const Addr n = make_notification(name, object, 0);
    if (n) deliver(n);
    return 0;
}
void c_postNotificationNameUserInfo(abi::GuestRegs* r) {
    const Addr n = make_notification(r->gp[2], r->gp[3], r->gp[4]);
    if (n) deliver(n);
}

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

void post_notification(const std::string& name, Addr object, Addr userinfo) {
    if (!g_cls_notification) return;
    const Addr n_name = new_string(name);
    const Addr n = make_notification(n_name, object, userinfo);
    rt().release(n_name);
    if (n) deliver(n);
}

void install_nsnotification(Runtime& r) {
    g_observers.clear();
    g_tokens.clear();
    Class* n = r.declare_native_class("NSNotification", "NSObject", 16);
    g_cls_notification = n;
    classes().extra_payload.push_back(n);
    r.add_native_methods(n, true, {
        {"notificationWithName:object:", "@32@0:8@16@24", addr(n_notificationWithName), false},
        {"notificationWithName:object:userInfo:", "@40@0:8@16@24@32", addr(n_notificationWithNameUserInfo), true},
    });
    r.add_native_methods(n, false, {
        {"dealloc", "v16@0:8", addr(n_dealloc), false},
        {"name", "@16@0:8", addr(n_name), false},
        {"object", "@16@0:8", addr(n_object), false},
        {"userInfo", "@16@0:8", addr(n_userInfo), false},
        {"description", "@16@0:8", addr(n_description), false},
    });
    Class* c = r.declare_native_class("NSNotificationCenter", "NSObject", 16);
    g_cls_center = c;
    g_center = r.arena_alloc(16, 16);
    reinterpret_cast<Addr*>(g_center)[0] = c->addr;
    reinterpret_cast<Addr*>(g_center)[1] = 0;
    r.add_native_methods(c, true, {{"defaultCenter", "@16@0:8", addr(c_defaultCenter), false}});
    r.add_native_methods(c, false, {
        {"addObserver:selector:name:object:", "v48@0:8@16:24@32@40", addr(c_addObserver), true},
        {"addObserverForName:object:queue:usingBlock:", "@48@0:8@16@24@32@?40", addr(c_addObserverForName), true},
        {"removeObserver:", "v24@0:8@16", addr(c_removeObserver), false},
        {"postNotification:", "v24@0:8@16", addr(c_postNotification), false},
        {"postNotificationName:object:", "v32@0:8@16@24", addr(c_postNotificationName), false},
        {"postNotificationName:object:userInfo:", "v40@0:8@16@24@32", addr(c_postNotificationNameUserInfo), true},
    });
}

}  // namespace cider::foundation
