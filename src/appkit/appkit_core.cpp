// AppKit core: shared state, payload access, the connection to the GUI backend, responder chain helpers, notifications and the class registry.
#include <format>

#include "orvyn/appkit.hpp"
#include "orvyn/libsystem.hpp"
#include "orvyn/log.hpp"
#include "orvyn/runloop.hpp"
#include "../libsystem/internal.hpp"
#include "appkit_internal.hpp"
#include "appkit_install.hpp"

namespace orvyn::appkit {
namespace {

State g_state;
std::unordered_map<std::string, uint64_t> g_symbols;       // exported variables: NSApp, notification names

template <class T>
T* payload_for(Addr obj, Class* base) {
    if (!obj || !base) return nullptr;
    uint64_t* slot = fnd::payload_slot(obj);
    if (!slot || !fnd::is_kind(obj, base)) return nullptr;
    if (!*slot) *slot = fnd::store_add(std::make_unique<T>());
    return dynamic_cast<T*>(fnd::store_get(*slot));
}

struct PoolScope {
    Addr token;
    PoolScope() : token(rt().pool_push()) {}
    ~PoolScope() { rt().pool_pop(token); }
};

}  // namespace

State& st() { return g_state; }
ViewPayload* V(Addr obj) { return payload_for<ViewPayload>(obj, st().view); }
WindowPayload* W(Addr obj) { return payload_for<WindowPayload>(obj, st().window); }
AppPayload* A(Addr obj) { return payload_for<AppPayload>(obj, st().application); }
EventPayload* E(Addr obj) { return payload_for<EventPayload>(obj, st().event); }

Addr sel(const char* name) { return rt().sel(name); }
bool responds(Addr obj, Addr s) {
    if (!obj || !s) return false;
    Class* c = rt().class_of_object(obj);
    return c && rt().find_imp(c, s) != 0;
}

void post(const char* notification, Addr object) { fnd::post_notification(notification, object, 0); }

// ---- responder chain -----------------------------------------------------------------------------------------------------------------------------
Addr next_responder(Addr r) {
    if (!r) return 0;
    if (fnd::is_kind(r, st().view)) {
        ViewPayload* v = V(r);
        if (v && v->superview) return v->superview;
        return v ? v->window : 0;
    }
    if (fnd::is_kind(r, st().window)) return st().app;
    return 0;
}

bool send_action(Addr action, Addr target, Addr sender) {
    if (!action) return false;
    if (target) {
        if (!responds(target, action)) return false;
        rt().send(target, action, {sender});
        return true;
    }
    AppPayload* app = A(st().app);
    Addr start = 0;
    if (app && app->key_window) {
        WindowPayload* w = W(app->key_window);
        start = w && w->first_responder ? w->first_responder : app->key_window;
    }
    for (Addr r = start; r; r = next_responder(r)) {
        if (responds(r, action)) { rt().send(r, action, {sender}); return true; }
        if (fnd::is_kind(r, st().window)) {
            WindowPayload* w = W(r);
            if (w && w->delegate && responds(w->delegate, action)) { rt().send(w->delegate, action, {sender}); return true; }
        }
    }
    if (st().app && responds(st().app, action)) { rt().send(st().app, action, {sender}); return true; }
    if (app && app->delegate && responds(app->delegate, action)) { rt().send(app->delegate, action, {sender}); return true; }
    return false;
}

// ---- backend events --------------------------------------------------------------------------------------------------------------------------------
namespace {

Addr window_for(gui::NativeId n) {
    auto it = st().window_of_native.find(n);
    return it == st().window_of_native.end() ? 0 : it->second;
}
Addr view_for(gui::NativeId n) {
    auto it = st().view_of_native.find(n);
    return it == st().view_of_native.end() ? 0 : it->second;
}

Addr make_notification_object(const char* name, Addr object) {
    const Addr n = fnd::new_string(name);
    const Addr note = rt().send(rt().class_named("NSNotification")->addr, "notificationWithName:object:", {n, object});
    rt().release(n);
    return note;
}

bool window_should_close(Addr w) {
    WindowPayload* wp = W(w);
    if (!wp) return true;
    if (wp->closing) return true;
    const Addr sh = sel("windowShouldClose:");
    if (wp->delegate && responds(wp->delegate, sh)) {
        if (!(rt().send(wp->delegate, sh, {w}) & 0xff)) return false;
    } else if (responds(w, sh)) {
        if (!(rt().send(w, sh, {w}) & 0xff)) return false;
    }
    return true;
}

void window_will_close(Addr w) {
    WindowPayload* wp = W(w);
    if (!wp || wp->closing) return;
    wp->closing = true;
    if (wp->delegate && responds(wp->delegate, sel("windowWillClose:"))) rt().send(wp->delegate, sel("windowWillClose:"), {make_notification_object("NSWindowWillCloseNotification", w)});
    post("NSWindowWillCloseNotification", w);
}

void forget_tree(Addr view) {
    ViewPayload* v = V(view);
    if (!v) return;
    if (v->native) st().view_of_native.erase(v->native);
    v->native = 0;
    for (Addr s : v->subviews) forget_tree(s);
}

// Everything that happens after a window is gone from the screen (or never was on it): detach the tree, leave the application's list, maybe terminate.
void window_closed_cleanup(Addr w) {
    WindowPayload* wp = W(w);
    if (!wp) return;
    if (!wp->closing) window_will_close(w);                    // destroyed without a close request (programmatic destroy / system)
    wp->native = 0;
    wp->visible = false;
    if (wp->content_view) forget_tree(wp->content_view);
    AppPayload* app = A(st().app);
    bool was_listed = false;
    if (app) {
        auto it = std::find(app->windows.begin(), app->windows.end(), w);
        if (it != app->windows.end()) { app->windows.erase(it); was_listed = true; }
        if (app->key_window == w) app->key_window = 0;
        if (app->main_window == w) app->main_window = 0;
    }
    const bool last = app && app->windows.empty();
    const bool running = app && app->running;
    rt().retain(w);                                            // keep it alive until the callbacks below finish
    if (was_listed && wp->app_holds_reference) { wp->app_holds_reference = false; rt().release(w); }
    if (last && running && !app->terminating) {
        bool terminate = false;
        const Addr sel_last = sel("applicationShouldTerminateAfterLastWindowClosed:");
        if (app->delegate && responds(app->delegate, sel_last)) terminate = (rt().send(app->delegate, sel_last, {st().app}) & 0xff) != 0;
        if (terminate) terminate_application();
    }
    rt().release(w);
}

void on_window_destroyed(gui::NativeId n) {
    const Addr w = window_for(n);
    st().window_of_native.erase(n);
    if (w) window_closed_cleanup(w);
}

void resize_content_to_client(Addr w, Size size) {
    WindowPayload* wp = W(w);
    if (!wp || wp->in_native_resize) return;
    wp->content_rect.size = size;
    if (wp->content_view) view_set_frame(wp->content_view, geometry::make_rect(0, 0, size.width, size.height));
}

void on_click(gui::NativeId n) {
    const Addr v = view_for(n);
    ViewPayload* vp = v ? V(v) : nullptr;
    if (!vp || !vp->enabled) return;
    if (fnd::is_kind(v, st().button) && vp->is_checkbox) vp->state = st().gui->checked(n);
    if (!vp->action) return;
    send_action(vp->action, vp->target, v);
}

void on_mouse(gui::NativeId n, const gui::MouseEvent& m) {
    const Addr v = view_for(n);
    ViewPayload* vp = v ? V(v) : nullptr;
    if (!vp) return;
    const Point wp = to_window_point(v, m.x, m.y);
    int64_t type = 0;
    const char* selector = nullptr;
    const bool right = m.button == 1;
    switch (m.type) {
        case gui::MouseEvent::Down: type = right ? 3 : 1; selector = right ? "rightMouseDown:" : "mouseDown:"; break;
        case gui::MouseEvent::Up: type = right ? 4 : 2; selector = right ? "rightMouseUp:" : "mouseUp:"; break;
        case gui::MouseEvent::Drag: type = 6; selector = "mouseDragged:"; break;
        case gui::MouseEvent::Move: type = 5; selector = "mouseMoved:"; break;
    }
    const Addr ev = make_event(type, wp, vp->window);
    if (EventPayload* ep = E(ev)) { ep->click_count = m.click_count; ep->button = m.button; ep->modifiers = m.modifiers; }
    rt().send(v, sel(selector), {ev});
}

void on_key(gui::NativeId n, const gui::KeyEvent& k) {
    const Addr w = window_for(n);
    WindowPayload* wp = w ? W(w) : nullptr;
    if (!wp) return;
    const Addr target = wp->first_responder ? wp->first_responder : (wp->content_view ? wp->content_view : w);
    const Addr ev = make_event(k.down ? 10 : 11, Point{0, 0}, w);
    if (EventPayload* ep = E(ev)) { ep->characters = k.characters; ep->key_code = k.key_code; ep->modifiers = k.modifiers; }
    rt().send(target, sel(k.down ? "keyDown:" : "keyUp:"), {ev});
}

}  // namespace

void window_close(Addr w, bool ask) {
    WindowPayload* wp = W(w);
    if (!wp || wp->closing) return;
    if (ask && !window_should_close(w)) return;
    window_will_close(w);
    if (wp->native && st().gui) st().gui->destroy(wp->native);  // the backend reports window_destroyed -> window_closed_cleanup
    else window_closed_cleanup(w);                              // never shown: nothing native to wait for
}

void wire_backend_events() {
    gui::Events& e = st().gui->events;
    e.window_should_close = [](gui::NativeId n) {
        PoolScope pool;
        const Addr w = window_for(n);
        if (!w) return true;
        if (!window_should_close(w)) return false;
        window_will_close(w);
        return true;
    };
    e.window_destroyed = [](gui::NativeId n) { PoolScope pool; on_window_destroyed(n); };
    e.window_resized = [](gui::NativeId n, geometry::Size size) {
        PoolScope pool;
        const Addr w = window_for(n);
        if (!w) return;
        WindowPayload* wp = W(w);
        const bool changed = wp && (wp->content_rect.size.width != size.width || wp->content_rect.size.height != size.height);
        resize_content_to_client(w, size);
        if (changed && wp->delegate && responds(wp->delegate, sel("windowDidResize:"))) rt().send(wp->delegate, sel("windowDidResize:"), {make_notification_object("NSWindowDidResizeNotification", w)});
    };
    e.window_moved = [](gui::NativeId n, geometry::Point origin) {
        PoolScope pool;
        const Addr w = window_for(n);
        WindowPayload* wp = w ? W(w) : nullptr;
        if (!wp) return;
        wp->content_rect.origin = origin;
        if (wp->delegate && responds(wp->delegate, sel("windowDidMove:"))) rt().send(wp->delegate, sel("windowDidMove:"), {make_notification_object("NSWindowDidMoveNotification", w)});
    };
    e.window_dpi_changed = [](gui::NativeId n) {
        PoolScope pool;
        const Addr w = window_for(n);
        WindowPayload* wp = w ? W(w) : nullptr;
        if (!wp || !wp->content_view) return;
        const Size size = st().gui->client_size(n);
        wp->content_rect.size = size;
        view_set_frame(wp->content_view, geometry::make_rect(0, 0, size.width, size.height));     // re-applies every child rectangle with the new scale
    };
    e.window_activated = [](gui::NativeId n, bool active) {
        PoolScope pool;
        const Addr w = window_for(n);
        AppPayload* app = A(st().app);
        WindowPayload* wp = w ? W(w) : nullptr;
        if (!app || !wp) return;
        if (active) { app->key_window = w; app->main_window = w; app->active = true; } else if (app->key_window == w) app->key_window = 0;
        const char* sel_name = active ? "windowDidBecomeKey:" : "windowDidResignKey:";
        if (wp->delegate && responds(wp->delegate, sel(sel_name))) rt().send(wp->delegate, sel(sel_name), {make_notification_object(active ? "NSWindowDidBecomeKeyNotification" : "NSWindowDidResignKeyNotification", w)});
    };
    e.control_clicked = [](gui::NativeId n) { PoolScope pool; on_click(n); };
    e.mouse = [](gui::NativeId n, const gui::MouseEvent& m) { PoolScope pool; on_mouse(n, m); };
    e.paint = [](gui::NativeId n, const geometry::Rect&) { PoolScope pool; const Addr v = view_for(n); if (v) view_paint(v); };
    e.key = [](gui::NativeId n, const gui::KeyEvent& k) { PoolScope pool; on_key(n, k); };
}

bool ensure_backend() {
    if (st().gui) return true;
    libsystem::RuntimeContext* c = libsystem::context();
    const std::string kind = c ? c->gui_backend : "win32";
    if (kind == "offscreen") st().gui = gui::create_offscreen_backend(c ? c->gui_script : std::vector<std::string>{});
    else st().gui = gui::create_win32_backend();
    if (!st().gui) { libsystem::detail::trap("AppKit: the GUI backend could not be created"); return false; }
    wire_backend_events();
    log::info(log::Category::Objc, "AppKit: {} GUI backend ready", st().gui->name());
    return true;
}

Addr make_event(int64_t type, Point loc, Addr window) {
    const Addr ev = rt().create_instance(st().event, 0);
    if (!ev) return 0;
    EventPayload* p = E(ev);
    if (!p) { rt().release(ev); return 0; }
    p->type = type;
    p->location = loc;
    p->window = window;
    return fnd::autoreleased(ev);
}

Point to_window_point(Addr view, double x, double y) {
    // walk to the root accumulating frame origins (bounds origins are not supported, see APPKIT.md)
    ViewPayload* v = V(view);
    if (!v) return Point{x, y};
    double ox = 0, oy = 0;                                      // origin of `view` in window space, y up, measured at its bottom-left corner
    Addr cur = view;
    double height = v->frame.size.height;
    while (cur) {
        ViewPayload* c = V(cur);
        if (!c) break;
        ox += c->frame.origin.x;
        oy += c->frame.origin.y;
        cur = c->superview;
    }
    // the local point arrives top-left based; convert to the view's own orientation, then to window y up
    const double local_y = v->flipped ? y : height - y;
    return Point{ox + x, oy + local_y};
}

// ---- application singleton -------------------------------------------------------------------------------------------------------------------------
Addr shared_application() {
    if (st().app) return st().app;
    if (!ensure_backend()) return 0;
    const Addr a = rt().arena_alloc(16, 16);
    reinterpret_cast<Addr*>(a)[0] = st().application->addr;
    reinterpret_cast<Addr*>(a)[1] = 0;
    st().app = a;
    if (st().nsapp_var) *reinterpret_cast<Addr*>(st().nsapp_var) = a;
    A(a);                                                      // payload now
    return a;
}

std::optional<uint64_t> data_symbol(std::string_view name) {
    auto it = g_symbols.find(std::string(name));
    return it == g_symbols.end() ? std::nullopt : std::optional<uint64_t>(it->second);
}

uint64_t shutdown(uint64_t& open_windows) {
    open_windows = 0;
    if (!st().rt) return 0;
    runloop::clear_gui_hooks();
    uint64_t left = 0;
    if (st().gui) {
        st().gui->events = gui::Events{};                      // no guest code may run any more
        std::vector<gui::NativeId> windows;
        for (auto& kv : st().window_of_native) windows.push_back(kv.first);
        open_windows = windows.size();
        for (gui::NativeId n : windows) st().gui->destroy(n);
        left = st().gui->native_count();
        st().gui.reset();
    }
    st().view_of_native.clear();
    st().window_of_native.clear();
    st().app = 0;
    return left;
}

void install(objc::Runtime& r) {
    State fresh;
    st().gui.reset();
    st() = std::move(fresh);
    st().rt = &r;
    g_symbols.clear();
    r.set_native_owner("AppKit");
    install_responder_classes(r);       // appkit_views.cpp: NSResponder, NSView, NSControl, NSButton, NSTextField
    install_window_class(r);            // appkit_window.cpp
    install_application_class(r);       // appkit_app.cpp
    install_event_classes(r);           // appkit_event.cpp: NSEvent, NSScreen
    install_drawing_classes(r);         // appkit_draw.cpp: NSGraphicsContext
    st().nsapp_var = r.arena_alloc(8, 8);
    *reinterpret_cast<Addr*>(st().nsapp_var) = 0;
    g_symbols["_NSApp"] = st().nsapp_var;
    for (const char* n : {"NSApplicationDidFinishLaunchingNotification", "NSApplicationWillTerminateNotification", "NSApplicationDidBecomeActiveNotification",
                          "NSWindowWillCloseNotification", "NSWindowDidResizeNotification", "NSWindowDidMoveNotification", "NSWindowDidBecomeKeyNotification",
                          "NSWindowDidResignKeyNotification"}) {
        const Addr var = r.arena_alloc(8, 8);
        *reinterpret_cast<Addr*>(var) = fnd::immortal_string(n);
        g_symbols[std::string("_") + n] = var;
    }
    r.add_dynamic_provider([](std::string_view name) { return data_symbol(name); });
}

}  // namespace orvyn::appkit
