# CoreGraphics geometry and coordinate conversion (CIDER 0.5.6)

Scope: **geometry only**. There is no drawing (`CGContext`, paths, colours, images, fonts): custom views are containers without paint (`drawRect:` is never called).
Code: `include/cider/geometry.hpp` + `src/graphics/geometry.cpp` (pure, tested by `tests/unit/test_geometry.cpp`), the guest-visible functions in `src/graphics/cg_bridge.cpp`.

## The two coordinate systems are not the same

| | AppKit / CoreGraphics | Win32 |
|---|---|---|
| screen origin | bottom-left of the **primary** display | top-left of the virtual desktop |
| y axis | up | down |
| window position | frame origin = bottom-left corner of the window | left/top of the window rectangle |
| units | points (logical) | device pixels |
| view origin | bottom-left of the view unless `isFlipped` | top-left of the client area |

CIDER converts explicitly at the backend boundary (`gui_backend.hpp` documents which space each argument is in):

* `screen_to_win32(rect, scale, primary_height_px)`: `top = H - max_y*scale`, `bottom = H - min_y*scale` (the flip pivots on the primary display height, which is also how macOS defines global
  coordinates, so multiple monitors keep their relative placement).
* `child_to_client(frame, parent_height, parent_flipped, scale)`: an unflipped parent measures y from its **bottom** edge, so `top = parent_h - frame.max_y`; a flipped parent already uses top-left.
* `autoresize(frame, old_size, new_size, mask)`: `NSViewMinXMargin/WidthSizable/MaxXMargin/MinYMargin/HeightSizable/MaxYMargin`; the extra space is shared evenly between the flexible parts of an axis.

## DPI and "Retina"

A point is one device-independent unit at 96 dpi: `scale = dpi / 96` (`scale_for_dpi`, clamped to 0.5..8, 0 means 96), and `to_pixels` rounds half away from zero. The windows are created per-monitor DPI aware
(manifest `PerMonitorV2` plus a runtime fallback). `WM_DPICHANGED` resizes the window to the rectangle Windows suggests, recreates the control font for the new dpi and asks the AppKit layer to re-apply every child
rectangle. `-[NSScreen backingScaleFactor]` returns the primary monitor scale; there is no 2x backing store: a point is `scale` pixels, not a Retina pixel pair. Multiple monitors: only the primary display is
modelled (`NSScreen mainScreen`); `screens` does not exist.

## Exported functions

`CGRectGetMinX/MidX/MaxX/MinY/MidY/MaxY/Width/Height, CGPointEqualToPoint, CGSizeEqualToSize, CGRectEqualToRect, CGRectIsEmpty/IsNull/IsInfinite, CGRectContainsPoint, CGRectContainsRect,
CGRectIntersectsRect, CGRectStandardize, CGRectIntegral, CGRectInset, CGRectOffset, CGRectUnion, CGRectIntersection, CGRectDivide`; data `CGPointZero, CGSizeZero, CGRectZero, CGRectNull, CGRectInfinite`.
Foundation's `NSPointInRect, NSMouseInRect, NSContainsRect, NSIsEmptyRect, NSEqualRects/Points/Sizes, NSIntersectionRect, NSUnionRect, NSInsetRect, NSOffsetRect, NSIntegralRect,
NSStringFromRect/Point/Size`, `NSZeroPoint/Size/Rect` (on 64-bit `NSRect` is `CGRect`).

System V details (they decide whether the arguments arrive correctly): a `CGPoint`/`CGSize` (two doubles) travels in two consecutive xmm registers; a `CGRect` (32 bytes) is class MEMORY and is copied onto the stack in
argument order; a `CGRect` result comes back through a hidden pointer in `rdi` (also returned in `rax`). All of these functions are raw host handlers (`src/graphics/sysv_args.hpp` walks the registers and the stack
area). `cf_basic.m` calls every one of them from clang-generated code.

## Not implemented

Affine transforms, `CGRectMakeWithDictionaryRepresentation`/dictionary forms, the "nearly equal" functions, `NSRange` helpers, `NSDivideRect`, bounds origin/rotation/scale of views (bounds always start at (0,0) with the frame size).
