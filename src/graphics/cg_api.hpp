#pragma once
// Guest-visible CoreGraphics objects and functions (ORVYN 0.6.1). See docs/COREGRAPHICS-RENDERING.md.
//
// Object model: CGContextRef, CGColorRef, CGColorSpaceRef, CGPathRef / CGMutablePathRef and CGImageRef are ordinary Objective-C objects of private native classes
// (like the Foundation classes, "toll-free bridged by construction"), so CFRetain / CFRelease and the CG*Retain / CG*Release functions are the runtime's
// retain / release, the live-object counter covers them, and a pointer that is not a live object of the right class is refused instead of dereferenced.
#include <cstdint>
#include <vector>

#include "orvyn/cg.hpp"
#include "orvyn/libsystem.hpp"
#include "orvyn/objc.hpp"

namespace orvyn::graphics {

void add_cg_api_symbols(std::vector<libsystem::HostSymbol>& table);
void install_cg_api(objc::Runtime& rt);                               // classes and the CGAffineTransformIdentity constant

// A context over `device` (not owned; it must outlive the context or be detached first with cg_context_invalidate). +1.
objc::Addr cg_context_create(cg::Device* device, const cg::Affine& base, cg::Size size);
cg::Context* cg_context_engine(objc::Addr ctx);                       // the engine behind a live context, nullptr when `ctx` is not one or was invalidated
void cg_context_invalidate(objc::Addr ctx);                           // the end of a repaint: pops the clips, forgets the device; every later call is refused
// Wraps decoded pixels as a CGImage (+1); nullptr-address when the image is invalid.
objc::Addr cg_image_create(const cg::Image& image);
const cg::Image* cg_image_pixels(objc::Addr image);

// Counters for tests and leak checks.
uint64_t cg_invalid_handle_events();                                  // calls refused because of a nil, dead or wrong-class handle, or a stale context
size_t cg_live_objects();                                              // CG objects currently alive (contexts, colors, spaces, paths, images)

}  // namespace orvyn::graphics
