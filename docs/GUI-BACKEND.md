# GUI backends (ORVYN 0.5.8, 0.5.11, 0.6.2)

`src/graphics/gui_backend.hpp` is the seam between AppKit semantics (`src/appkit`) and a native windowing system. The AppKit layer decides *what exists* (windows, views, controls, frames, titles, state) and
asks a `Backend` to make it exist; the backend reports input through `Events` callbacks. Everything runs on the main guest thread, which owns every window.

```
Mach-O Objective-C program ──objc_msgSend──> AppKit classes (host, src/appkit) ──Backend interface──> Win32 backend ──> real HWNDs
                                                       ^                                   |
                                                       └──── Events: click, close, resize, mouse, key ──┘
```

## Win32 backend (`win32_backend.cpp`)

* Window classes `ORVYNWindow` (top level, `WS_EX_APPWINDOW`) and `ORVYNView` (container child) registered with `RegisterClassExW`; windows by `CreateWindowExW`; controls are the standard `BUTTON`
  (push button / auto check box), `STATIC` (labels) and `EDIT` (single-line text) created as children, so they look and behave natively (the manifest of `orvyn.exe` enables visual styles).
* `NSWindowStyleMask` → `WS_CAPTION | WS_SYSMENU`, `WS_MINIMIZEBOX`, `WS_THICKFRAME | WS_MAXIMIZEBOX`; borderless → `WS_POPUP`. The outer size comes from `AdjustWindowRectExForDpi`.
* Messages handled: `WM_CLOSE` (asks the AppKit layer; refusal keeps the window), `WM_DESTROY`/`WM_NCDESTROY` (forget the native tree), `WM_SIZE` (→ `windowDidResize:`, content view resize and autoresizing), `WM_MOVE`,
  `WM_ACTIVATE`, `WM_GETMINMAXINFO` (`setMinSize:`), `WM_DPICHANGED` (resize to the suggested rectangle, new font, re-apply child rectangles), `WM_COMMAND`/`BN_CLICKED` (→ control click),
  `WM_CTLCOLORSTATIC/BTN` (transparent labels), `WM_LBUTTONDOWN/UP/DBLCLK`, `WM_RBUTTONDOWN/UP`, `WM_MOUSEMOVE` (container views → `NSEvent` mouse methods), `WM_KEYDOWN/UP`, `WM_CHAR` (UTF-16 → UTF-8, surrogate pairs).
* `pump()` runs `PeekMessageW`/`DispatchMessageW` for at most 512 messages per iteration and gives Tab to `IsDialogMessageW`; `wait()` is `MsgWaitForMultipleObjectsEx` on the run loop's wake event and the input queue.
* DPI: per-monitor v2 (manifest + `SetProcessDpiAwarenessContext` fallback). Child rectangles arrive in points and are scaled with the window's `GetDpiForWindow`.
* Fonts: Segoe UI 9 pt scaled to the window dpi (`CreateFontIndirectW`), applied to every control; `SystemParametersInfo` is not used (the worker's Job Object forbids it).
* Lifetime: every native object created is tracked; destroying a window forgets its children; at the end of the process `appkit::shutdown` destroys the windows the program left open and reports
  `gui_open_windows_at_exit` and `gui_native_objects_after` (must be 0). A window procedure only calls into the guest for the messages above, always on the thread that pumps messages, never from a different thread.
* `JOB_OBJECT_UILIMIT_*` (the worker's job): desktop switching, display settings, global atoms, clipboard and system parameters are blocked; creating and using windows is not.

## Offscreen backend (`offscreen_backend.cpp`)

The same native-object model in memory, with scripted input replayed by `pump()`: `wait <ms>`, `click <control text>`, `close`, `type <text>`, `mouse <x> <y>`, `key <chars>`,
and (0.6) `snapshot <file.png>`, `resize <width> <height>`, `scale <factor>`. Input goes through the **same `Events`
callbacks** as the Win32 window procedures, so the AppKit code under test is the real one; only the window system is replaced (like Qt's `offscreen` platform plugin). It is selected by
`GuestConfig::gui_backend = "offscreen"` (in-process tests only; the CLI always uses Win32) and never fabricates a result: a click on a disabled or missing control does nothing.
Its fake window chrome (30 points of title bar) is documented in the source.

## Drawing (0.6.2)

The backend owns the drawing surface of every **custom-painted** container view (one whose class overrides `-drawRect:`; the AppKit layer tells it with `set_custom_paint`):

| Backend call / event | Win32 | Offscreen |
|---|---|---|
| `set_custom_paint(view, on)` | the `ORVYNView` window gets a `ID2D1HwndRenderTarget`, `WM_ERASEBKGND` returns 1, `WM_PAINT` is routed to the paint event | a WIC bitmap surface, repaint flag |
| `invalidate(view)` | `InvalidateRect` (async `WM_PAINT`) | marks dirty; the next `pump()` paints before any scripted input |
| `display_now(view)` | `InvalidateRect` + `UpdateWindow` (synchronous) | paints immediately |
| `Events::paint(view, dirty)` | inside `BeginPaint`/`EndPaint`, between `BeginDraw`/`EndDraw` | between `BeginDraw`/`EndDraw` on the bitmap |
| `paint_device()`, `paint_scale()` | the window surface; `GetDpiForWindow / 96` | the bitmap; the emulated scale |
| `save_snapshot(window, png)` | not available | composite of the custom-painted views as a PNG |

`Events::paint` is the only place the AppKit layer draws; `paint_device()` is non-null only inside it. The surface is moved out of the native node while the guest
draws, so a window closed from inside `-drawRect:` cannot free it under the running frame. A lost Direct2D target (`D2DERR_RECREATE_TARGET`) drops the surface and
repaints. See [APPKIT-DRAWING.md](APPKIT-DRAWING.md) and [COREGRAPHICS-RENDERING.md](COREGRAPHICS-RENDERING.md).

## Tests

| Kind | Where | Needs a desktop |
|---|---|---|
| unit (geometry) | `tests/unit/test_geometry.cpp` | no |
| headless integration (compiled programs on the offscreen backend) | `tests/runtime/test_appkit.cpp` | no |
| headless drawing (Mach-O scene on a WIC surface, PNG snapshots compared by pixel) | `tests/runtime/test_cg_rendering.cpp`, `test_cg_abi.cpp`, `tests/unit/test_cg.cpp` | no (Direct2D software target) |
| interactive integration (real `orvyn.exe`, real window, driven with `BM_CLICK` / `WM_CLOSE` / `SetWindowPos`, captured with `PrintWindow` by a separate process) | `tests/integration/test_gui_win32.cpp` (`ctest -L gui`) | **yes**: skipped when no input desktop is available or `ORVYN_NO_GUI_TESTS` is set |
| real windows in the test process (GDI/USER/COM leak counts around 40 windows with Direct2D surfaces) | `test_cg_rendering.cpp` (`real_windows_cycle...`) | **yes** (skipped otherwise) |

## Limits

One primary display modelled; no multi-monitor placement logic besides what Windows does; no menus, no clipboard, no accessibility; custom drawing only through `-drawRect:` of views (no text drawing, no images, whole-view dirty rectangles); keyboard codes are Windows virtual-key codes.
