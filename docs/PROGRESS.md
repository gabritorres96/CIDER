# Progress

**Current phase: CIDER 0.5.0 — AppKit subset, native Windows GUI, Blocks, libdispatch, Apple SDK research. Implemented and verified for controlled fixtures only.
No macOS application runs; Cocoa does not exist; Swift and SwiftUI are not supported; AppKit and Foundation are host-written subsets.**
Last verified: 2026-10-11, Windows 11 Pro 10.0.26200, MSVC 14.51 + ml64, clang-cl/LLVM 23.1.3 (portable), CMake 4.2.3.

## What now works (0.5, on top of 0.4)

| Capability | Evidence |
|---|---|
| A clang-built Objective-C Mach-O creates a **real Windows window** (`hello_window`): title "CIDER — Hello from macOS", labels, "Click me" / "Close" buttons; clicks run guest methods, the counter lives in the program, Close ends the process with status 0 | `test_gui_win32` (3, interactive: a separate process finds the window and sends `BM_CLICK` / `WM_CLOSE`), `test_appkit` (3, headless) — also verified visually and by hand on the author's desktop |
| Blocks (copy/release, `__block`, captured objects, runtime-invoked blocks) | `test_blocks` (3) |
| libdispatch subset (main/serial/global queues, groups, semaphores, once, after, worker→main sync) | `test_dispatch` (3) |
| Foundation: NSMutableString, NSData, NSDate, NSURL, NSError, NSFileManager (root-confined), NSNotificationCenter | `test_foundation_extra` (3), `test_appkit` |
| CoreFoundation basics, CoreGraphics geometry with System V struct passing, shared run loop (timers, sources, main queue) | `test_corefoundation` (1), `test_geometry` (4) |
| Message forwarding (resolve / forwarding target / NSInvocation), uncaught NSException, `@try` programs refused | `test_objc_dynamic` (4) |
| Apple SDK `.tbd` reader and `cider sdk-scan` (3,702 stubs parsed, 0 failures) | `test_sdk` (4), [APPLE-SDK-RESEARCH.md](APPLE-SDK-RESEARCH.md) |
| CotEditor, Rectangle, Stats, MonitorControl analysed statically: all are Swift apps, refused | [COTEDITOR-COMPATIBILITY.md](COTEDITOR-COMPATIBILITY.md), [COMPATIBILITY.md](COMPATIBILITY.md) |

## Verification evidence (real runs, 2026-10-11)

| Check | Result |
| --- | --- |
| Debug build + `ctest` | 36/36 suites pass |
| Release build + `ctest` | 36/36 suites pass |
| Test count (Release, per executable) | **490 run, 487 passed, 0 failed, 3 skipped** (0.4: 462 run) |
| Fresh build from scratch (new directory, Release) | configure + build rc 0; ctest 36/36 pass |
| AddressSanitizer (clang-cl, all 36 suites incl. AppKit, GUI window tests, blocks, dispatch) | 0 errors, 36/36 pass |
| libFuzzer, 120 s each | `fuzz_objc` 9,251 runs; `fuzz_macho` 1,105,698; `fuzz_loader` 707,530; `fuzz_linker` 303,260; `fuzz_chained` 795,571; **`fuzz_tbd` (new)** 75,968 — no crash, leak, timeout or OOM |
| Interactive GUI tests | passed on this machine (real window, `BM_CLICK`, `WM_CLOSE`); skipped automatically without an input desktop |
| GitHub Actions | **NOT validated**: never ran on GitHub |
| Real macOS apps executed | **none**; four analysed statically (all Swift) |

## Commits (local, no remote)

`97b993d` ignore `utilities/` · `f8d05ac` SDK analyzer · `efa710b` Blocks · `52a8f3e` libdispatch · `64d3f99` Foundation extension · `afb3204` CoreFoundation/CoreGraphics/run loop ·
`f05a9fe` AppKit + Win32/offscreen backends + GUI tests · `b739541` forwarding + NSException · plus the documentation/version commit that follows.

## Problems found while building 0.5 (all fixed, covered by tests)
1. `_Block_copy` of a `__block` variable needs two references (the copying block and the frame that still disposes its own): with one, the variable was freed under the running block.
2. Block names: the Mach-O symbols are `__Block_copy` (C name `_Block_copy`); the stubs and registry had the wrong spelling.
3. A double `Block_release` was accepted when freed heap memory kept a "global" flag bit; the check now uses the image layout instead of block-owned bits.
4. A dispatch worker retained its queue forever; workers now retire when only they reference the queue; dispatch objects are released after threads stop so leak counters are meaningful.
5. `NSNotificationCenter` retained the sender filter and kept window trees alive.
6. A new `NSWindow` had no content view; the default one is created at init.
7. `std::filesystem` iteration of the SDK stopped silently at paths longer than `MAX_PATH` (extended-length paths now).
8. ASan: the allocator quarantine pushed `repeated_runs_do_not_leak_memory_or_address_space` over its release limit; the limit is wider under ASan only.

## Known gaps
CI never ran on GitHub; one machine; x86_64 only; threads are cooperative; no Swift, SwiftUI, `@try/@catch`; AppKit has 9 classes and no menus, drawing, images, text system or Auto Layout; only the primary display is
modelled; libdispatch runs concurrent queues serially; the worker is not a sandbox (GUI programs get a 24 h default timeout); fuzzing sessions are minutes long. See [LIMITATIONS.md](LIMITATIONS.md).

## Next phase (recommended, not started)
0.6: CoreGraphics drawing (`CGContext`, `drawRect:`), NSMenu/NSMenuItem and the menu bar, NSImage/NSColor/NSFont, NSTextView basics, NSTimer, NSUserDefaults/NSBundle, key-value coding and property introspection; in parallel a
study of what it would take to host the open-source Swift runtime (the wall every real application hits).
