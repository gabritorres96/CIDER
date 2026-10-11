# Top 20 compatibility improvements

Ranked by **evidence** (how many measured real binaries each item unblocks, and how early in the link/run it stops them), then by cost. Evidence sources: [REAL-APP-COMPATIBILITY.md](REAL-APP-COMPATIBILITY.md), [DYLD-COMPATIBILITY.md](DYLD-COMPATIBILITY.md), [OBJC-RUNTIME-COMPATIBILITY.md](OBJC-RUNTIME-COMPATIBILITY.md), [LIBSYSTEM-LIBC-ANALYSIS.md](LIBSYSTEM-LIBC-ANALYSIS.md), [FOUNDATION-COREFOUNDATION.md](FOUNDATION-COREFOUNDATION.md), [APPKIT-COREGRAPHICS.md](APPKIT-COREGRAPHICS.md), [SWIFT-RUNTIME-FEASIBILITY.md](SWIFT-RUNTIME-FEASIBILITY.md).
Effort: S (days), M (1–3 weeks), L (a month or more), XL (a quarter or more) — **estimates, not measurements**. Every item needs a clang-built fixture that fails before and passes after, and an update of the exact-registry test. Nothing here is implemented.

| # | Improvement | Unblocks (evidence) | Effort | Risk | Depends on |
|---|---|---|---|---|---|
| 1 | **Small (relative) method lists** in the ObjC metadata reader | every ObjC binary built for macOS 11+ is refused today (`src/objc/objc_metadata.cpp:30`; verified by a run) | S | low | — |
| 2 | **Weak binding and weak-definition coalescing** | `execution_validator.cpp:39` refuses any image with weak bindings: all C++ and most Swift images | S–M | low | — |
| 3 | **Libc basics found first in real link errors**: `__stack_chk_guard/fail`, `___chkstk_darwin`, `__assert_rtn`, `abort`, `memchr`, `modf`, `fma`, `posix_memalign`, `malloc_size`, `pthread_main_np`, `__cxa_atexit`, `os_unfair_lock_*` | first unresolved symbol of nearly every binary | S | low | — |
| 4 | **`Cocoa` umbrella as an alias** re-exporting AppKit/Foundation/CoreData names | 4 of 5 ObjC apps name `Cocoa`; link stops there even if all imports are satisfied | S | low | — |
| 5 | **Stub-library mechanism** for `ApplicationServices`, `Carbon`, `IOKit`, `CoreServices`, `ServiceManagement`, `Security`, `libcompression`: load succeeds, each symbol traps at call time naming itself | first blocker of cliclick, Flycut, Sloth, KeepingYouAwake, CotEditor | M | medium: must never make `compatibility` claim support | design review (honest traps) |
| 6 | **First third-party run: cliclick `-V`/`-h`** (stages 0–2 of the plan) | proves the pipeline on an unmodified Apple-built binary | M | medium | 1, 3, 4, 5 |
| 7 | **`dlopen`/`dlsym`/`dladdr` + `_dyld_*` image callbacks** over a "load more images" API | Swift runtime, `NSBundle`, plug-ins, Foundation internals | M | medium | linker refactor |
| 8 | **C++ runtime**: `libc++.1`/`libc++abi` subset (`operator new/delete`, `__cxa_*`, guards, `std::` string/vector exports used by binaries) | KeepingYouAwake (Sparkle), Stats, CotEditor, Swift runtime | L | high (large symbol surface) | 2, 3 |
| 9 | **`objc_sync_enter/exit` and associated objects** | `@synchronized` (very common), category properties, KVO, many framework internals | S | low | — |
| 10 | **Hash-table `NSDictionary`/`NSSet`** with CF semantics (`CFBasicHash` adapted or reimplemented) | the linear-scan dictionary is unusable for real data sizes; `NSSet` missing | M | low | licence note (Apache + exception) |
| 11 | **`NSBundle`, `NSUserDefaults`, `NSProcessInfo`, `NSTimer`/`NSRunLoop`, `NSValue`, `NSNull`, `NSIndexSet`, `NSCharacterSet`** | needed by every GUI app measured | M–L | low | 10 |
| 12 | **Images for CoreGraphics/AppKit**: `CGBitmapContextCreate*`, `CGImageSource*`, `NSImage`, `NSBitmapImageRep`, `NSImageView` | status items, icons, any custom UI (Flycut, KeepingYouAwake) | M | low | WIC code exists |
| 13 | **`NSMenu`/`NSMenuItem`/`NSStatusBar`** on Win32 menus and notification area | both candidate GUI apps are status-bar apps | M | medium (global menu bar vs per-window) | 11, 12 |
| 14 | **Remaining ObjC runtime surface**: `objc_msgSendSuper2_stret`, `_fp2ret`, `class_copy*List`, `objc_getClassList`, `method_*` introspection, `imp_implementationWithBlock` | reflection, KVC, serialization; `_objc_msgSendSuper2_stret` is in CotEditor's imports | S–M | low | — |
| 15 | **Lazy binding to real stubs** with `dyld_stub_binder` semantics | a missing symbol in an unused stub currently fails the whole load | M | medium | — |
| 16 | **Terminators and `__cxa_finalize`**; initializer/terminator ordering | C++ static destruction, `dlclose` | S–M | low | 3, 7 |
| 17 | **`dispatch_source` timers, suspend/resume/cancel, target queues** | `NSTimer`-less timers, background activity schedulers (Stats) | M | low | — |
| 18 | **Fonts and text**: font-name mapping over DirectWrite, `NSFont`, `NSColor`, `NSBezierPath`, `NSAttributedString`, then a TextKit-shaped `NSTextView` | text-heavy apps; CotEditor class list | L–XL | medium | 12 |
| 19 | **`@try/@catch` via an unwinder** over `__unwind_info`/`__eh_frame` | exceptions in ObjC, C++, Swift-through-C++ frames | XL | high | 2, 8 |
| 20 | **Swift runtime proof of concept** (adapt `swiftlang/swift` runtime to serve a Mach-O guest; start with a 5.0 `libswiftCore.dylib`, 261 imports) | all four Swift apps measured | XL | high | 2, 3, 7, 8, 19 |

## Reading the ranking

* Items 1–5 are small, independent, and remove the *first* error of every measured binary. They are what makes the next failure visible.
* Item 6 is the first honest milestone: a real, unmodified third-party binary prints the output it prints on a Mac. **Do not declare success before stage 1 is run and its output compared** (no Mac was available: PENDING VALIDATION of the expected string).
* Items 12–13 plus 11 unlock the first GUI application. Items 19–20 are not prerequisites for an Objective-C-only first application.
* Out of scope for this list: SwiftUI (1,267 symbols in CotEditor), Metal, Auto Layout, nibs/keyed archives (needed for most nib-based apps; add after 11–13), IOKit/CGEvent behavior on Windows (those calls must fail honestly).

## Measurement rule

Progress is reported as **named, tested behaviors and binaries that reach a defined stage**, never as a percentage of imported symbols (`tools/app_coverage.py` counts names only and under-counts dynamic providers).
