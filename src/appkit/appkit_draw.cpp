// View drawing (ORVYN 0.6.2): the path from a native repaint request to the guest's -drawRect:, and NSGraphicsContext.
//
//   WM_PAINT / offscreen repaint -> Backend Events::paint -> view_paint:
//     1. take the surface the backend prepared (paint_device(), valid only during the event) and its pixel scale;
//     2. create a CGContext over it (engine + base transform: y flip for unflipped views, points -> pixels) and an NSGraphicsContext around that;
//     3. make the NSGraphicsContext current, call [view drawRect:] in the guest with the dirty rectangle (a CGRect passed on the stack, System V MEMORY class);
//     4. pop the context, INVALIDATE the CGContext (a guest that kept the pointer can no longer draw), release the temporaries.
// The context belongs to the repaint: it is never shared between views or windows and never outlives the surface. Drawing is single threaded (the main guest
// thread) and a view that is already being painted is not painted again (no re-entrant -drawRect:); asking for a repaint from inside -drawRect: is ignored so
// a view cannot make itself repaint forever.
#include "../graphics/cg_api.hpp"
#include "appkit_internal.hpp"
#include "appkit_install.hpp"
#include "orvyn/abi.hpp"
#include "orvyn/log.hpp"

namespace orvyn::appkit {

bool view_is_flipped(Addr view) { return (rt().send(view, "isFlipped") & 0xff) != 0; }

bool has_custom_draw(Addr view) {
    Class* c = rt().class_of_object(view);
    if (!c || !st().view) return false;
    const Addr s = sel("drawRect:");
    const Addr mine = rt().find_imp(c, s), base = rt().find_imp(st().view, s);
    return mine != 0 && mine != base;
}

void view_invalidate(Addr view) {
    ViewPayload* v = V(view);
    if (!v) return;
    if (v->painting) return;                                   // from inside -drawRect: of the same view: ignored (see the header comment)
    v->needs_display = true;
    if (v->native && st().gui) st().gui->invalidate(v->native);
}

void view_display(Addr view) {
    ViewPayload* v = V(view);
    if (!v || v->painting) return;
    v->needs_display = true;
    if (v->native && st().gui) st().gui->display_now(v->native);       // paints synchronously when the view is custom-painted
    else v->needs_display = false;                                     // not on screen: nothing to draw
}

namespace {

using abi::GuestRegs;
using graphics::sysv::Args;

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

struct GcPayload : fnd::Payload {
    Addr cg = 0;                                               // retained CGContext object
    bool flipped = false;
};
GcPayload* G(Addr o) {
    if (!o || !st().graphics_context || !fnd::is_kind(o, st().graphics_context)) return nullptr;
    return dynamic_cast<GcPayload*>(fnd::payload_of(o));
}

Addr new_gc(Addr cg_context, bool flipped) {                    // +1; takes its own reference on the CGContext
    const Addr o = rt().create_instance(st().graphics_context, 0);
    uint64_t* slot = o ? fnd::payload_slot(o) : nullptr;
    if (!slot) { if (o) rt().release(o); return 0; }
    auto p = std::make_unique<GcPayload>();
    p->cg = rt().retain(cg_context);
    p->flipped = flipped;
    *slot = fnd::store_add(std::move(p));
    return o;
}

Addr gc_dealloc(Addr self, Addr) {
    if (GcPayload* p = G(self)) { if (p->cg) rt().release(p->cg); p->cg = 0; }
    if (uint64_t* slot = fnd::payload_slot(self)) { fnd::store_drop(*slot); *slot = 0; }
    rt().destruct_instance(self);
    rt().dispose_instance(self);
    return 0;
}
Addr gc_currentContext(Addr, Addr) { return st().gc_stack.empty() ? 0 : st().gc_stack.back(); }
Addr gc_setCurrentContext(Addr, Addr, Addr ctx) {
    if (ctx && !G(ctx)) { fnd::raise("NSInvalidArgumentException", "+[NSGraphicsContext setCurrentContext:]: argument is not an NSGraphicsContext"); return 0; }
    if (st().gc_stack.empty()) { if (ctx) st().gc_stack.push_back(ctx); }
    else if (ctx) st().gc_stack.back() = ctx;
    else st().gc_stack.pop_back();
    return 0;
}
void gc_withCGContextFlipped(GuestRegs* r) {                     // +graphicsContextWithCGContext:flipped:  self, _cmd, cgctx, flipped
    if (!graphics::cg_context_engine(r->gp[2])) { fnd::raise("NSInvalidArgumentException", "+[NSGraphicsContext graphicsContextWithCGContext:flipped:]: invalid CGContext"); return; }
    r->ret_rax = fnd::autoreleased(new_gc(r->gp[2], (r->gp[3] & 0xff) != 0));
}
Addr gc_CGContext(Addr self, Addr) { GcPayload* p = G(self); return p ? p->cg : 0; }
uint64_t gc_isFlipped(Addr self, Addr) { GcPayload* p = G(self); return p && p->flipped ? 1 : 0; }
Addr gc_flushGraphics(Addr, Addr) { return 0; }
uint64_t gc_isDrawingToScreen(Addr, Addr) { return 1; }
Addr gc_save_instance(Addr self, Addr) { if (GcPayload* p = G(self)) if (cg::Context* c = graphics::cg_context_engine(p->cg)) c->save(); return 0; }
Addr gc_restore_instance(Addr self, Addr) { if (GcPayload* p = G(self)) if (cg::Context* c = graphics::cg_context_engine(p->cg)) c->restore(); return 0; }
Addr gc_save_class(Addr, Addr) { return st().gc_stack.empty() ? 0 : gc_save_instance(st().gc_stack.back(), 0); }
Addr gc_restore_class(Addr, Addr) { return st().gc_stack.empty() ? 0 : gc_restore_instance(st().gc_stack.back(), 0); }

}  // namespace

void view_paint(Addr view) {
    ViewPayload* v = V(view);
    if (!v || v->painting || !st().gui) return;
    cg::Device* device = st().gui->paint_device();
    if (!device) return;
    if (!has_custom_draw(view)) return;
    const double scale = st().gui->paint_scale();
    const Size size = v->frame.size;
    const bool flipped = view_is_flipped(view);
    const Addr cgctx = graphics::cg_context_create(device, cg::view_base(size.height, flipped, scale), size);
    if (!cgctx) return;
    if (cg::Context* engine = graphics::cg_context_engine(cgctx)) engine->clip_to_rect(geometry::make_rect(0, 0, size.width, size.height));    // AppKit clips to the dirty rectangle
    const Addr gc = new_gc(cgctx, flipped);
    rt().release(cgctx);                                       // the NSGraphicsContext holds the reference now
    if (!gc) return;
    const Addr imp = rt().find_imp(rt().class_of_object(view), sel("drawRect:"));
    rt().retain(view);                                         // the view must outlive its own drawing even if the guest drops its last reference inside it
    const size_t depth = st().gc_stack.size();
    st().gc_stack.push_back(gc);
    v->painting = true;
    v->needs_display = false;
    // -drawRect: (NSRect dirtyRect): self, _cmd in rdi/rsi; the 32-byte NSRect is class MEMORY and travels on the stack
    abi::GuestCallArgs args;
    args.push_int(view).push_int(sel("drawRect:"));
    const double rect[4] = {0, 0, size.width, size.height};
    for (double d : rect) args.stack.push_back(graphics::sysv::as_bits(d));
    const abi::CallOutcome o = abi::call_guest_nested(imp, args);
    if (o.refused) log::warn(log::Category::Objc, "-drawRect: was not called: {}", o.refusal);
    if (ViewPayload* again = V(view)) again->painting = false;
    st().gc_stack.resize(depth);                               // a guest that unbalanced setCurrentContext: cannot leave a stale entry behind
    graphics::cg_context_invalidate(G(gc) ? G(gc)->cg : 0);
    rt().release(gc);
    rt().release(view);
}

void install_drawing_classes(objc::Runtime& r) {
    Class* gc = r.declare_native_class("NSGraphicsContext", "NSObject", 16);
    st().graphics_context = gc;
    fnd::classes().extra_payload.push_back(gc);
    r.add_native_methods(gc, true, {
        {"currentContext", "@16@0:8", addr(gc_currentContext), false},
        {"setCurrentContext:", "v24@0:8@16", addr(gc_setCurrentContext), false},
        {"graphicsContextWithCGContext:flipped:", "@32@0:8^{CGContext=}16B24", addr(gc_withCGContextFlipped), true},
        {"saveGraphicsState", "v16@0:8", addr(gc_save_class), false},
        {"restoreGraphicsState", "v16@0:8", addr(gc_restore_class), false},
        {"currentContextDrawingToScreen", "B16@0:8", addr(gc_isDrawingToScreen), false},
    });
    r.add_native_methods(gc, false, {
        {"dealloc", "v16@0:8", addr(gc_dealloc), false},
        {"CGContext", "^{CGContext=}16@0:8", addr(gc_CGContext), false},
        {"graphicsPort", "^v16@0:8", addr(gc_CGContext), false},
        {"isFlipped", "B16@0:8", addr(gc_isFlipped), false},
        {"flushGraphics", "v16@0:8", addr(gc_flushGraphics), false},
        {"saveGraphicsState", "v16@0:8", addr(gc_save_instance), false},
        {"restoreGraphicsState", "v16@0:8", addr(gc_restore_instance), false},
    });
}

}  // namespace orvyn::appkit
