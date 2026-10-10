// NSWindow (CIDER 0.5.7). The native window is created lazily, the first time the window is ordered front (or needs a native handle).
// Frames are in AppKit screen space (origin bottom-left of the primary display, y up); the backend converts to Win32 pixels.
#include <algorithm>

#include "cider/libsystem.hpp"
#include "../libsystem/internal.hpp"
#include "appkit_internal.hpp"
#include "appkit_install.hpp"

namespace cider::appkit {
namespace {

using abi::GuestRegs;
using graphics::sysv::Args;

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

void set_retained(Addr& slot, Addr value) {
    if (value) rt().retain(value);
    const Addr old = slot;
    slot = value;
    if (old) rt().release(old);
}
std::string str_of(Addr s) {
    std::string_view sv;
    return s && fnd::string_view_of(s, sv) ? std::string(sv) : std::string();
}

}  // namespace

void ensure_window_native(Addr window) {
    WindowPayload* w = W(window);
    if (!w || w->native || w->closing || !ensure_backend()) return;
    gui::WindowSpec spec;
    spec.style_mask = w->style_mask;
    spec.title = str_of(w->title);
    spec.content_rect = w->content_rect;
    w->native = st().gui->create_window(spec);
    if (!w->native) { libsystem::detail::trap("NSWindow: the native window could not be created"); return; }
    st().window_of_native[w->native] = window;
    if (w->min_size.width > 0 || w->min_size.height > 0) st().gui->set_min_content_size(w->native, w->min_size);
    if (w->content_view) {
        ViewPayload* cv = V(w->content_view);
        const Size client = st().gui->client_size(w->native);
        w->content_rect.size = client;
        if (cv) { view_set_window(w->content_view, window); cv->frame = geometry::make_rect(0, 0, client.width, client.height); }
        view_build_native(w->content_view);
    }
}

void set_content_view(Addr window, Addr view) {
    WindowPayload* w = W(window);
    if (!w) return;
    if (view && !V(view)) { fnd::raise("NSInvalidArgumentException", "-[NSWindow setContentView:]: argument is not an NSView"); return; }
    if (view == w->content_view) return;
    const Addr old = w->content_view;
    w->content_view = 0;
    if (old) {
        view_set_window(old, 0);
        rt().release(old);
    }
    if (!view) return;
    ViewPayload* v = V(view);
    if (v->superview) view_detach(view);
    rt().retain(view);
    w->content_view = view;
    v->superview = 0;
    const Size size = w->native && st().gui ? st().gui->client_size(w->native) : w->content_rect.size;
    v->frame = geometry::make_rect(0, 0, size.width, size.height);
    view_set_window(view, window);
    if (w->native) view_build_native(view);
}

void window_set_content_size(Addr window, Size size) {
    WindowPayload* w = W(window);
    if (!w) return;
    const Size before = w->content_rect.size;
    w->content_rect.size = size;
    if (w->native && st().gui) {
        w->in_native_resize = true;
        st().gui->set_content_rect(w->native, w->content_rect);
        w->in_native_resize = false;
        const Size real = st().gui->client_size(w->native);
        w->content_rect.size = real;
        if (w->content_view) view_set_frame(w->content_view, geometry::make_rect(0, 0, real.width, real.height));
    } else if (w->content_view) {
        view_set_frame(w->content_view, geometry::make_rect(0, 0, size.width, size.height));
    }
    const Size after = w->content_rect.size;
    if ((before.width != after.width || before.height != after.height) && w->delegate && responds(w->delegate, sel("windowDidResize:"))) {
        const Addr n = fnd::new_string("NSWindowDidResizeNotification");
        const Addr note = rt().send(rt().class_named("NSNotification")->addr, "notificationWithName:object:", {n, window});
        rt().release(n);
        rt().send(w->delegate, sel("windowDidResize:"), {note});
    }
}

Rect window_frame(Addr window) {
    WindowPayload* w = W(window);
    if (!w) return geometry::make_rect(0, 0, 0, 0);
    if (w->native && st().gui) return st().gui->frame_rect(w->native);
    return w->content_rect;                                              // no chrome until the native window exists
}

namespace {

// ---- creation ----------------------------------------------------------------------------------------------------------------------------------------
void w_init(GuestRegs* r) {                                              // initWithContentRect:styleMask:backing:defer:
    Args a(r);
    const Addr self = a.next_int();
    a.next_int();
    const Rect rect = a.next_rect();
    const uint64_t style = a.next_int();
    a.next_int();                                                        // backing store type: always buffered
    const bool defer = (a.next_int() & 0xff) != 0;
    WindowPayload* w = W(self);
    if (!w) { fnd::raise("NSInvalidArgumentException", "-[NSWindow initWithContentRect:...]: receiver is not an NSWindow"); return; }
    if (!shared_application()) return;
    w->content_rect = geometry::standardize(rect);
    w->style_mask = static_cast<unsigned>(style);
    w->defer = defer;
    AppPayload* app = A(st().app);
    rt().retain(self);                                                   // the application keeps every open window alive
    w->app_holds_reference = true;
    app->windows.push_back(self);
    const Addr content = rt().create_instance(st().view, 0);                // every window starts with an empty content view
    if (content) {
        if (ViewPayload* cv = V(content)) cv->frame = geometry::make_rect(0, 0, w->content_rect.size.width, w->content_rect.size.height);
        set_content_view(self, content);
        rt().release(content);
    }
    r->ret_rax = self;
}
Addr w_dealloc(Addr self, Addr) {
    if (WindowPayload* w = W(self)) {
        if (w->native) {
            st().window_of_native.erase(w->native);
            if (st().gui) st().gui->destroy(w->native);
            w->native = 0;
        }
        if (AppPayload* app = A(st().app)) {
            app->windows.erase(std::remove(app->windows.begin(), app->windows.end(), self), app->windows.end());
            if (app->key_window == self) app->key_window = 0;
            if (app->main_window == self) app->main_window = 0;
        }
        if (w->content_view) { const Addr cv = w->content_view; w->content_view = 0; view_set_window(cv, 0); rt().release(cv); }
        if (w->title) { rt().release(w->title); w->title = 0; }
    }
    if (uint64_t* slot = fnd::payload_slot(self)) { fnd::store_drop(*slot); *slot = 0; }
    rt().destruct_instance(self);
    rt().dispose_instance(self);
    return 0;
}

// ---- properties ----------------------------------------------------------------------------------------------------------------------------------------
Addr w_title(Addr self, Addr) { WindowPayload* w = W(self); return w ? (w->title ? w->title : fnd::new_string_autoreleased("")) : 0; }
Addr w_setTitle(Addr self, Addr, Addr t) {
    WindowPayload* w = W(self);
    if (!w) return 0;
    std::string_view check;
    if (t && !fnd::string_view_of(t, check)) { fnd::raise("NSInvalidArgumentException", "-[NSWindow setTitle:]: argument is not an NSString"); return 0; }
    set_retained(w->title, t);
    if (w->native && st().gui) st().gui->set_window_title(w->native, str_of(t));
    return 0;
}
Addr w_contentView(Addr self, Addr) { WindowPayload* w = W(self); return w ? w->content_view : 0; }
Addr w_setContentView(Addr self, Addr, Addr v) { set_content_view(self, v); return 0; }
void w_frame(GuestRegs* r) {                                              // stret
    Args a(r);
    const uint64_t out = a.next_int();
    const Addr self = a.next_int();
    graphics::sysv::ret_rect(r, out, window_frame(self));
}
void w_setFrameDisplay(GuestRegs* r) {                                    // setFrame:(NSRect) display:(BOOL)
    Args a(r);
    const Addr self = a.next_int();
    a.next_int();
    const Rect f = geometry::standardize(a.next_rect());
    WindowPayload* w = W(self);
    if (!w) return;
    Rect content = f;
    if (w->native && st().gui) {                                          // remove the chrome the backend reports
        const Rect fr = st().gui->frame_rect(w->native), cr = st().gui->content_rect(w->native);
        content = geometry::make_rect(f.origin.x + (cr.min_x() - fr.min_x()), f.origin.y + (cr.min_y() - fr.min_y()),
                                      f.size.width - (fr.size.width - cr.size.width), f.size.height - (fr.size.height - cr.size.height));
    }
    w->content_rect = content;
    if (w->native && st().gui) {
        w->in_native_resize = true;
        st().gui->set_content_rect(w->native, content);
        w->in_native_resize = false;
        w->content_rect = st().gui->content_rect(w->native);
        if (w->content_view) view_set_frame(w->content_view, geometry::make_rect(0, 0, w->content_rect.size.width, w->content_rect.size.height));
    } else if (w->content_view) {
        view_set_frame(w->content_view, geometry::make_rect(0, 0, content.size.width, content.size.height));
    }
}
void w_setContentSize(GuestRegs* r) {
    Args a(r);
    const Addr self = a.next_int();
    a.next_int();
    window_set_content_size(self, a.next_size());
}
void w_setFrameOrigin(GuestRegs* r) {
    Args a(r);
    const Addr self = a.next_int();
    a.next_int();
    const Point p = a.next_point();
    WindowPayload* w = W(self);
    if (!w) return;
    const Rect fr = window_frame(self);
    w->content_rect.origin.x += p.x - fr.origin.x;
    w->content_rect.origin.y += p.y - fr.origin.y;
    if (w->native && st().gui) { st().gui->set_content_rect(w->native, w->content_rect); w->content_rect = st().gui->content_rect(w->native); }
}
Addr w_center(Addr self, Addr) {
    WindowPayload* w = W(self);
    if (!w || !ensure_backend()) return 0;
    if (w->native) {
        st().gui->center(w->native);
        w->content_rect = st().gui->content_rect(w->native);
    } else {
        const Rect screen = st().gui->visible_frame();
        w->content_rect.origin.x = screen.min_x() + (screen.size.width - w->content_rect.size.width) / 2;
        w->content_rect.origin.y = screen.min_y() + (screen.size.height - w->content_rect.size.height) / 2;
    }
    return 0;
}
void w_setMinSize(GuestRegs* r) {
    Args a(r);
    const Addr self = a.next_int();
    a.next_int();
    WindowPayload* w = W(self);
    if (!w) return;
    w->min_size = a.next_size();
    if (w->native && st().gui) st().gui->set_min_content_size(w->native, w->min_size);
}
void w_minSize(GuestRegs* r) { WindowPayload* w = W(r->gp[0]); graphics::sysv::ret_size(r, w ? w->min_size : Size{0, 0}); }
uint64_t w_styleMask(Addr self, Addr) { WindowPayload* w = W(self); return w ? w->style_mask : 0; }
int64_t w_level(Addr self, Addr) { WindowPayload* w = W(self); return w ? w->level : 0; }
Addr w_setLevel(Addr self, Addr, int64_t l) { if (WindowPayload* w = W(self)) w->level = l; return 0; }
Addr w_delegate(Addr self, Addr) { WindowPayload* w = W(self); return w ? w->delegate : 0; }
Addr w_setDelegate(Addr self, Addr, Addr d) { if (WindowPayload* w = W(self)) w->delegate = d; return 0; }
uint64_t w_isReleasedWhenClosed(Addr self, Addr) { WindowPayload* w = W(self); return w && w->released_when_closed ? 1 : 0; }
Addr w_setReleasedWhenClosed(Addr self, Addr, uint64_t f) { if (WindowPayload* w = W(self)) w->released_when_closed = (f & 0xff) != 0; return 0; }
Addr w_ignoreObject(Addr, Addr, Addr) { return 0; }

// ---- ordering and closing ---------------------------------------------------------------------------------------------------------------------------
Addr order_front(Addr self, bool key) {
    WindowPayload* w = W(self);
    if (!w || w->closing) return 0;
    ensure_window_native(self);
    if (!w->native) return 0;
    st().gui->show(w->native, key);
    w->visible = true;
    if (key) if (AppPayload* app = A(st().app)) { app->key_window = self; app->main_window = self; }
    return 0;
}
Addr w_makeKeyAndOrderFront(Addr self, Addr, Addr) { return order_front(self, true); }
Addr w_orderFront(Addr self, Addr, Addr) { return order_front(self, false); }
Addr w_orderOut(Addr self, Addr, Addr) {
    WindowPayload* w = W(self);
    if (w && w->native && st().gui) { st().gui->hide(w->native); w->visible = false; }
    return 0;
}
Addr w_close(Addr self, Addr) { window_close(self, false); return 0; }
Addr w_performClose(Addr self, Addr, Addr) { window_close(self, true); return 0; }
uint64_t w_isVisible(Addr self, Addr) {
    WindowPayload* w = W(self);
    if (!w) return 0;
    return w->native && st().gui ? st().gui->is_visible(w->native) : 0;
}
uint64_t w_isKeyWindow(Addr self, Addr) { AppPayload* app = A(st().app); return app && app->key_window == self ? 1 : 0; }
uint64_t w_isMainWindow(Addr self, Addr) { AppPayload* app = A(st().app); return app && app->main_window == self ? 1 : 0; }
Addr w_makeKeyWindow(Addr self, Addr) {
    WindowPayload* w = W(self);
    AppPayload* app = A(st().app);
    if (w && app && w->native && st().gui) { st().gui->activate(w->native); app->key_window = self; }
    return 0;
}
uint64_t w_yes(Addr, Addr) { return 1; }

// ---- first responder -----------------------------------------------------------------------------------------------------------------------------------
uint64_t w_makeFirstResponder(Addr self, Addr, Addr r) {
    WindowPayload* w = W(self);
    if (!w) return 0;
    if (r && !(rt().send(r, "acceptsFirstResponder") & 0xff)) return 0;
    if (w->first_responder && w->first_responder != r) {
        if (!(rt().send(w->first_responder, "resignFirstResponder") & 0xff)) return 0;
    }
    w->first_responder = r;
    if (r) {
        rt().send(r, "becomeFirstResponder");
        if (ViewPayload* v = V(r)) if (v->native && st().gui) st().gui->focus(v->native);
    }
    return 1;
}
Addr w_firstResponder(Addr self, Addr) { WindowPayload* w = W(self); return w ? (w->first_responder ? w->first_responder : self) : 0; }
Addr w_display(Addr, Addr) { return 0; }

}  // namespace

void install_window_class(objc::Runtime& r) {
    Class* win = r.declare_native_class("NSWindow", "NSResponder", 16);
    st().window = win;
    r.add_native_methods(win, false, {
        {"initWithContentRect:styleMask:backing:defer:", "@64@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16Q48Q56B64", addr(w_init), true},
        {"dealloc", "v16@0:8", addr(w_dealloc), false},
        {"title", "@16@0:8", addr(w_title), false},
        {"setTitle:", "v24@0:8@16", addr(w_setTitle), false},
        {"contentView", "@16@0:8", addr(w_contentView), false},
        {"setContentView:", "v24@0:8@16", addr(w_setContentView), false},
        {"frame", "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8", addr(w_frame), true},
        {"setFrame:display:", "v52@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16B48", addr(w_setFrameDisplay), true},
        {"setContentSize:", "v32@0:8{CGSize=dd}16", addr(w_setContentSize), true},
        {"setFrameOrigin:", "v32@0:8{CGPoint=dd}16", addr(w_setFrameOrigin), true},
        {"center", "v16@0:8", addr(w_center), false},
        {"setMinSize:", "v32@0:8{CGSize=dd}16", addr(w_setMinSize), true},
        {"minSize", "{CGSize=dd}16@0:8", addr(w_minSize), true},
        {"styleMask", "Q16@0:8", addr(w_styleMask), false},
        {"level", "q16@0:8", addr(w_level), false},
        {"setLevel:", "v24@0:8q16", addr(w_setLevel), false},
        {"delegate", "@16@0:8", addr(w_delegate), false},
        {"setDelegate:", "v24@0:8@16", addr(w_setDelegate), false},
        {"isReleasedWhenClosed", "B16@0:8", addr(w_isReleasedWhenClosed), false},
        {"setReleasedWhenClosed:", "v20@0:8B16", addr(w_setReleasedWhenClosed), false},
        {"makeKeyAndOrderFront:", "v24@0:8@16", addr(w_makeKeyAndOrderFront), false},
        {"orderFront:", "v24@0:8@16", addr(w_orderFront), false},
        {"orderFrontRegardless", "v16@0:8", addr(w_orderFront), false},
        {"orderOut:", "v24@0:8@16", addr(w_orderOut), false},
        {"close", "v16@0:8", addr(w_close), false},
        {"performClose:", "v24@0:8@16", addr(w_performClose), false},
        {"isVisible", "B16@0:8", addr(w_isVisible), false},
        {"isKeyWindow", "B16@0:8", addr(w_isKeyWindow), false},
        {"isMainWindow", "B16@0:8", addr(w_isMainWindow), false},
        {"makeKeyWindow", "v16@0:8", addr(w_makeKeyWindow), false},
        {"canBecomeKeyWindow", "B16@0:8", addr(w_yes), false},
        {"makeFirstResponder:", "B24@0:8@16", addr(w_makeFirstResponder), false},
        {"firstResponder", "@16@0:8", addr(w_firstResponder), false},
        {"display", "v16@0:8", addr(w_display), false},
        {"setBackgroundColor:", "v24@0:8@16", addr(w_ignoreObject), false},
        {"setInitialFirstResponder:", "v24@0:8@16", addr(w_ignoreObject), false},
    });
}

}  // namespace cider::appkit
