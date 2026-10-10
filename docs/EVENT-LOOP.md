# The event loop (CIDER 0.5)

One run loop per process, shared by `CFRunLoop`, `NSRunLoop`-style timers, `dispatch_main`, and `NSApplication run`. Code: `src/corefoundation/runloop.cpp` (`include/cider/runloop.hpp`),
`src/corefoundation/cf_bridge.cpp` (sources and timers), `src/appkit/appkit_app.cpp` (the AppKit driver).

## One iteration (`runloop::run`)

1. fire the timers that are due (host timers and `CFRunLoopTimer`s);
2. perform the version-0 sources that were signaled (`CFRunLoopSourceSignal`), ordered by their `order`;
3. run every ready block of the **main dispatch queue** (and `dispatch_after` items that became due);
4. let the GUI backend `pump()` its window messages (`PeekMessage` / `DispatchMessage`), where each button click or mouse/key event becomes a call into the guest;
5. if the loop was stopped, return; if something was handled, go round again; otherwise compute the wait time (next timer, next delayed main-queue block, the deadline, at most 20 ms) and sleep.

The sleep is `MsgWaitForMultipleObjectsEx` on a wake event **and** the thread's input queue (GUI active), or a plain wait on the event, inside `GuestThreads::blocking`, which **releases the cooperative guest
lock**: guest threads (including dispatch workers) run while the main thread waits. `runloop::wake()` (called when a worker adds work to the main queue, a source is signaled or `stop` is called) sets the
event. The 20 ms slices also let an `exit()` requested by another thread unwind the loop.

Return values follow CoreFoundation: `kCFRunLoopRunFinished` (nothing can ever wake it: no timers, sources, main-queue work or GUI), `Stopped`, `TimedOut`, `HandledSource`. Modes are not implemented
(one mode; `kCFRunLoopDefaultMode`/`CommonModes`/`NSDefaultRunLoopMode` are the same string).

## Threads

Only the **main guest thread** runs the loop and owns every window. Other threads submit work through `dispatch_async(dispatch_get_main_queue(), ...)`; a callback of the GUI is never run on a worker.
`CFRunLoopGetCurrent` on a secondary thread is refused (secondary run loops do not exist).

## NSApplication

`-[NSApplication run]`: `finishLaunching` (delegate `applicationDidFinishLaunching:`, notification), then loops `runloop::run(-1)` with the GUI hooks installed (pump, wait, keep-alive while the app is running and not stopped).
* `stop:` sets a flag and stops the innermost loop; `run` returns, the program continues after it.
* `terminate:` asks the delegate (`applicationShouldTerminate:` must answer `NSTerminateNow` = 1), posts `applicationWillTerminate:`/notification, then ends the process with status 0 through the normal `exit()` path
  (every guest thread unwinds, `atexit` handlers run). It is also what happens when the last window closes and the delegate answers YES to `applicationShouldTerminateAfterLastWindowClosed:`.
* An action invoked from a window message (target/action) runs **inside** `run` as a nested guest call; `exit()` called in it propagates to the interrupted `run` call and from there to `main`.

## What a click does (Win32 backend)

```
user clicks "Click me"  ->  BUTTON control sends WM_COMMAND(BN_CLICKED) to its parent window
  -> window procedure (backend) -> Events::control_clicked(native id)
  -> AppKit layer maps the HWND to the NSButton, pushes an autorelease pool
  -> -[NSApplication sendAction:to:from:] semantics: target (or the responder chain when the target is nil)
  -> objc_msgSend-equivalent dispatch of the selector -> -[AppController buttonClicked:] (code compiled into the guest Mach-O)
  -> NSLog, -[NSTextField setStringValue:] -> SetWindowTextW
```
Nothing in this path is simulated: the controller in `tests/integration/test_gui_win32.cpp` sends real `BM_CLICK` / `WM_CLOSE` messages to the real window of a real `cider.exe` process.

## Timeouts

`cider run` kills the worker after `--timeout` (default 60 s). A program that links AppKit is interactive, so its default is **lifted to 24 hours** unless `--timeout` is given. The worker is still
supervised (Job Object: memory and process limits, kill-on-close) and still **not a sandbox**.

## Limits

No `NSEvent` loop APIs (`nextEventMatchingMask:`), no modal sessions, no run-loop observers, no `NSTimer` class (use `CFRunLoopTimer` or `dispatch_after`), no input method / IME handling beyond WM_CHAR,
no keyboard shortcuts / menus, no drag and drop.
