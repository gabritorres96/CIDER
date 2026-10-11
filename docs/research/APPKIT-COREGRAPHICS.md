# AppKit and the graphics stack

Question: how should ORVYN's AppKit/CoreGraphics subsystem grow, which Windows APIs map well, and what do alternative implementations teach us?
Labels: **VERIFIED**, **INFERRED**, **HYPOTHESIS**, **PENDING VALIDATION**.

## 1. What exists today (VERIFIED, repository at `1b77aa7`)

| Component | State | Evidence |
|---|---|---|
| `CGContext` (guest object, state stack, CTM, clips, current path) | **done for the subset**: ~100 `CG*` functions | `src/graphics/cg_api.cpp`, `include/orvyn/cg.hpp`, `cg_context.cpp`; `test_cg`, `test_cg_abi` |
| `CGColor`, `CGColorSpace` (DeviceRGB only), `CGPath`/`CGMutablePath`, `CGImage` (handles + `CGContextDrawImage`; **no guest-side creation path**) | done / partial | `cg_api.cpp` |
| `CGAffineTransform` functions and System V struct passing (`CGRect`, `CGAffineTransform` on the stack; hidden result pointers) | done | `cg_abi_test` (clang-built Mach-O), `sysv_args.hpp` |
| `NSView -drawRect:` via `WM_PAINT` → `Events::paint` → `abi::call_guest_nested` | **done** (dirty rect = whole view) | `src/appkit/appkit_draw.cpp`, `win32_backend.cpp`, `test_gui_win32` captures real pixels with `PrintWindow` |
| `NSGraphicsContext` (`currentContext`, `CGContext`, `graphicsContextWithCGContext:flipped:`, save/restore) | done | `appkit_draw.cpp` |
| Direct2D device (HWND render target; WIC software bitmap target for headless tests), DirectWrite measure/draw primitives (not yet exposed to guests), WIC decode (PNG/JPEG/BMP/GIF/TIFF/ICO with limits) and PNG encode | done / partly unused | `src/graphics/d2d_device.cpp`, `fuzz_cg` |
| AppKit classes | `NSResponder NSApplication NSWindow NSView NSControl NSButton NSTextField NSEvent NSScreen NSGraphicsContext` (10) | `src/appkit/`, [APPKIT.md](../APPKIT.md) |
| Not present | menus, `NSImage`/reps, `NSColor`, `NSBezierPath`, `NSFont`, text system, scroll views, panels, pasteboard, `NSWorkspace`, layout, nibs, `NSDocument`, tracking areas, animations | |

Do not re-implement any of the first six rows; the research below builds on them.

## 2. What real applications need first (VERIFIED, CotEditor 7.0.9, imports from AppKit)

CotEditor imports **103 distinct AppKit classes** and 95 other AppKit symbols. ORVYN provides 8 of those 103 (`NSButton NSControl NSEvent NSGraphicsContext NSResponder NSTextField NSView NSWindow`). The list is the shape of a document-based text editor (one data point, not a census):

* **Application/document model:** `NSDocument`, `NSDocumentController`, `NSWindowController`, `NSViewController`, `NSStoryboard`, `NSUserDefaultsController`, `NSWorkspace(+OpenConfiguration)`.
* **Text system:** `NSTextView`, `NSTextStorage`, `NSLayoutManager`, `NSTextContainer`, TextKit 2 (`NSTextContentStorage`, `NSTextLayoutManager`, `NSTextLayoutFragment`, `NSTextLineFragment`, `NSTextRange`), `NSTextAttachment`, `NSParagraphStyle`, `NSMutableParagraphStyle`, `NSTextTab`, `NSTextInputContext`, `NSFont`, `NSFontDescriptor`, `NSFontManager`, `NSFontPanel`.
* **Drawing helpers:** `NSColor`, `NSBezierPath`, `NSGradient`, `NSImage`, `NSImageRep`, `NSBitmapImageRep`, `NSImageView`, `NSColorSpace`, `NSAppearance`, `NSVisualEffectView`.
* **Chrome:** `NSMenu`, `NSMenuItem`, `NSToolbar(+Items)`, `NSTouchBar` family, `NSTabView`, `NSSplitView(+Controller)`, `NSStackView`, `NSScrollView`, `NSScroller`, `NSClipView`, `NSTableView/NSOutlineView` + cells, `NSPopover`, `NSPopUpButton`, `NSSegmentedControl`, `NSSearchField`, `NSStepper`, `NSProgressIndicator`, `NSBox`.
* **Panels and services:** `NSOpenPanel`, `NSAlert`, `NSColorPanel`, `NSPrintOperation`, `NSPasteboard(+Item)`, `NSSharingServicePicker`, `NSSound`, `NSHelpManager`, `NSHapticFeedbackManager`, `NSWritingToolsCoordinator`.
* **Non-AppKit graphics frameworks it also imports:** CoreText (`CTFontCreateWithGraphicsFont`, `CTFontGetGlyphsForCharacters`, `CTFontGetAdvancesForGlyphs`...), ImageIO (`CGImageSourceCreateWithURL`, `CGImageSourceCopyPropertiesAtIndex`), QuartzCore (`CATransform3D*`).

What it implies for planning: **Swift or not, a text editor needs the text system, menus, `NSDocument`, scroll views and `NSColor/NSFont/NSImage` before it can show a window.** A smaller application (Flycut-like utilities) needs far fewer: see [REAL-APP-COMPATIBILITY.md](REAL-APP-COMPATIBILITY.md).

## 3. Windows backend options

| API | Role | Fit | Already used? |
|---|---|---|---|
| **Win32** (HWND, `WM_*`, menus `HMENU`, common controls) | windows, menus (`CreateMenu`/`TrackPopupMenu`), standard controls, file dialogs (`IFileOpenDialog`), clipboard (`OpenClipboard`/`SetClipboardData`), timers (`SetTimer`/waitable timers), message loop | solid for chrome; menus map one-to-one to `NSMenu` (the menu bar vs Windows' per-window menu is a *behavioral* difference: macOS has one global bar) | yes (`win32_backend.cpp`) |
| **Direct2D** | 2D vector drawing, geometry, bitmaps, clipping layers, DPI-aware targets | the right backend for `CGContext` (done) | yes |
| **DirectWrite** | glyph layout, font fallback, metrics, hit testing (`IDWriteTextLayout`) | the natural base for `CTFramesetter`-style measure/draw and for a TextKit-like layout engine; font names differ between platforms (`Helvetica Neue`, `SF Pro` are not on Windows: a font-mapping table is needed; system fonts cannot be redistributed) | draw/measure in the device, not exposed |
| **WIC** (Windows Imaging Component) | decoders/encoders and pixel format conversion | `ImageIO`/`NSBitmapImageRep` decode, PNG/TIFF/JPEG encode | yes (`decode_image`, `encode_png`) |
| **DirectComposition** | GPU composition of visuals, effects, animations | candidate for `CALayer`/`NSAnimationContext`/`NSVisualEffectView` blur; large new surface; not needed for the first applications | no |
| **UI Automation** | accessibility tree | `NSAccessibility` protocols map to UIA providers; real apps import `NSAccessibility*` symbols | no |
| **RichEdit / EDIT** | native text controls | ORVYN uses `EDIT` for `NSTextField`. A RichEdit-based `NSTextView` would be quick but would not match the TextKit API (storage/layout/container separation, attributed ranges, `NSTextInputClient`) | `EDIT` only |
| **Windows Common Controls** (`SysListView32`, `SysTreeView32`, `SysTabControl32`, toolbar) | table/outline/tab/toolbar | possible for `NSTableView`/`NSTabView`; semantics (data-source callbacks, cell views) differ; own-drawn views are more faithful | no |

Mapping decisions that follow from the evidence (INFERRED): own-draw (Direct2D) for views whose appearance AppKit defines (custom views, scroll bars, tables), native Win32 controls where the user expects Windows behavior (buttons, edit fields, dialogs), DirectWrite for all text, WIC for images.

## 4. Alternatives studied

**GNUstep GUI (`libs-gui`)**: an AppKit-like library in Objective-C with `NSView`, `NSTextView`, `NSLayoutManager`, menus, panels, `NSDocument`, nib/gorm loading. **License: LGPL v2 or later** in the source headers (`Source/NSView.m`) with `COPYING.LIB`; GitHub shows GPL-3.0 only because `COPYING` is present. Limitations (VERIFIED/INFERRED): it targets the **GNUstep Base** API and (normally) the GNU runtime (`libobjc2`, a different ABI, see [OBJC-RUNTIME-COMPATIBILITY.md](OBJC-RUNTIME-COMPATIBILITY.md)); its Windows support goes through `gnustep-back` (Cairo/WinUI backends), not Direct2D; it implements the OpenStep/Cocoa API **partially** and with its own look; it is not binary compatible with Apple's AppKit, so it cannot be dropped in for compiled Mach-O guests that were built against Apple's.
**HYPOTHESIS (not tested, not recommended now):** compile selected GNUstep GUI sources as Objective-C against ORVYN's Foundation for x86_64-apple-macos and load them as guest code. That would need GNUstep Base functionality (huge), conflict with ORVYN's own Foundation, and LGPL relinking obligations. Use GNUstep as a **behavioral reference** (e.g. how `NSTextView` splits storage/layout/container, `NSScrollView` and `NSClipView` geometry, menu validation) — reading LGPL code for understanding is unproblematic; copying it is a licence decision (see [THIRD-PARTY-LICENSE-REVIEW.md](THIRD-PARTY-LICENSE-REVIEW.md)).

**Apple's AppKit:** not open source. No public repository exists in `distribution-macOS` (VERIFIED). The behavior must be reimplemented from documentation and tests.

## 5. Knowledge from this research that improves the current graphics subsystem

1. **Guest image creation is the next CoreGraphics gap:** real code builds images with `CGBitmapContextCreate`/`CGBitmapContextCreateImage`, `CGImageSourceCreateWithURL`/`CreateImageAtIndex` (ImageIO), `NSImage initWithContentsOfFile:`. The decode/encode machinery and the `CGImage` object exist; what is missing is the guest API. Order: `CGBitmapContextCreate*` (draw into a WIC bitmap target) → `CGImageSource*` (WIC decode with the existing limits) → `NSImage`/`NSBitmapImageRep` → `NSImageView`.
2. **`dirtyRect` is the whole view today.** Apps that depend on partial redraw for performance (text editors) need real dirty rectangles: Direct2D HWND targets present whole frames, so a backing bitmap per view (draw into it, blit on `WM_PAINT`) enables partial invalidation and `cacheDisplayInRect:`; cost: memory per view.
3. **`CGContextGetCTM` is relative to the view** (documented difference). Real code (e.g. text drawing code that reads `a`/`d` to detect Retina) needs the full device matrix; fix by exposing the base transform in `GetCTM` (low risk, needs a test with scale 2).
4. **Fonts:** CoreText imports (`CTFont*`) and `NSFont` need a font-name mapping and a metrics layer over DirectWrite (`IDWriteFontFace`, `GetDesignGlyphMetrics`). Decision needed: which Windows font stands in for `SF`/`Helvetica` — and that Apple fonts are not redistributable.
5. **Color:** `NSColor`/`CGColor` with named system colors and appearance (`NSAppearanceNameDarkAqua` is imported): a light/dark appearance switch can follow Windows' `AppsUseLightTheme` setting (HYPOTHESIS).
6. **Text system boundary:** decide early whether `NSTextView` is (a) a thin wrapper over RichEdit (fast, wrong API shape) or (b) a TextKit-shaped engine over DirectWrite (right shape, large). Evidence from CotEditor (TextKit 1 *and* 2 classes) favours (b); recommendation: build (b) incrementally behind the `NSTextStorage`/`NSLayoutManager`/`NSTextContainer` API, starting with plain text editing, and keep RichEdit out.
7. **Threading:** every Direct2D/DirectWrite object is single-threaded here; background drawing (`NSOperation`, `CALayer` async) will need a plan before it appears.
8. **Accessibility and DirectComposition** can wait; neither blocks a first third-party application.
