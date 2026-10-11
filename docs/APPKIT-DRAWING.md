# AppKit drawing: `NSView -drawRect:` (ORVYN 0.6.2)

A custom `NSView` subclass that overrides `-drawRect:` is drawn by its own Objective-C code, through the CoreGraphics functions described in
[COREGRAPHICS-RENDERING.md](COREGRAPHICS-RENDERING.md), on a real Windows window.

## Paint flow

```
Win32:      WM_PAINT on the view's ORVYNView child window            Offscreen:  invalid view found by pump() / display_now()
                 |                                                                  |
                 +---- Backend::Events::paint(view, dirty) ---------------------------+
                                  |                                   (backend prepared a Direct2D surface: paint_device(), paint_scale())
                                  v
                  appkit view_paint(view)       appkit/appkit_draw.cpp
                    1. view.painting = true (a view is never painted re-entrantly)
                    2. CGContext over the surface; base = view_base(height, [view isFlipped], scale); clip = the dirty rectangle
                    3. NSGraphicsContext around it, pushed as +currentContext
                    4. abi::call_guest_nested(-drawRect:) with self, _cmd and the NSRect on the stack (System V MEMORY class)
                    5. pop the context, invalidate the CGContext (a pointer the guest kept can no longer draw), release everything
                                  |
                                  v
                  backend ends the Direct2D frame: EndDraw presents (Win32) / the bitmap keeps the pixels (offscreen)
```

* A view is **custom-painted** when its class overrides `-drawRect:` (checked when its native window is created). Plain `NSView`s, controls and containers keep
  the old behavior (a native child window painted by Windows). `NSButton`/`NSTextField` are not custom-painted: subclasses of them that override
  `-drawRect:` are not drawn by it yet.
* **Each custom-painted view has its own HWND** (`ORVYNView`, `WS_CLIPCHILDREN | WS_CLIPSIBLINGS`) and its own surface. Subviews that are native controls sit on
  top of it as child windows; a custom view inside another custom view is its own window drawn independently by Windows (there is no parent-draws-children
  pass: `-drawRect:` of a parent does not paint its subviews, and `isOpaque`/`wantsLayer` are ignored).
* **`dirtyRect`** passed to `-drawRect:` is always the whole view bounds. Direct2D presents the surface as a whole, so a partial invalidation
  (`setNeedsDisplayInRect:`) is widened to the whole view. Drawing is clipped to the view bounds. The backend clears the surface to the face color
  (240, 240, 240) before the guest draws; a view that paints its dirty rectangle (the normal idiom) covers it.
* **Coordinates.** Unflipped views (the default): origin at the bottom-left, y up; flipped views (`-isFlipped` returning YES): origin top-left, y down. The base
  transform handles the flip and the monitor scale (points to pixels), so guest code never sees pixels. Scale changes (`WM_DPICHANGED` / the offscreen
  `scale` command) recreate the surface at the new size and repaint.
* **Invalidation.** `-setNeedsDisplay:YES` marks the view and asks the backend for a repaint (`InvalidateRect` → `WM_PAINT`; offscreen: dirty flag handled by the
  next `pump()`); `-display` repaints synchronously (`InvalidateRect` + `UpdateWindow`); `-displayIfNeeded` only when marked. A request made **from inside
  `-drawRect:` of the same view is ignored**, so a view cannot make itself repaint forever (Cocoa warns about the same thing). Windows validates the region
  after each `WM_PAINT`, so there is no permanent repaint loop.
* **Resize.** The view's frame follows autoresizing; `WM_SIZE` resizes the Direct2D target and the view is repainted with the new bounds (`dirtyRect` equals
  the new bounds).

## Context life cycle and affinity

* One context per repaint, created for exactly the view being painted. It is never shared between views or windows and never outlives its surface.
* `+[NSGraphicsContext currentContext]` is non-nil only inside `-drawRect:`; `-CGContext` / `-graphicsPort` return a pointer owned by the NSGraphicsContext.
  A guest may `CGContextRetain` it; after `-drawRect:` returns the context is **invalid**: further CG calls on it are refused and counted (they never reach
  the device) and `CGContextRelease` is fine.
* A window can be closed from inside `-drawRect:` (`[[self window] close]`): the surface and the native window disappear during the pass; the pass finishes on
  the surface it owns and ends the frame; the view is retained for the duration of the call.
* Everything runs on the main guest thread, which owns every window and every Direct2D resource (single-threaded Direct2D factory). Drawing from another guest
  thread is not supported; `NSGraphicsContext` is main-thread only.
* If the guest unbalances `SaveGState` / `ClipTo...`, the context pops the leftover clips when it dies.

## Example

```objc
@interface DrawingView : NSView
@end
@implementation DrawingView
- (void)drawRect:(NSRect)dirtyRect {
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextSetRGBFillColor(ctx, 1.0, 1.0, 0.8, 1.0);
    CGContextFillRect(ctx, dirtyRect);                    /* NSRect arrives on the stack */
    CGContextSetRGBFillColor(ctx, 1, 0, 0, 1);
    CGContextFillRect(ctx, CGRectMake(20, 20, 80, 40));   /* 20 points from the left and BOTTOM edge in an unflipped view */
}
@end
```

The complete reference program is `tests/fixtures/appkit/src/cg_draw_test.m`: background, red and blue rectangles, a diagonal line, a green ellipse, a `CGPath`
triangle, a rotated square through a `CGAffineTransform`, a clipped region and a stroked rectangle, plus a **Redraw** button whose action toggles a variable of
the program and calls `-setNeedsDisplay:`.

## Building and running the fixture

```
tools\llvm-env.cmd tools\gen-appkit-fixtures.cmd        # clang + ld64.lld, no Apple SDK: writes tests\fixtures\appkit\bin\<name>\<name>
build\bin\Release\orvyn.exe run tests\fixtures\appkit\bin\cg_draw_test\cg_draw_test            # real window
build\bin\Release\orvyn.exe run tests\fixtures\appkit\bin\cg_draw_test\cg_draw_test flipped    # same commands, flipped view
```

The window is titled "ORVYN — CoreGraphics Drawing". Pressing **Redraw** turns the bottom-left rectangle from red to blue; resizing the window repaints it.
Programs are compiled against *hand-written* headers (`tests/fixtures/objc/include`) and `.tbd` stubs (`sysroot`); they are not Apple's SDK.

## Backends and verification

* Win32: `win32_backend.cpp`, a Direct2D `ID2D1HwndRenderTarget` per custom view, `WM_PAINT` / `WM_ERASEBKGND` (returns 1) / `WM_SIZE`.
  `test_gui_win32` captures the window with `PrintWindow(PW_RENDERFULLCONTENT)` from another process and compares pixels.
* Offscreen: `offscreen_backend.cpp`, a WIC bitmap per custom view; script commands `snapshot <file.png>` (composite of the custom-painted views, native
  controls are not rasterized), `resize <w> <h>`, `scale <factor>`. The tests decode the PNG and check pixels with tolerances: exact colors where the
  rasterizer cannot differ (interiors), `< 40` for black strokes (antialiasing). A blank image cannot pass: the scene checks include non-background colors at
  specific positions, and the image must differ from the background in more than 3000 pixels.

## Not implemented (drawing)

`NSColor`, `NSBezierPath`, `NSRectFill` and the other AppKit drawing helpers; `NSImage` drawing; text drawing (`NSString drawAtPoint:`, `NSAttributedString`);
`bounds` origin/rotation and `setBoundsOrigin:`/`scaleUnitSquareToSize:`; `convertPoint:fromView:` family; `isOpaque` optimization; layer-backed views,
`CALayer`, `wantsLayer`; `lockFocus`/`unlockFocus` and `cacheDisplayInRect:`; drawing from background threads; partial dirty rectangles; `NSView` subclasses of
controls drawing themselves.
