#pragma once
// The seam between AppKit semantics (src/appkit) and a native windowing system (see docs/GUI-BACKEND.md).
//
// The AppKit layer decides WHAT exists (windows, views, controls, their frames, titles and state); a backend makes it exist: the Win32 backend
// creates real HWNDs; the offscreen backend keeps the same model in memory and replays scripted input (used by the headless tests; it is a
// platform backend like Qt's "offscreen" plugin, not a shortcut around the AppKit layer: events travel the same callbacks as Win32 messages).
//
// Coordinates crossing this interface are POINTS (AppKit logical units; the backend applies the monitor scale):
//   * window rectangles: AppKit screen space (origin bottom-left of the primary display, y up);
//   * child rectangles: relative to the parent's client area, origin TOP-left, y down (the AppKit layer already flipped them).
// Everything is called on the main guest thread, which owns every window.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cider/geometry.hpp"

namespace cider::gui {

using NativeId = uint64_t;                  // 0 = none; for Win32 the HWND value

enum class ChildKind { Container, PushButton, CheckBox, Label, TextEdit };
constexpr unsigned kStyleTitled = 1, kStyleClosable = 2, kStyleMiniaturizable = 4, kStyleResizable = 8;      // NSWindowStyleMask values

struct WindowSpec {
    unsigned style_mask = kStyleTitled | kStyleClosable;
    std::string title;
    geometry::Rect content_rect;            // screen space, y up, points
};

struct MouseEvent {
    enum Type { Down = 1, Up = 2, Move = 3, Drag = 4 } type = Down;
    int button = 0;                         // 0 left, 1 right, 2 other
    double x = 0, y = 0;                    // points, origin top-left of the receiving native object
    int click_count = 1;
    unsigned modifiers = 0;                 // NSEventModifierFlag bits
};
struct KeyEvent {
    bool down = true;
    unsigned key_code = 0;                  // Windows virtual-key code (documented difference from Apple key codes)
    std::string characters;                 // UTF-8; empty for non-character keys
    unsigned modifiers = 0;
};

// Input and lifecycle notifications from the native system.
struct Events {
    std::function<bool(NativeId window)> window_should_close;                // the user asked to close: true lets it happen
    std::function<void(NativeId window)> window_destroyed;                   // the native window no longer exists (any reason)
    std::function<void(NativeId window, geometry::Size content)> window_resized;   // new content size in points
    std::function<void(NativeId window, geometry::Point origin)> window_moved;     // new content origin, screen y up
    std::function<void(NativeId window)> window_dpi_changed;                 // the scale changed: re-apply every child rectangle
    std::function<void(NativeId window, bool active)> window_activated;
    std::function<void(NativeId control)> control_clicked;                   // push button / check box activated
    std::function<void(NativeId native, const MouseEvent&)> mouse;           // on container views
    std::function<void(NativeId window, const KeyEvent&)> key;               // keyboard input that reached a container view or the window
};

class Backend {
public:
    virtual ~Backend() = default;
    Events events;
    virtual const char* name() const = 0;

    // ---- windows ------------------------------------------------------------------------------------------------------------------------------
    virtual NativeId create_window(const WindowSpec& spec) = 0;               // hidden; 0 on failure
    virtual void set_window_title(NativeId window, const std::string& utf8) = 0;
    virtual geometry::Rect content_rect(NativeId window) = 0;                  // screen space, y up
    virtual geometry::Rect frame_rect(NativeId window) = 0;                    // outer rectangle including the title bar, screen space, y up
    virtual void set_content_rect(NativeId window, const geometry::Rect& screen_rect) = 0;
    virtual geometry::Size client_size(NativeId window) = 0;
    virtual void set_min_content_size(NativeId window, geometry::Size size) = 0;
    virtual void center(NativeId window) = 0;
    virtual void show(NativeId window, bool activate) = 0;
    virtual void hide(NativeId window) = 0;
    virtual bool is_visible(NativeId window) = 0;
    virtual bool is_active(NativeId window) = 0;
    virtual void activate(NativeId window) = 0;

    // ---- children (views and controls) ----------------------------------------------------------------------------------------------------
    virtual NativeId create_child(NativeId parent, ChildKind kind, const geometry::Rect& rect_topleft, const std::string& text) = 0;
    virtual void set_rect(NativeId child, const geometry::Rect& rect_topleft) = 0;
    virtual void set_text(NativeId native, const std::string& utf8) = 0;
    virtual std::string text(NativeId native) = 0;
    virtual void set_enabled(NativeId native, bool enabled) = 0;
    virtual void set_hidden(NativeId native, bool hidden) = 0;
    virtual void set_checked(NativeId native, int state) = 0;                   // 0 off, 1 on
    virtual int checked(NativeId native) = 0;
    virtual void set_read_only(NativeId native, bool read_only) = 0;
    virtual void set_placeholder(NativeId native, const std::string& utf8) = 0;
    virtual void focus(NativeId native) = 0;
    virtual void destroy(NativeId native) = 0;                                  // a window (and everything inside) or a single child

    // ---- screen and loop -----------------------------------------------------------------------------------------------------------------------
    virtual geometry::Rect screen_frame() = 0;                                  // primary display, screen space, y up, points
    virtual geometry::Rect visible_frame() = 0;                                 // minus the taskbar
    virtual double scale_factor() = 0;                                          // primary monitor: dpi / 96
    virtual int pump() = 0;                                                     // handle pending input without blocking; returns how many messages
    virtual void wait(void* wake_event, int64_t ms) = 0;                        // block until input, the event, or the timeout (-1: no limit)
    virtual size_t native_count() const = 0;                                    // live native objects (windows + children): leak checks
};

std::unique_ptr<Backend> create_win32_backend();
// `script`: lines replayed by pump(): "click <control text>", "close", "wait <ms>", "type <text>" (into the first editable field),
// "mouse <x> <y>" (left click on the front window content at the point, top-left origin), "key <chars>".
std::unique_ptr<Backend> create_offscreen_backend(std::vector<std::string> script);

}  // namespace cider::gui
