# CoreGraphics rendering (ORVYN 0.6.1)

A Mach-O program compiled for macOS can call CoreGraphics functions; ORVYN executes them on a Direct2D surface. The program decides **what** to draw, ORVYN only
translates and presents. Nothing in the host draws a scene on behalf of a guest.

```
Objective-C Mach-O x86_64
  -[DrawingView drawRect:]                        (guest code)
        |  CGContextFillRect(ctx, rect) ...        System V call, struct arguments on the stack
        v
cg_api.cpp         guest-visible CG* functions, object handles, argument validation
        v
cg::Context        graphics state stack, CTM, current path, clip stack        (include/orvyn/cg.hpp, cg_context.cpp; pure C++)
        v
cg::Device         fill_path / stroke_path / push_clip / pop_clip / draw_image / draw_text
        v
D2DDevice          Direct2D: HWND render target (windows) or software WIC bitmap target (offscreen backend, tests)
        v
Win32 HWND         real pixels on screen
```

The engine knows nothing about Objective-C, the guest or HWNDs. `Device` has a Direct2D implementation and (in the tests) a recording implementation.

## Why Direct2D

GDI has no antialiasing or transforms for vector drawing. GDI+ has them but is a legacy API (no per-monitor DPI targets, no geometry masks). Direct2D gives
antialiased paths, affine transforms, geometry-masked clipping, DPI-aware targets and a *software* WIC bitmap target that works without a desktop, which is
what makes the headless pixel tests possible. It only needs system DLLs (`d2d1`, `dwrite`, `windowscodecs`). Every COM object the module creates is counted
(`cg::com_objects_alive()`), so tests prove that frames do not leak.

## Guest-visible objects

`CGContextRef`, `CGColorRef`, `CGColorSpaceRef`, `CGPathRef`/`CGMutablePathRef` and `CGImageRef` are ordinary Objective-C objects of private native classes
(`__NSCGContext`, `__NSCGColor`, `__NSCGColorSpace`, `__NSCGPath`, `__NSCGImage`), with their host state in the payload store like the Foundation classes.
Consequences:

* `CFRetain` / `CFRelease` / `CFGetRetainCount` and `CGColorRetain` / `CGColorRelease` ... are the runtime's retain/release; the "Create" rule applies (+1).
* A handle is accepted only if it is a **live** object of the right class (`is_live_object` + class check). A NULL pointer, a garbage pointer, a live object of
  another class (an `NSString` passed as a context), an already released object (double release, use after release) and a `CGPathRef` used where a
  `CGMutablePathRef` is required are **refused**: the call does nothing, a warning is logged and `cg_invalid_handle_events()` is incremented. Apple's
  CoreGraphics also logs and returns for a NULL context; ORVYN additionally refuses forged and dead pointers instead of dereferencing them. `NULL` passed to a
  `*Release` function is a silent no-op, like Apple's.
* Ownership: objects a program creates belong to it; the `CGContext` obtained from `NSGraphicsContext` belongs to the repaint (see below). Objects that are still
  alive when the guest ends are counted by `objc_live_objects` and named by `objc_live_summary`.

## Implemented functions

All names and argument orders follow Apple's public API (verified from the public documentation; ORVYN ships no Apple header, the fixtures use hand-written
declarations in `tests/fixtures/objc/include/CoreGraphics/CoreGraphics.h`).

| Area | Functions |
|---|---|
| Color space | `CGColorSpaceCreateDeviceRGB`, `Retain`, `Release`, `GetNumberOfComponents` (DeviceRGB only) |
| Color | `CGColorCreate`, `CGColorCreateGenericRGB`, `CGColorCreateCopy`, `Retain`, `Release`, `GetNumberOfComponents`, `GetComponents`, `GetAlpha` |
| Path | `CGPathCreateMutable`, `CreateCopy`, `CreateMutableCopy`, `Retain`, `Release`, `MoveToPoint`, `AddLineToPoint`, `AddCurveToPoint`, `AddQuadCurveToPoint`, `AddRect`, `AddEllipseInRect`, `AddArc`, `AddPath`, `CloseSubpath`, `IsEmpty`, `GetBoundingBox`, `GetCurrentPoint`, `ContainsPoint`, `CreateWithRect`, `CreateWithEllipseInRect` |
| Image | `CGImageGetWidth`, `GetHeight`, `Retain`, `Release`, `CGContextDrawImage` — **no guest-side way to create an image exists yet** (`CGBitmapContextCreate`, `CGImageCreate*`, image decoding arrive with NSImage); the functions work on host-created images only |
| Transform | `CGAffineTransformMake`, `MakeTranslation`, `MakeScale`, `MakeRotation`, `Concat`, `Translate`, `Scale`, `Rotate`, `Invert`, `IsIdentity`, `EqualToTransform`, `CGPointApplyAffineTransform`, `CGSizeApplyAffineTransform`, `CGRectApplyAffineTransform`, data symbol `CGAffineTransformIdentity` |
| Context state | `CGContextRetain`, `Release`, `SaveGState`, `RestoreGState`, `SetFillColorWithColor`, `SetStrokeColorWithColor`, `SetRGBFillColor`, `SetRGBStrokeColor`, `SetGrayFillColor`, `SetGrayStrokeColor`, `SetAlpha`, `SetLineWidth`, `SetLineCap`, `SetLineJoin`, `SetMiterLimit`, `SetLineDash`, `SetShouldAntialias`, `SetInterpolationQuality` (none / default only), `Flush`, `Synchronize` (no-ops: a window context is presented when its drawing pass ends) |
| Context painting | `FillRect`, `StrokeRect`, `StrokeRectWithWidth`, `ClearRect`, `FillEllipseInRect`, `StrokeEllipseInRect`, `StrokePath`, `FillPath`, `EOFillPath`, `DrawPath`, `DrawImage` |
| Context path | `BeginPath`, `ClosePath`, `MoveToPoint`, `AddLineToPoint`, `AddCurveToPoint`, `AddQuadCurveToPoint`, `AddRect`, `AddRects`, `AddEllipseInRect`, `AddArc`, `AddArcToPoint`, `AddPath` |
| Context clip / CTM | `Clip`, `EOClip`, `ClipToRect`, `TranslateCTM`, `ScaleCTM`, `RotateCTM`, `ConcatCTM`, `GetCTM` |

**Not implemented** (absent from the symbol table, so a program importing one is refused when loaded — `cg_unimplemented` proves it for
`CGContextDrawLinearGradient`): bitmap, PDF and layer contexts, gradients, shadings, patterns, shadows, blend modes, `CGContextSetFillColorSpace`,
`CGContextSetFillColor` (component arrays), color spaces other than DeviceRGB, color conversion, `CGContextShowText*`, `CGContextSelectFont`, `CGPathApply`,
`CGPathCreateCopyByStrokingPath`, `CGContextReplacePathWithStrokedPath`, `CGContextGetClipBoundingBox`, `CGContextGetPathCurrentPoint`, `CGContextDrawPDFPage`.

## System V ABI (macOS x86_64) — what the handlers assume and what is tested

| Type | Size | Class | Passed | Returned |
|---|---|---|---|---|
| `CGFloat` | 8 (double) | SSE | xmm0..xmm7, then stack | xmm0 |
| `CGPoint`, `CGSize` | 16 | SSE, SSE | two consecutive xmm registers | xmm0:xmm1 |
| `CGRect` | 32 | MEMORY | copied onto the stack, in argument order | hidden pointer in rdi (also returned in rax); real arguments start at rsi |
| `CGAffineTransform` | 48 | MEMORY | copied onto the stack, in argument order | hidden pointer in rdi |
| `const CGAffineTransform *`, `const CGFloat *` | 8 | INTEGER | rdi.. | — |

Integer and SSE arguments consume their own register files independently; MEMORY structs and overflowing arguments share the stack area in call order
(`graphics/sysv_args.hpp`: `next_int`, `next_double`, `next_point`, `next_size`, `next_rect`, `next_affine`, `ret_rect`, `ret_point`, `ret_affine`).
`tests/fixtures/appkit/src/cg_abi_test.m` is compiled by clang for macOS and checks, from the guest, `CGAffineTransform` in and out (`Make`, `Concat`,
`Translate`, `Scale`, `Rotate`, `Invert`), `CGPoint`/`CGSize`/`CGRect` mixed with transforms (`CGRectApplyAffineTransform`: rect + transform in, rect out),
pointers to structs, `CGPathGetBoundingBox` (hidden pointer + argument), `CGPathGetCurrentPoint` (point result), `CGPathContainsPoint` (pointer, pointer, point, bool),
`CGPathAddArc` / `AddCurveToPoint` (eight and six doubles, no stack arguments) and 5000 iterations with six integers and two doubles live across calls (callee-saved
registers are preserved). `cg_draw_test` passes an `NSRect` on the stack to `-drawRect:` and calls `CGContextGetCTM` (hidden pointer + context argument).
Not covered: structs containing `float` members (CoreGraphics on x86_64 macOS uses `double`), more than eight SSE arguments, `long double`.

## Semantics decisions (differences from Apple are listed)

* **Coordinates.** User space is y-up with the origin at the bottom-left of an unflipped view, y-down at the top-left of a flipped one. The *base* transform
  (y flip for unflipped views and the points-to-pixels scale) is supplied when the context is created; `CGContextGetCTM` returns the user CTM **relative to
  that base** (identity for a fresh context). Apple returns the full user-to-device matrix, which for a retina view includes the backing scale and, for an
  unflipped view, no flip. Programs that only compose CTMs behave identically; programs that read `GetCTM().a` to learn the backing scale differ.
* **Paths** are stored already transformed by the CTM that was current when each point was added (as CoreGraphics does), so a later `CTM` change does not move a
  path that was already built.
* **Stroke width and dash** are scaled by `sqrt(|det(ctm)|)`: a non-uniform scale gives a uniform average width, not an elliptical pen (Apple strokes with the
  transformed pen).
* **Arcs** connect to the current point with a line (`CGContextAddArc`, `CGPathAddArc`, `AddArcToPoint`), like CoreGraphics. Angles are in user space: in a flipped
  view the same call looks mirrored, as on macOS.
* **Clips** are strictly nested and belong to the graphics state that created them: `RestoreGState` pops exactly those; a context destroyed with clips
  pushed (an unbalanced `SaveGState` in the guest) pops them all (`test_cg`, `fuzz_cg`, `cg_lifecycle`). Rectangle clips under axis-aligned transforms use
  `PushAxisAlignedClip`; others use a geometry layer.
* `CGContextClearRect` clears to transparent; under a rotating CTM the cleared area is the axis-aligned bounding box of the rectangle.
* `CGContextRestoreGState` without a matching save is refused (counted), nothing is popped.
* `CGContextSetLineDash` with an odd count repeats the pattern; negative values or an all-zero pattern leave the line solid.
* `CGContextSetAlpha` multiplies the alpha of fill and stroke colors; no transparency layers.
* Text drawing through CoreGraphics does not exist; the device has `draw_text` (DirectWrite) for AppKit's later text work.

## Failure policy

Direct2D errors (`D2DERR_RECREATE_TARGET`, creation failures) mark the device not ok; a window surface is dropped and recreated on the next paint. The guest
never sees a Windows error. An exception cannot cross the bridge: the handlers do not throw. A guest that calls `exit()` or traps inside drawing unwinds through
`call_guest_nested`; the paint pass ends the Direct2D frame and invalidates the context.

## Tests

`test_cg` (engine and Direct2D bitmap pixels), `test_cg_abi` (ABI and handle safety), `test_cg_rendering` (Mach-O scene rendered and compared pixel by pixel,
flipped, resized, DPI 2x, redraw, leaks, lifecycle, real windows GDI/USER/COM counts), `test_gui_win32` (real window captured with `PrintWindow`),
`test_runtime` (the exact symbol registry), `fuzz_cg`. See [TESTING.md](TESTING.md).
