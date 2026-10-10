#pragma once
// Private to src/appkit. See docs/APPKIT.md for what is implemented and what is not.
//
// Object model: AppKit objects are ordinary Objective-C objects [isa][payload id] whose state lives in a host payload (like the Foundation
// classes), so guest subclasses work (NSView subclasses, NSObject delegates, NSWindow subclasses). Retain rules follow Cocoa: a view retains
// its subviews, a window retains its content view, NSApplication retains the open windows; targets, delegates and back pointers are NOT retained.
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cider/geometry.hpp"
#include "cider/objc.hpp"
#include "../foundation/foundation_internal.hpp"
#include "../graphics/gui_backend.hpp"
#include "../graphics/sysv_args.hpp"

namespace cider::appkit {

using geometry::Point;
using geometry::Rect;
using geometry::Size;
using objc::Addr;
using objc::Class;
using objc::Runtime;
namespace fnd = cider::foundation;

struct ViewPayload : fnd::Payload {
    Rect frame;
    Addr superview = 0;                    // not retained
    Addr window = 0;                       // not retained: the NSWindow whose content tree contains this view
    std::vector<Addr> subviews;            // retained, back to front
    bool hidden = false, flipped = false, needs_display = false;
    unsigned autoresize = 0;
    int64_t tag = 0;
    gui::NativeId native = 0;
    // NSControl
    Addr target = 0;                       // not retained
    Addr action = 0;                       // SEL
    bool enabled = true;
    Addr string_value = 0;                 // retained NSString: NSButton title, NSTextField value before it has a native control
    // NSButton
    int64_t state = 0;
    int button_type = 0;                   // NSButtonTypeMomentaryPushIn = 7, NSButtonTypeSwitch = 3 (the pre-10.12 constants are accepted too)
    bool is_checkbox = false;
    // NSTextField
    bool editable = true, selectable = true, bezeled = true, draws_background = true;
    Addr placeholder = 0;                  // retained
    Addr delegate = 0;                     // not retained
};

struct WindowPayload : fnd::Payload {
    Rect content_rect;                     // screen space, y up; authoritative until the native window exists
    unsigned style_mask = 0;
    Addr title = 0;                        // retained
    Addr content_view = 0;                 // retained
    Addr delegate = 0;                     // not retained
    Addr first_responder = 0;              // not retained
    bool visible = false, closing = false, released_when_closed = false, defer = false, in_native_resize = false;
    int64_t level = 0;
    Size min_size{0, 0};
    gui::NativeId native = 0;
    bool app_holds_reference = false;      // NSApplication keeps a +1 on every window that has not been closed
};

struct AppPayload : fnd::Payload {
    Addr delegate = 0;                     // not retained
    std::vector<Addr> windows;             // order of creation; each entry carries the application's +1
    Addr key_window = 0, main_window = 0;  // not retained
    Addr main_menu = 0;                    // retained but unused: menus are not implemented
    bool finished_launching = false, running = false, stop_requested = false, terminating = false, active = false;
    int64_t policy = 0;                    // NSApplicationActivationPolicyRegular
};

struct EventPayload : fnd::Payload {
    int64_t type = 0;                      // NSEventType
    Point location;                        // window coordinates, y up
    Addr window = 0;                       // not retained
    std::string characters;
    unsigned key_code = 0, modifiers = 0;
    int64_t click_count = 0, button = 0;
};

struct State {
    Runtime* rt = nullptr;
    std::unique_ptr<gui::Backend> gui;
    Addr app = 0;                          // the NSApplication singleton (arena object)
    Addr nsapp_var = 0;                    // storage behind the exported variable NSApp
    std::unordered_map<gui::NativeId, Addr> view_of_native;     // not retained
    std::unordered_map<gui::NativeId, Addr> window_of_native;   // not retained
    Class *responder = nullptr, *application = nullptr, *window = nullptr, *view = nullptr, *control = nullptr, *button = nullptr,
          *text_field = nullptr, *event = nullptr, *screen = nullptr;
    int callback_depth = 0;
};
State& st();
inline Runtime& rt() { return *st().rt; }

// ---- payload access (lazily created; null for objects of the wrong class) -----------------------------------------------------------------
ViewPayload* V(Addr obj);
WindowPayload* W(Addr obj);
AppPayload* A(Addr obj);
EventPayload* E(Addr obj);

// ---- backend and application life cycle --------------------------------------------------------------------------------------------------
bool ensure_backend();                                  // creates the configured backend on first use; false (and a trap) when it cannot be created
void wire_backend_events();                             // connects the backend callbacks to the AppKit objects
Addr shared_application();                              // creates NSApplication on first use
void finish_launching();
void terminate_application();                           // delegate + exit, see nsapplication.cpp
bool send_action(Addr action, Addr target, Addr sender);   // NSApplication sendAction:to:from: semantics (nil target: responder chain)
void post(const char* notification, Addr object);       // NSNotificationCenter delivery

// ---- windows ------------------------------------------------------------------------------------------------------------------------------------------------
void ensure_window_native(Addr window);                 // creates the HWND and the native tree of the content view
void window_close(Addr window, bool ask);               // performClose (ask = true: windowShouldClose:) / close
void set_content_view(Addr window, Addr view);
void window_set_content_size(Addr window, Size size);   // keeps the origin; updates the content view tree
Rect window_frame(Addr window);                         // outer rectangle, screen space, y up

// ---- views --------------------------------------------------------------------------------------------------------------------------------------------------
void view_attach(Addr view, Addr parent);               // add as subview (retains), builds natives when the parent is in a window
void view_detach(Addr view);                            // remove from the superview (releases the superview's reference when it held the last one only through the caller)
void view_set_frame(Addr view, const Rect& frame);      // autoresizes subviews, updates the native control
void view_build_native(Addr view);
void view_destroy_native(Addr view);
void view_rebuild_native(Addr view);                    // after the control kind changed (editable text field)
void view_set_window(Addr view, Addr window);           // recursive
std::string view_text(Addr view);
void view_apply_text(Addr view);
Addr next_responder(Addr responder);
Point to_window_point(Addr view, double x_down, double y_down);   // native top-left coordinates of a view -> window space, y up

// ---- misc ---------------------------------------------------------------------------------------------------------------------------------------------------
Addr make_event(int64_t type, Point loc_window, Addr window);   // autoreleased
bool responds(Addr obj, Addr sel);
Addr sel(const char* name);

}  // namespace cider::appkit
