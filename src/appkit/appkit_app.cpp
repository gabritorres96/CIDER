// NSApplication (ORVYN 0.5.7): the singleton, the launch sequence, run / stop: / terminate:, the delegate and the action dispatch.
//
// run drives the shared run loop (orvyn/runloop.hpp) with the GUI backend as its input source: window messages, timers, sources and
// libdispatch main-queue blocks are all serviced by the same loop on the main thread. See docs/EVENT-LOOP.md.
#include "orvyn/libsystem.hpp"
#include "orvyn/log.hpp"
#include "orvyn/runloop.hpp"
#include "../libsystem/internal.hpp"
#include "appkit_internal.hpp"
#include "appkit_install.hpp"

namespace orvyn::appkit {

void finish_launching() {
    AppPayload* app = A(st().app);
    if (!app || app->finished_launching) return;
    app->finished_launching = true;
    const Addr n = fnd::new_string("NSApplicationDidFinishLaunchingNotification");
    const Addr note = rt().send(rt().class_named("NSNotification")->addr, "notificationWithName:object:", {n, st().app});
    rt().release(n);
    if (app->delegate && responds(app->delegate, sel("applicationDidFinishLaunching:"))) rt().send(app->delegate, sel("applicationDidFinishLaunching:"), {note});
    post("NSApplicationDidFinishLaunchingNotification", st().app);
}

void terminate_application() {
    AppPayload* app = A(st().app);
    if (!app || app->terminating) return;
    // NSTerminateCancel = 0, NSTerminateNow = 1, NSTerminateLater = 2 (treated as "not now")
    const Addr should = sel("applicationShouldTerminate:");
    if (app->delegate && responds(app->delegate, should)) {
        if (rt().send(app->delegate, should, {st().app}) != 1) return;
    }
    app->terminating = true;
    const Addr n = fnd::new_string("NSApplicationWillTerminateNotification");
    const Addr note = rt().send(rt().class_named("NSNotification")->addr, "notificationWithName:object:", {n, st().app});
    rt().release(n);
    if (app->delegate && responds(app->delegate, sel("applicationWillTerminate:"))) rt().send(app->delegate, sel("applicationWillTerminate:"), {note});
    post("NSApplicationWillTerminateNotification", st().app);
    runloop::stop();
    libsystem::detail::guest_exit(0);                           // like AppKit: terminate: ends the process with status 0 (atexit handlers run)
}

namespace {

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

Addr a_sharedApplication(Addr, Addr) { return shared_application(); }
Addr a_init(Addr self, Addr) { return A(self) ? self : 0; }

Addr a_run(Addr self, Addr) {
    AppPayload* app = A(self);
    if (!app || !ensure_backend()) return 0;
    if (app->running) { fnd::raise("NSInternalInconsistencyException", "-[NSApplication run]: the application is already running"); return 0; }
    finish_launching();
    app->running = true;
    app->stop_requested = false;
    runloop::GuiHooks hooks;
    hooks.pump = [] { return st().gui ? st().gui->pump() : 0; };
    hooks.wait = [](void* ev, int64_t ms) { if (st().gui) st().gui->wait(ev, ms); };
    hooks.keep_alive = [] { AppPayload* a = A(st().app); return a && a->running && !a->stop_requested; };
    runloop::set_gui_hooks(std::move(hooks));
    log::info(log::Category::Objc, "AppKit: event loop started");
    while (!app->stop_requested && !app->terminating) {
        const runloop::Result res = runloop::run(-1, false);
        if (res == runloop::Result::Finished) break;           // nothing can ever wake the loop again
        if (!libsystem::context() || !libsystem::context()->fault.empty()) break;
        if (libsystem::context()->threads && libsystem::context()->threads->exit_requested()) break;
    }
    runloop::clear_gui_hooks();
    app->running = false;
    app->stop_requested = false;
    log::info(log::Category::Objc, "AppKit: event loop ended");
    return 0;
}
Addr a_stop(Addr self, Addr, Addr) {
    AppPayload* app = A(self);
    if (!app) return 0;
    app->stop_requested = true;
    runloop::stop();
    return 0;
}
Addr a_terminate(Addr, Addr, Addr) { terminate_application(); return 0; }
Addr a_finishLaunching(Addr, Addr) { finish_launching(); return 0; }
uint64_t a_setActivationPolicy(Addr self, Addr, int64_t p) { AppPayload* app = A(self); if (!app) return 0; app->policy = p; return p == 0 || p == 1 || p == 2 ? 1 : 0; }
int64_t a_activationPolicy(Addr self, Addr) { AppPayload* app = A(self); return app ? app->policy : 0; }
Addr a_activate(Addr self, Addr, uint64_t) {
    AppPayload* app = A(self);
    if (!app) return 0;
    app->active = true;
    if (app->key_window) if (WindowPayload* w = W(app->key_window)) if (w->native && st().gui) st().gui->activate(w->native);
    return 0;
}
Addr a_delegate(Addr self, Addr) { AppPayload* app = A(self); return app ? app->delegate : 0; }
Addr a_setDelegate(Addr self, Addr, Addr d) { if (AppPayload* app = A(self)) app->delegate = d; return 0; }
Addr a_windows(Addr self, Addr) {
    AppPayload* app = A(self);
    return app ? fnd::new_array(std::vector<Addr>(app->windows), false, true) : 0;
}
Addr a_keyWindow(Addr self, Addr) { AppPayload* app = A(self); return app ? app->key_window : 0; }
Addr a_mainWindow(Addr self, Addr) { AppPayload* app = A(self); return app ? app->main_window : 0; }
uint64_t a_isRunning(Addr self, Addr) { AppPayload* app = A(self); return app && app->running ? 1 : 0; }
uint64_t a_isActive(Addr self, Addr) { AppPayload* app = A(self); return app && app->active ? 1 : 0; }
void a_sendAction(abi::GuestRegs* r) { r->ret_rax = send_action(r->gp[2], r->gp[3], r->gp[4]) ? 1 : 0; }      // sendAction:to:from: self,_cmd,action,target,sender
Addr a_mainMenu(Addr self, Addr) { AppPayload* app = A(self); return app ? app->main_menu : 0; }
Addr a_setMainMenu(Addr self, Addr, Addr m) {
    AppPayload* app = A(self);
    if (!app) return 0;
    if (m) rt().retain(m);
    if (app->main_menu) rt().release(app->main_menu);
    app->main_menu = m;                                          // stored, never shown: menus are not implemented
    return 0;
}
Addr a_ignore(Addr, Addr, Addr) { return 0; }

}  // namespace

void install_application_class(objc::Runtime& r) {
    Class* app = r.declare_native_class("NSApplication", "NSResponder", 16);
    st().application = app;
    r.add_native_methods(app, true, {{"sharedApplication", "@16@0:8", addr(a_sharedApplication), false}});
    r.add_native_methods(app, false, {
        {"init", "@16@0:8", addr(a_init), false},
        {"run", "v16@0:8", addr(a_run), false},
        {"stop:", "v24@0:8@16", addr(a_stop), false},
        {"terminate:", "v24@0:8@16", addr(a_terminate), false},
        {"finishLaunching", "v16@0:8", addr(a_finishLaunching), false},
        {"setActivationPolicy:", "B24@0:8q16", addr(a_setActivationPolicy), false},
        {"activationPolicy", "q16@0:8", addr(a_activationPolicy), false},
        {"activateIgnoringOtherApps:", "v20@0:8B16", addr(a_activate), false},
        {"delegate", "@16@0:8", addr(a_delegate), false},
        {"setDelegate:", "v24@0:8@16", addr(a_setDelegate), false},
        {"windows", "@16@0:8", addr(a_windows), false},
        {"keyWindow", "@16@0:8", addr(a_keyWindow), false},
        {"mainWindow", "@16@0:8", addr(a_mainWindow), false},
        {"isRunning", "B16@0:8", addr(a_isRunning), false},
        {"isActive", "B16@0:8", addr(a_isActive), false},
        {"sendAction:to:from:", "B40@0:8:16@24@32", addr(a_sendAction), true},
        {"mainMenu", "@16@0:8", addr(a_mainMenu), false},
        {"setMainMenu:", "v24@0:8@16", addr(a_setMainMenu), false},
        {"setApplicationIconImage:", "v24@0:8@16", addr(a_ignore), false},
    });
}

}  // namespace orvyn::appkit
