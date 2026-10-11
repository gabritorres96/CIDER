// NSEvent (mouse and key events delivered to views by the GUI backend) and NSScreen (geometry of the primary display).
#include "appkit_internal.hpp"
#include "appkit_install.hpp"

namespace orvyn::appkit {
namespace {

using abi::GuestRegs;
using graphics::sysv::Args;

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

int64_t e_type(Addr self, Addr) { EventPayload* e = E(self); return e ? e->type : 0; }
void e_locationInWindow(GuestRegs* r) { EventPayload* e = E(r->gp[0]); graphics::sysv::ret_point(r, e ? e->location : Point{0, 0}); }
Addr e_window(Addr self, Addr) { EventPayload* e = E(self); return e ? e->window : 0; }
Addr e_characters(Addr self, Addr) { EventPayload* e = E(self); return e ? fnd::new_string_autoreleased(e->characters) : 0; }
uint64_t e_keyCode(Addr self, Addr) { EventPayload* e = E(self); return e ? e->key_code : 0; }
uint64_t e_modifierFlags(Addr self, Addr) { EventPayload* e = E(self); return e ? e->modifiers : 0; }
int64_t e_clickCount(Addr self, Addr) { EventPayload* e = E(self); return e ? e->click_count : 0; }
int64_t e_buttonNumber(Addr self, Addr) { EventPayload* e = E(self); return e ? e->button : 0; }
Addr e_dealloc(Addr self, Addr) {
    if (uint64_t* slot = fnd::payload_slot(self)) { fnd::store_drop(*slot); *slot = 0; }
    rt().destruct_instance(self);
    rt().dispose_instance(self);
    return 0;
}

// NSScreen: one immortal instance for the primary display
Addr g_main_screen = 0;
Addr s_mainScreen(Addr, Addr) { return ensure_backend() ? g_main_screen : 0; }
void s_frame(GuestRegs* r) { Args a(r); const uint64_t out = a.next_int(); graphics::sysv::ret_rect(r, out, ensure_backend() ? st().gui->screen_frame() : geometry::make_rect(0, 0, 0, 0)); }
void s_visibleFrame(GuestRegs* r) { Args a(r); const uint64_t out = a.next_int(); graphics::sysv::ret_rect(r, out, ensure_backend() ? st().gui->visible_frame() : geometry::make_rect(0, 0, 0, 0)); }
void s_backingScaleFactor(GuestRegs* r) { graphics::sysv::ret_double(r, ensure_backend() ? st().gui->scale_factor() : 1.0); }

}  // namespace

void install_event_classes(objc::Runtime& r) {
    Class* ev = r.declare_native_class("NSEvent", "NSObject", 16);
    st().event = ev;
    fnd::classes().extra_payload.push_back(ev);
    r.add_native_methods(ev, false, {
        {"dealloc", "v16@0:8", addr(e_dealloc), false},
        {"type", "Q16@0:8", addr(e_type), false},
        {"locationInWindow", "{CGPoint=dd}16@0:8", addr(e_locationInWindow), true},
        {"window", "@16@0:8", addr(e_window), false},
        {"characters", "@16@0:8", addr(e_characters), false},
        {"charactersIgnoringModifiers", "@16@0:8", addr(e_characters), false},
        {"keyCode", "S16@0:8", addr(e_keyCode), false},
        {"modifierFlags", "Q16@0:8", addr(e_modifierFlags), false},
        {"clickCount", "q16@0:8", addr(e_clickCount), false},
        {"buttonNumber", "q16@0:8", addr(e_buttonNumber), false},
    });
    Class* sc = r.declare_native_class("NSScreen", "NSObject", 16);
    st().screen = sc;
    g_main_screen = r.arena_alloc(16, 16);
    reinterpret_cast<Addr*>(g_main_screen)[0] = sc->addr;
    reinterpret_cast<Addr*>(g_main_screen)[1] = 0;
    r.add_native_methods(sc, true, {{"mainScreen", "@16@0:8", addr(s_mainScreen), false}});
    r.add_native_methods(sc, false, {
        {"frame", "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8", addr(s_frame), true},
        {"visibleFrame", "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8", addr(s_visibleFrame), true},
        {"backingScaleFactor", "d16@0:8", addr(s_backingScaleFactor), true},
    });
}

}  // namespace orvyn::appkit
