// NSResponder, NSView, NSControl, NSButton, NSTextField (CIDER 0.5.7).
//
// Layout model: every view has a frame in its superview coordinates (origin bottom-left unless the view is flipped). The backend wants
// rectangles measured from the TOP-left of the parent client area, so each frame is converted here (see cider/geometry.hpp). bounds always
// start at (0,0) and have the frame size: bounds rotation/scaling/origin are not supported.
#include <algorithm>
#include <cstring>
#include <format>

#include "cider/libsystem.hpp"
#include "../libsystem/internal.hpp"
#include "appkit_internal.hpp"
#include "appkit_install.hpp"

namespace cider::appkit {
namespace {

using abi::GuestRegs;
using graphics::sysv::Args;

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

bool is_flipped(Addr view) { return (rt().send(view, "isFlipped") & 0xff) != 0; }

Rect topleft_rect(Addr view) {
    ViewPayload* v = V(view);
    const Rect f = geometry::standardize(v->frame);
    double parent_h = 0;
    bool parent_flipped = false;
    if (v->superview) {
        ViewPayload* p = V(v->superview);
        parent_h = p ? p->frame.size.height : 0;
        parent_flipped = p && is_flipped(v->superview);
    } else if (v->window) {
        WindowPayload* w = W(v->window);
        parent_h = w ? w->content_rect.size.height : f.size.height;
    }
    const double y = parent_flipped ? f.min_y() : parent_h - f.max_y();
    return geometry::make_rect(f.min_x(), y, f.width(), f.height());
}

void set_string(Addr& slot, Addr value) {
    if (value) rt().retain(value);
    const Addr old = slot;
    slot = value;
    if (old) rt().release(old);
}

gui::ChildKind kind_for(Addr view) {
    ViewPayload* v = V(view);
    if (fnd::is_kind(view, st().button)) return v->is_checkbox ? gui::ChildKind::CheckBox : gui::ChildKind::PushButton;
    if (fnd::is_kind(view, st().text_field)) return v->editable ? gui::ChildKind::TextEdit : gui::ChildKind::Label;
    return gui::ChildKind::Container;
}

std::string str_of(Addr s) {
    std::string_view sv;
    return s && fnd::string_view_of(s, sv) ? std::string(sv) : std::string();
}

}  // namespace

// ---- native tree -------------------------------------------------------------------------------------------------------------------------------------
std::string view_text(Addr view) {
    ViewPayload* v = V(view);
    if (!v) return {};
    if (v->native && st().gui) return st().gui->text(v->native);
    return str_of(v->string_value);
}

void view_apply_text(Addr view) {
    ViewPayload* v = V(view);
    if (!v || !v->native || !st().gui) return;
    st().gui->set_text(v->native, str_of(v->string_value));
}

void view_build_native(Addr view) {
    ViewPayload* v = V(view);
    if (!v || v->native || !v->window || !st().gui) return;
    WindowPayload* w = W(v->window);
    if (!w || !w->native) return;
    gui::NativeId parent = 0;
    if (v->superview) { ViewPayload* p = V(v->superview); parent = p ? p->native : 0; }
    else if (w->content_view == view) parent = w->native;
    if (!parent) return;
    const gui::ChildKind kind = kind_for(view);
    v->native = st().gui->create_child(parent, kind, topleft_rect(view), str_of(v->string_value));
    if (!v->native) return;
    st().view_of_native[v->native] = view;
    if (!v->enabled) st().gui->set_enabled(v->native, false);
    if (v->hidden) st().gui->set_hidden(v->native, true);
    if (kind == gui::ChildKind::CheckBox) st().gui->set_checked(v->native, v->state != 0);
    if (kind == gui::ChildKind::TextEdit) {
        if (!v->selectable) st().gui->set_read_only(v->native, true);
        if (v->placeholder) st().gui->set_placeholder(v->native, str_of(v->placeholder));
    }
    for (Addr s : std::vector<Addr>(v->subviews)) view_build_native(s);
}

static void forget_native_tree(Addr view) {
    ViewPayload* v = V(view);
    if (!v) return;
    if (v->native) st().view_of_native.erase(v->native);
    v->native = 0;
    for (Addr s : v->subviews) forget_native_tree(s);
}

void view_destroy_native(Addr view) {
    ViewPayload* v = V(view);
    if (!v) return;
    const gui::NativeId n = v->native;
    forget_native_tree(view);
    if (n && st().gui) st().gui->destroy(n);
}

void view_rebuild_native(Addr view) {
    ViewPayload* v = V(view);
    if (!v || !v->native) return;
    // the text of an editable field lives in the native control: keep it
    set_string(v->string_value, fnd::new_string(view_text(view)));
    rt().release(v->string_value);
    view_destroy_native(view);
    view_build_native(view);
}

void view_set_window(Addr view, Addr window) {
    ViewPayload* v = V(view);
    if (!v) return;
    if (v->window && !window) view_destroy_native(view);          // leaving the window
    v->window = window;
    for (Addr s : std::vector<Addr>(v->subviews)) view_set_window(s, window);
}

void view_set_frame(Addr view, const Rect& frame) {
    ViewPayload* v = V(view);
    if (!v) return;
    const Size old_size = v->frame.size;
    v->frame = frame;
    const bool size_changed = old_size.width != frame.size.width || old_size.height != frame.size.height;
    if (v->native && st().gui) st().gui->set_rect(v->native, topleft_rect(view));
    // subviews: autoresize when the size changed; re-apply the native rectangle anyway because unflipped children are positioned from the parent top
    for (Addr s : std::vector<Addr>(v->subviews)) {
        ViewPayload* sv = V(s);
        if (!sv) continue;
        const Rect nf = size_changed ? geometry::autoresize(sv->frame, old_size, frame.size, sv->autoresize) : sv->frame;
        view_set_frame(s, nf);
    }
}

void view_attach(Addr view, Addr parent) {
    ViewPayload *v = V(view), *p = V(parent);
    if (!v || !p) return;
    if (view == parent || fnd::is_kind(parent, st().view) == false) { fnd::raise("NSInvalidArgumentException", "-[NSView addSubview:]: invalid view"); return; }
    for (Addr a = parent; a; a = V(a) ? V(a)->superview : 0)
        if (a == view) { fnd::raise("NSInvalidArgumentException", "-[NSView addSubview:]: cannot add a view to its own descendant"); return; }
    if (v->superview == parent) return;
    if (v->superview) view_detach(view);
    rt().retain(view);
    p->subviews.push_back(view);
    v->superview = parent;
    view_set_window(view, p->window);
    if (p->window) view_build_native(view);
}

void view_detach(Addr view) {
    ViewPayload* v = V(view);
    if (!v || !v->superview) return;
    ViewPayload* p = V(v->superview);
    if (p) p->subviews.erase(std::remove(p->subviews.begin(), p->subviews.end(), view), p->subviews.end());
    v->superview = 0;
    rt().retain(view);                                              // like Cocoa: retain/autorelease so the caller can still use the view
    view_set_window(view, 0);
    rt().release(view);                                             // the superview's reference
    rt().autorelease(view);
}

namespace {

// ---- NSResponder ---------------------------------------------------------------------------------------------------------------------------------
Addr r_nextResponder(Addr self, Addr) { return next_responder(self); }
Addr r_forward(Addr self, const char* selector, Addr event) {
    const Addr n = next_responder(self);
    if (n && responds(n, sel(selector))) rt().send(n, sel(selector), {event});
    return 0;
}
#define FORWARDER(fn, name) Addr fn(Addr self, Addr, Addr event) { return r_forward(self, name, event); }
FORWARDER(r_mouseDown, "mouseDown:")
FORWARDER(r_mouseUp, "mouseUp:")
FORWARDER(r_mouseDragged, "mouseDragged:")
FORWARDER(r_mouseMoved, "mouseMoved:")
FORWARDER(r_rightMouseDown, "rightMouseDown:")
FORWARDER(r_rightMouseUp, "rightMouseUp:")
FORWARDER(r_keyDown, "keyDown:")
FORWARDER(r_keyUp, "keyUp:")
#undef FORWARDER
uint64_t r_acceptsFirstResponder(Addr, Addr) { return 0; }
uint64_t r_yes(Addr, Addr) { return 1; }
uint64_t r_no(Addr, Addr) { return 0; }

// ---- NSView --------------------------------------------------------------------------------------------------------------------------------------------
void v_initWithFrame(GuestRegs* r) {
    Args a(r);
    const Addr self = a.next_int();
    a.next_int();
    const Rect f = a.next_rect();
    ViewPayload* v = V(self);
    if (!v) { fnd::raise("NSInvalidArgumentException", "-[NSView initWithFrame:]: receiver is not an NSView"); return; }
    v->frame = f;
    r->ret_rax = self;
}
Addr v_init(Addr self, Addr) { return V(self) ? self : 0; }
Addr v_dealloc(Addr self, Addr) {
    if (ViewPayload* v = V(self)) {
        view_destroy_native(self);
        std::vector<Addr> subs = std::move(v->subviews);
        v->subviews.clear();
        for (Addr s : subs) {
            if (ViewPayload* sv = V(s)) { sv->superview = 0; sv->window = 0; }
            rt().release(s);
        }
        if (v->string_value) rt().release(v->string_value);
        if (v->placeholder) rt().release(v->placeholder);
        v->string_value = v->placeholder = 0;
    }
    if (uint64_t* slot = fnd::payload_slot(self)) { fnd::store_drop(*slot); *slot = 0; }
    rt().destruct_instance(self);
    rt().dispose_instance(self);
    return 0;
}
void v_frame(GuestRegs* r) {                                        // stret: rdi = result, rsi = self
    Args a(r);
    const uint64_t out = a.next_int();
    const Addr self = a.next_int();
    ViewPayload* v = V(self);
    graphics::sysv::ret_rect(r, out, v ? v->frame : geometry::make_rect(0, 0, 0, 0));
}
void v_bounds(GuestRegs* r) {
    Args a(r);
    const uint64_t out = a.next_int();
    const Addr self = a.next_int();
    ViewPayload* v = V(self);
    graphics::sysv::ret_rect(r, out, v ? geometry::make_rect(0, 0, v->frame.size.width, v->frame.size.height) : geometry::make_rect(0, 0, 0, 0));
}
void v_setFrame(GuestRegs* r) {
    Args a(r);
    const Addr self = a.next_int();
    a.next_int();
    const Rect f = a.next_rect();
    view_set_frame(self, f);
}
void v_setFrameSize(GuestRegs* r) {
    Args a(r);
    const Addr self = a.next_int();
    a.next_int();
    const Size s = a.next_size();
    if (ViewPayload* v = V(self)) { Rect f = v->frame; f.size = s; view_set_frame(self, f); }
}
void v_setFrameOrigin(GuestRegs* r) {
    Args a(r);
    const Addr self = a.next_int();
    a.next_int();
    const Point p = a.next_point();
    if (ViewPayload* v = V(self)) { Rect f = v->frame; f.origin = p; view_set_frame(self, f); }
}
Addr v_addSubview(Addr self, Addr, Addr sub) {
    if (!sub || !V(sub)) { fnd::raise("NSInvalidArgumentException", "-[NSView addSubview:]: the subview is nil or not an NSView"); return 0; }
    view_attach(sub, self);
    return 0;
}
Addr v_removeFromSuperview(Addr self, Addr) { view_detach(self); return 0; }
Addr v_superview(Addr self, Addr) { ViewPayload* v = V(self); return v ? v->superview : 0; }
Addr v_window(Addr self, Addr) { ViewPayload* v = V(self); return v ? v->window : 0; }
Addr v_subviews(Addr self, Addr) {
    ViewPayload* v = V(self);
    return v ? fnd::new_array(std::vector<Addr>(v->subviews), false, true) : 0;
}
Addr v_setNeedsDisplay(Addr self, Addr, uint64_t flag) { if (ViewPayload* v = V(self)) v->needs_display = (flag & 0xff) != 0; return 0; }
uint64_t v_needsDisplay(Addr self, Addr) { ViewPayload* v = V(self); return v && v->needs_display ? 1 : 0; }
Addr v_display(Addr self, Addr) { if (ViewPayload* v = V(self)) v->needs_display = false; return 0; }
uint64_t v_isHidden(Addr self, Addr) { ViewPayload* v = V(self); return v && v->hidden ? 1 : 0; }
Addr v_setHidden(Addr self, Addr, uint64_t flag) {
    ViewPayload* v = V(self);
    if (!v) return 0;
    v->hidden = (flag & 0xff) != 0;
    if (v->native && st().gui) st().gui->set_hidden(v->native, v->hidden);
    return 0;
}
int64_t v_tag(Addr self, Addr) { ViewPayload* v = V(self); return v ? v->tag : 0; }
Addr v_setTag(Addr self, Addr, int64_t t) { if (ViewPayload* v = V(self)) v->tag = t; return 0; }
Addr view_with_tag(Addr root, int64_t tag) {
    ViewPayload* v = V(root);
    if (!v) return 0;
    if (v->tag == tag) return root;
    for (Addr s : v->subviews) if (Addr f = view_with_tag(s, tag)) return f;
    return 0;
}
Addr v_viewWithTag(Addr self, Addr, int64_t tag) { return view_with_tag(self, tag); }
uint64_t v_autoresizingMask(Addr self, Addr) { ViewPayload* v = V(self); return v ? v->autoresize : 0; }
Addr v_setAutoresizingMask(Addr self, Addr, uint64_t m) { if (ViewPayload* v = V(self)) v->autoresize = static_cast<unsigned>(m); return 0; }
uint64_t v_isFlipped(Addr, Addr) { return 0; }
uint64_t v_isDescendantOf(Addr self, Addr, Addr other) {
    for (Addr a = self; a; a = V(a) ? V(a)->superview : 0) if (a == other) return 1;
    return 0;
}
uint64_t v_isOpaque(Addr, Addr) { return 0; }
Addr v_drawRect(Addr, Addr) { return 0; }           // drawing is not implemented: custom views are containers without paint

// ---- NSControl -----------------------------------------------------------------------------------------------------------------------------------------
Addr c_target(Addr self, Addr) { ViewPayload* v = V(self); return v ? v->target : 0; }
Addr c_setTarget(Addr self, Addr, Addr t) { if (ViewPayload* v = V(self)) v->target = t; return 0; }
Addr c_action(Addr self, Addr) { ViewPayload* v = V(self); return v ? v->action : 0; }
Addr c_setAction(Addr self, Addr, Addr a) { if (ViewPayload* v = V(self)) v->action = a; return 0; }
uint64_t c_isEnabled(Addr self, Addr) { ViewPayload* v = V(self); return v && v->enabled ? 1 : 0; }
Addr c_setEnabled(Addr self, Addr, uint64_t flag) {
    ViewPayload* v = V(self);
    if (!v) return 0;
    v->enabled = (flag & 0xff) != 0;
    if (v->native && st().gui) st().gui->set_enabled(v->native, v->enabled);
    return 0;
}
Addr c_stringValue(Addr self, Addr) { return fnd::new_string_autoreleased(view_text(self)); }
Addr c_setStringValue(Addr self, Addr, Addr s) {
    ViewPayload* v = V(self);
    if (!v) return 0;
    std::string_view sv;
    if (!s || !fnd::string_view_of(s, sv)) { fnd::raise("NSInvalidArgumentException", "-[NSControl setStringValue:]: argument is not an NSString"); return 0; }
    set_string(v->string_value, s);
    view_apply_text(self);
    return 0;
}
int64_t c_integerValue(Addr self, Addr) { return std::strtoll(view_text(self).c_str(), nullptr, 10); }
Addr c_setIntegerValue(Addr self, Addr, int64_t n) {
    const Addr s = fnd::new_string(std::to_string(n));
    c_setStringValue(self, 0, s);
    rt().release(s);
    return 0;
}
void c_doubleValue(GuestRegs* r) { graphics::sysv::ret_double(r, std::strtod(view_text(r->gp[0]).c_str(), nullptr)); }
void c_setDoubleValue(GuestRegs* r) {
    const double d = graphics::sysv::as_double(r->xmm[0]);
    const Addr s = fnd::new_string(std::format("{}", d));
    c_setStringValue(r->gp[0], 0, s);
    rt().release(s);
}
uint64_t c_sendAction(Addr self, Addr, Addr action, Addr target) { return send_action(action, target, self) ? 1 : 0; }
Addr c_performClick(Addr self, Addr, Addr) {
    ViewPayload* v = V(self);
    if (!v || !v->enabled) return 0;
    if (fnd::is_kind(self, st().button) && v->is_checkbox) { v->state = v->state ? 0 : 1; if (v->native && st().gui) st().gui->set_checked(v->native, static_cast<int>(v->state)); }
    send_action(v->action, v->target, self);
    return 0;
}
Addr c_sizeToFit(Addr self, Addr) {
    ViewPayload* v = V(self);
    if (!v) return 0;
    const std::u16string u = fnd::to_u16(view_text(self));
    Rect f = v->frame;
    f.size = Size{std::max(24.0, 7.0 * static_cast<double>(u.size()) + 20.0), 22.0};
    view_set_frame(self, f);
    return 0;
}
Addr c_ignoreObject(Addr, Addr, Addr) { return 0; }               // setFont:, setAlignment: ..., accepted and ignored (documented)
Addr c_ignoreValue(Addr, Addr, uint64_t) { return 0; }

// ---- NSButton ---------------------------------------------------------------------------------------------------------------------------------------------
Addr b_title(Addr self, Addr) { return fnd::new_string_autoreleased(view_text(self)); }
void b_buttonWithTitleTargetAction(GuestRegs* r) {              // +buttonWithTitle:target:action:  self,_cmd,title,target,action
    Class* c = rt().class_at(r->gp[0]);
    if (!c || c->is_meta || !rt().is_kind_of(c, st().button)) c = st().button;
    const Addr b = rt().create_instance(c, 0);
    ViewPayload* v = b ? V(b) : nullptr;
    if (!v) return;
    v->frame = geometry::make_rect(0, 0, 80, 24);
    set_string(v->string_value, r->gp[2]);
    v->target = r->gp[3];
    v->action = r->gp[4];
    r->ret_rax = fnd::autoreleased(b);
}
int64_t b_state(Addr self, Addr) { ViewPayload* v = V(self); return v ? v->state : 0; }
Addr b_setState(Addr self, Addr, int64_t s) {
    ViewPayload* v = V(self);
    if (!v) return 0;
    v->state = s != 0;
    if (v->native && st().gui && v->is_checkbox) st().gui->set_checked(v->native, static_cast<int>(v->state));
    return 0;
}
Addr b_setButtonType(Addr self, Addr, int64_t type) {
    ViewPayload* v = V(self);
    if (!v) return 0;
    v->button_type = static_cast<int>(type);
    const bool checkbox = type == 3 || type == 4;                    // NSSwitchButton / NSRadioButton (radio groups are not implemented)
    if (checkbox != v->is_checkbox) { v->is_checkbox = checkbox; if (v->native) view_rebuild_native(self); }
    return 0;
}

// ---- NSTextField ---------------------------------------------------------------------------------------------------------------------------------------
Addr label_with(Addr cls_self, const std::string& text, bool wrapping) {
    Class* c = rt().class_at(cls_self);
    if (!c || c->is_meta || !rt().is_kind_of(c, st().text_field)) c = st().text_field;
    const Addr t = rt().create_instance(c, 0);
    ViewPayload* v = t ? V(t) : nullptr;
    if (!v) return 0;
    v->editable = false;
    v->selectable = false;
    v->bezeled = false;
    v->draws_background = false;
    v->frame = geometry::make_rect(0, 0, wrapping ? 200 : std::max(24.0, 7.0 * static_cast<double>(fnd::to_u16(text).size()) + 8.0), wrapping ? 40 : 17);
    set_string(v->string_value, fnd::new_string(text));
    rt().release(v->string_value);
    return fnd::autoreleased(t);
}
Addr t_labelWithString(Addr self, Addr, Addr s) {
    std::string_view sv;
    if (!fnd::string_view_of(s, sv)) { fnd::raise("NSInvalidArgumentException", "+[NSTextField labelWithString:]: argument is not an NSString"); return 0; }
    return label_with(self, std::string(sv), false);
}
Addr t_wrappingLabelWithString(Addr self, Addr, Addr s) {
    std::string_view sv;
    if (!fnd::string_view_of(s, sv)) { fnd::raise("NSInvalidArgumentException", "+[NSTextField wrappingLabelWithString:]: argument is not an NSString"); return 0; }
    return label_with(self, std::string(sv), true);
}
uint64_t t_isEditable(Addr self, Addr) { ViewPayload* v = V(self); return v && v->editable ? 1 : 0; }
Addr t_setEditable(Addr self, Addr, uint64_t flag) {
    ViewPayload* v = V(self);
    if (!v) return 0;
    const bool e = (flag & 0xff) != 0;
    if (e == v->editable) return 0;
    v->editable = e;
    if (v->native) view_rebuild_native(self);               // a different native control (EDIT <-> STATIC)
    return 0;
}
uint64_t t_isSelectable(Addr self, Addr) { ViewPayload* v = V(self); return v && v->selectable ? 1 : 0; }
Addr t_setSelectable(Addr self, Addr, uint64_t flag) {
    ViewPayload* v = V(self);
    if (!v) return 0;
    v->selectable = (flag & 0xff) != 0;
    if (v->native && st().gui && v->editable) st().gui->set_read_only(v->native, !v->selectable);
    return 0;
}
uint64_t t_isBezeled(Addr self, Addr) { ViewPayload* v = V(self); return v && v->bezeled ? 1 : 0; }
Addr t_setBezeled(Addr self, Addr, uint64_t flag) { if (ViewPayload* v = V(self)) v->bezeled = (flag & 0xff) != 0; return 0; }
Addr t_setDrawsBackground(Addr self, Addr, uint64_t flag) { if (ViewPayload* v = V(self)) v->draws_background = (flag & 0xff) != 0; return 0; }
Addr t_placeholderString(Addr self, Addr) { ViewPayload* v = V(self); return v ? v->placeholder : 0; }
Addr t_setPlaceholderString(Addr self, Addr, Addr s) {
    ViewPayload* v = V(self);
    if (!v) return 0;
    set_string(v->placeholder, s);
    if (v->native && st().gui) st().gui->set_placeholder(v->native, str_of(s));
    return 0;
}
Addr t_delegate(Addr self, Addr) { ViewPayload* v = V(self); return v ? v->delegate : 0; }
Addr t_setDelegate(Addr self, Addr, Addr d) { if (ViewPayload* v = V(self)) v->delegate = d; return 0; }
uint64_t t_acceptsFirstResponder(Addr self, Addr) { ViewPayload* v = V(self); return v && v->editable ? 1 : 0; }

}  // namespace

void install_responder_classes(objc::Runtime& r) {
    Class* resp = r.declare_native_class("NSResponder", "NSObject", 16);
    st().responder = resp;
    fnd::classes().extra_payload.push_back(resp);
    r.add_native_methods(resp, false, {
        {"nextResponder", "@16@0:8", addr(r_nextResponder), false},
        {"acceptsFirstResponder", "B16@0:8", addr(r_acceptsFirstResponder), false},
        {"becomeFirstResponder", "B16@0:8", addr(r_yes), false},
        {"resignFirstResponder", "B16@0:8", addr(r_yes), false},
        {"mouseDown:", "v24@0:8@16", addr(r_mouseDown), false},
        {"mouseUp:", "v24@0:8@16", addr(r_mouseUp), false},
        {"mouseDragged:", "v24@0:8@16", addr(r_mouseDragged), false},
        {"mouseMoved:", "v24@0:8@16", addr(r_mouseMoved), false},
        {"rightMouseDown:", "v24@0:8@16", addr(r_rightMouseDown), false},
        {"rightMouseUp:", "v24@0:8@16", addr(r_rightMouseUp), false},
        {"keyDown:", "v24@0:8@16", addr(r_keyDown), false},
        {"keyUp:", "v24@0:8@16", addr(r_keyUp), false},
    });
    (void)r_no;
    Class* view = r.declare_native_class("NSView", "NSResponder", 16);
    st().view = view;
    r.add_native_methods(view, false, {
        {"init", "@16@0:8", addr(v_init), false},
        {"initWithFrame:", "@48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16", addr(v_initWithFrame), true},
        {"dealloc", "v16@0:8", addr(v_dealloc), false},
        {"frame", "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8", addr(v_frame), true},
        {"bounds", "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8", addr(v_bounds), true},
        {"setFrame:", "v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16", addr(v_setFrame), true},
        {"setFrameSize:", "v32@0:8{CGSize=dd}16", addr(v_setFrameSize), true},
        {"setFrameOrigin:", "v32@0:8{CGPoint=dd}16", addr(v_setFrameOrigin), true},
        {"addSubview:", "v24@0:8@16", addr(v_addSubview), false},
        {"removeFromSuperview", "v16@0:8", addr(v_removeFromSuperview), false},
        {"superview", "@16@0:8", addr(v_superview), false},
        {"window", "@16@0:8", addr(v_window), false},
        {"subviews", "@16@0:8", addr(v_subviews), false},
        {"setNeedsDisplay:", "v20@0:8B16", addr(v_setNeedsDisplay), false},
        {"needsDisplay", "B16@0:8", addr(v_needsDisplay), false},
        {"display", "v16@0:8", addr(v_display), false},
        {"isHidden", "B16@0:8", addr(v_isHidden), false},
        {"setHidden:", "v20@0:8B16", addr(v_setHidden), false},
        {"tag", "q16@0:8", addr(v_tag), false},
        {"setTag:", "v24@0:8q16", addr(v_setTag), false},
        {"viewWithTag:", "@24@0:8q16", addr(v_viewWithTag), false},
        {"autoresizingMask", "Q16@0:8", addr(v_autoresizingMask), false},
        {"setAutoresizingMask:", "v24@0:8Q16", addr(v_setAutoresizingMask), false},
        {"isFlipped", "B16@0:8", addr(v_isFlipped), false},
        {"isDescendantOf:", "B24@0:8@16", addr(v_isDescendantOf), false},
        {"isOpaque", "B16@0:8", addr(v_isOpaque), false},
        {"drawRect:", "v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16", addr(v_drawRect), false},
    });
    Class* control = r.declare_native_class("NSControl", "NSView", 16);
    st().control = control;
    r.add_native_methods(control, false, {
        {"target", "@16@0:8", addr(c_target), false},
        {"setTarget:", "v24@0:8@16", addr(c_setTarget), false},
        {"action", ":16@0:8", addr(c_action), false},
        {"setAction:", "v24@0:8:16", addr(c_setAction), false},
        {"isEnabled", "B16@0:8", addr(c_isEnabled), false},
        {"setEnabled:", "v20@0:8B16", addr(c_setEnabled), false},
        {"stringValue", "@16@0:8", addr(c_stringValue), false},
        {"setStringValue:", "v24@0:8@16", addr(c_setStringValue), false},
        {"intValue", "i16@0:8", addr(c_integerValue), false},
        {"integerValue", "q16@0:8", addr(c_integerValue), false},
        {"setIntValue:", "v20@0:8i16", addr(c_setIntegerValue), false},
        {"setIntegerValue:", "v24@0:8q16", addr(c_setIntegerValue), false},
        {"doubleValue", "d16@0:8", addr(c_doubleValue), true},
        {"setDoubleValue:", "v24@0:8d16", addr(c_setDoubleValue), true},
        {"sendAction:to:", "B32@0:8:16@24", addr(c_sendAction), false},
        {"performClick:", "v24@0:8@16", addr(c_performClick), false},
        {"sizeToFit", "v16@0:8", addr(c_sizeToFit), false},
        {"setFont:", "v24@0:8@16", addr(c_ignoreObject), false},
        {"setAlignment:", "v24@0:8q16", addr(c_ignoreValue), false},
        {"setToolTip:", "v24@0:8@16", addr(c_ignoreObject), false},
    });
    Class* button = r.declare_native_class("NSButton", "NSControl", 16);
    st().button = button;
    r.add_native_methods(button, true, {{"buttonWithTitle:target:action:", "@40@0:8@16@24:32", addr(b_buttonWithTitleTargetAction), true}});
    r.add_native_methods(button, false, {
        {"title", "@16@0:8", addr(b_title), false},
        {"setTitle:", "v24@0:8@16", addr(c_setStringValue), false},
        {"state", "q16@0:8", addr(b_state), false},
        {"setState:", "v24@0:8q16", addr(b_setState), false},
        {"setButtonType:", "v24@0:8Q16", addr(b_setButtonType), false},
        {"setBezelStyle:", "v24@0:8Q16", addr(c_ignoreValue), false},
        {"setKeyEquivalent:", "v24@0:8@16", addr(c_ignoreObject), false},
        {"setImage:", "v24@0:8@16", addr(c_ignoreObject), false},
    });
    Class* tf = r.declare_native_class("NSTextField", "NSControl", 16);
    st().text_field = tf;
    r.add_native_methods(tf, true, {
        {"labelWithString:", "@24@0:8@16", addr(t_labelWithString), false},
        {"wrappingLabelWithString:", "@24@0:8@16", addr(t_wrappingLabelWithString), false},
    });
    r.add_native_methods(tf, false, {
        {"isEditable", "B16@0:8", addr(t_isEditable), false},
        {"setEditable:", "v20@0:8B16", addr(t_setEditable), false},
        {"isSelectable", "B16@0:8", addr(t_isSelectable), false},
        {"setSelectable:", "v20@0:8B16", addr(t_setSelectable), false},
        {"isBezeled", "B16@0:8", addr(t_isBezeled), false},
        {"setBezeled:", "v20@0:8B16", addr(t_setBezeled), false},
        {"setDrawsBackground:", "v20@0:8B16", addr(t_setDrawsBackground), false},
        {"placeholderString", "@16@0:8", addr(t_placeholderString), false},
        {"setPlaceholderString:", "v24@0:8@16", addr(t_setPlaceholderString), false},
        {"delegate", "@16@0:8", addr(t_delegate), false},
        {"setDelegate:", "v24@0:8@16", addr(t_setDelegate), false},
        {"setTextColor:", "v24@0:8@16", addr(c_ignoreObject), false},
        {"acceptsFirstResponder", "B16@0:8", addr(t_acceptsFirstResponder), false},
    });
}

}  // namespace cider::appkit
