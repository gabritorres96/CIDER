# AppKit subset (CIDER 0.5, experimental)

A small, host-implemented AppKit: Objective-C classes registered in CIDER's runtime that create **real Windows windows** through a GUI backend ([GUI-BACKEND.md](GUI-BACKEND.md)). It is not Apple's AppKit
(no Apple code, written from public behaviour), it covers only what is listed here, and **CIDER makes no claim of AppKit, Cocoa or macOS application compatibility**. Verified with clang-built Mach-O
fixtures (`tests/fixtures/appkit`), the headless `offscreen` backend, and a controller that drives the real Win32 window with window messages.

Status legend: **T** = exercised by a compiled fixture, **P** = partial (documented), **-** = absent.

## Object model

AppKit objects are ordinary Objective-C objects `[isa][payload id]`; the state lives in host payloads (like the Foundation classes), so **guest subclasses work** (`NSView` subclasses overriding `mouseDown:`,
delegates, `NSWindow` subclasses). Retain rules follow Cocoa: a view retains its subviews, a window its content view, `NSApplication` the windows that were not closed (so an open window and everything in it stay alive,
and its destruction releases them); targets, delegates, first responders and back pointers are **not** retained. The shared instance of `NSApplication`, `NSScreen mainScreen` and the NSApp variable are immortal.

## Classes and methods

**NSResponder** (T): `nextResponder`, `acceptsFirstResponder`, `becomeFirstResponder`, `resignFirstResponder`, `mouseDown:/mouseUp:/mouseDragged:/mouseMoved:/rightMouseDown:/rightMouseUp:/keyDown:/keyUp:` (default
implementations forward to the next responder: view → superview → window → application).

**NSApplication** (T): `+sharedApplication`, `run`, `stop:`, `terminate:`, `finishLaunching`, `setActivationPolicy:`/`activationPolicy` (stored; always a regular window), `activateIgnoringOtherApps:`, `delegate`/`setDelegate:`,
`windows`, `keyWindow`, `mainWindow`, `isRunning`, `isActive`, `sendAction:to:from:` (nil target: responder chain from the key window, then the window delegate, the application, its delegate), `mainMenu`/`setMainMenu:` (stored,
never shown), global `NSApp`. Delegate: `applicationDidFinishLaunching:`, `applicationShouldTerminate:`, `applicationWillTerminate:`, `applicationShouldTerminateAfterLastWindowClosed:`.

**NSWindow** (T): `initWithContentRect:styleMask:backing:defer:` (a default content view is created; the native window is created lazily on the first order-front), `title`/`setTitle:`, `contentView`/`setContentView:`,
`frame`, `setFrame:display:`, `setContentSize:`, `setFrameOrigin:`, `center`, `setMinSize:`/`minSize`, `styleMask`, `level`/`setLevel:` (stored), `delegate`/`setDelegate:`, `makeKeyAndOrderFront:`, `orderFront:`, `orderOut:`,
`close`, `performClose:`, `isVisible`, `isKeyWindow`, `isMainWindow`, `makeKeyWindow`, `makeFirstResponder:`/`firstResponder`, `isReleasedWhenClosed`/`setReleasedWhenClosed:` (stored; windows are never over-released),
`display` (no-op). Delegate: `windowShouldClose:`, `windowWillClose:`, `windowDidResize:`, `windowDidMove:`, `windowDidBecomeKey:`, `windowDidResignKey:`. Style mask bits: titled, closable, miniaturizable, resizable
(borderless = popup). Notifications `NSWindowWillCloseNotification`, `NSWindowDidResizeNotification`... are posted through `NSNotificationCenter` too.

**NSView** (T): `initWithFrame:`, `frame`, `bounds` (origin always (0,0)), `setFrame:`, `setFrameSize:`, `setFrameOrigin:`, `addSubview:`, `removeFromSuperview` (retain/autorelease like Cocoa), `superview`, `window`, `subviews`,
`setNeedsDisplay:`, `needsDisplay`, `display`, `isHidden`/`setHidden:`, `tag`/`setTag:`/`viewWithTag:`, `autoresizingMask`/`setAutoresizingMask:` (the six margin/size bits are honoured when the window or a parent is resized),
`isFlipped` (honoured when positioning children), `isDescendantOf:`, `drawRect:` (never called). A plain or subclassed `NSView` becomes a native child window that receives mouse and keyboard input.

**NSControl** (T): `target`/`setTarget:`, `action`/`setAction:`, `isEnabled`/`setEnabled:`, `stringValue`/`setStringValue:`, `intValue`, `integerValue`, `doubleValue` and their setters, `sendAction:to:`, `performClick:`, `sizeToFit`
(approximate), `setFont:`, `setAlignment:`, `setToolTip:` (accepted, ignored).

**NSButton** (T): `+buttonWithTitle:target:action:`, `title`/`setTitle:`, `state`/`setState:`, `setButtonType:` (switch/checkbox and radio map to a check box; radio grouping is not implemented), `setBezelStyle:`,
`setKeyEquivalent:`, `setImage:` (accepted, ignored). Native control: `BUTTON`.

**NSTextField** (T): `+labelWithString:`, `+wrappingLabelWithString:`, `initWithFrame:` (editable by default), `isEditable`/`setEditable:` (changing it replaces the native control: `EDIT` ↔ `STATIC`), `isSelectable`/`setSelectable:`
(read-only edit), `isBezeled`/`setBezeled:`, `setDrawsBackground:`, `placeholderString`/`setPlaceholderString:` (`EM_SETCUEBANNER`), `delegate` (stored; text-change delegate calls are not delivered), `stringValue` reads the live text
of the native control. Single line only; no `NSTextView`, no formatters, no fonts/colours.

**NSEvent** (T): `type`, `locationInWindow` (window space, y up), `window`, `characters`, `charactersIgnoringModifiers`, `keyCode` (the **Windows virtual-key code**, not Apple's), `modifierFlags`, `clickCount`, `buttonNumber`.
Created only by the backend; no `NSEvent` constructors, no `nextEventMatchingMask:`.

**NSScreen** (T): `+mainScreen`, `frame`, `visibleFrame`, `backingScaleFactor` (primary monitor only).

**NSNotification / NSNotificationCenter** (Foundation, T): `defaultCenter`, `addObserver:selector:name:object:`, `addObserverForName:object:queue:usingBlock:` (runs on the posting thread), `removeObserver:`,
`postNotification:`, `postNotificationName:object:(userInfo:)`; observers and the sender filter are not retained.

## What is **not** implemented (so do not expect it)

Menus (`NSMenu`, `NSMenuItem`, the menu bar), `NSTextView`/text system, `NSImage`, `NSColor`, `NSFont`, any drawing (`drawRect:`, `NSGraphicsContext`, `CGContext`), `NSPanel`/alerts/open-save panels, `NSViewController`/
`NSWindowController`, nibs/storyboards (`NSApplicationMain` is not exported), `NSTableView` and other controls, layout constraints (Auto Layout), animations/Core Animation, tracking areas, drag and drop, first-responder tab loop
(only the native Tab key between controls), accessibility, documents, `NSTimer`, `NSOperationQueue`, multiple screens, full-screen/zoom/miniaturize APIs, window levels/styles beyond the four mask bits, sheets, `releasedWhenClosed`
semantics, key equivalents, input methods.

## How to try it

```
tools\llvm-env.cmd tools\gen-appkit-fixtures.cmd
build\bin\Release\cider.exe run tests\fixtures\appkit\bin\hello_window\hello_window
```
A window "CIDER — Hello from macOS" appears with two labels, a counter ("Clicks: 0"), and the buttons "Click me" and "Close"; see `docs/EXECUTION.md`.
