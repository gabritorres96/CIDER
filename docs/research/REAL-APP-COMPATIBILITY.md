# Real application compatibility (re-measured)

**No real macOS application runs on ORVYN.** This document records what was measured, how, and what the first honest target should be.
Labels: **VERIFIED**, **INFERRED**, **HYPOTHESIS**, **PENDING VALIDATION**. Measurements: 2026-10-11, `orvyn.exe` built from `1b77aa7`+docs, Windows 11, `llvm-objdump` 23.1.3, tool `tools/app_coverage.py`.

## 1. What the numbers mean — and do not

The five levels the task asks to keep apart, and how each is established here:

| Level | Meaning | How measured |
|---|---|---|
| **Recognised symbol** | the name appears in ORVYN's registry (host function, data symbol or class) | `orvyn sdk-scan @implemented` (458 functions + 49 classes) |
| **Linked symbol** | the loader binds it without error | `orvyn run` — *no application reached this stage*; only controlled fixtures are linked |
| **Implemented API** | a host function/class exists | same as "recognised": it says nothing about behavior |
| **Tested API** | a clang-built fixture calls it and the result is asserted | `tests/runtime/*`, `tests/fixtures/*` |
| **Compatible behavior** / **functional application** | matches Apple's documented behavior / the app does its job | **none established** |

`tools/app_coverage.py` reports *recognised* only; its "implemented N of M" is a **name count**, never a compatibility percentage, and it **under-counts**: data symbols served by dynamic providers (`_NSApp`, `___CFConstantStringClassReference`, `__NSConcreteStackBlock`, `__dispatch_main_q`, `_kCFTypeArrayCallBacks`, `_kCFRunLoopDefaultMode`, `_NSZeroRect`...) are not in the registry list. Treat the figures below as *lower bounds on names*, not as progress bars.

## 2. The four Swift applications (earlier phase; one re-run)

| Application | Language | Slices | Imports | Swift libs | libswiftCore imports | First blocker today |
|---|---|---|---|---|---|---|
| CotEditor 7.0.9 | Swift (AppKit/SwiftUI/TextKit 2) | x86_64 (+arm64) | 4,371 (105 recognised) | 26 | 1,316 | **re-run**: `Missing library: /usr/lib/libcompression.dylib, IOKit, Security, ServiceManagement, SystemConfiguration, WebKit, AVFoundation, Accessibility, AppIntents, AudioToolbox, Combine, CoreText, CoreTransferable, DeveloperToolsSupport, ImageIO, OSAKit, QuartzCore, QuickLookUI, StoreKit, SwiftUI, Symbols, VisionKit, libc++.1.dylib, libswiftCore.dylib ...` (link aborted before mapping; Sparkle is parsed first) |
| Rectangle | Swift (+ SwiftUI, Combine, Carbon/AX) | x86_64 (+arm64) | 1,208 (94) | 19 | 342 (+ SwiftUI 344) | not re-run (bundle no longer on disk); report in `utilities/reports/coverage_Rectangle.json`: Swift libs, `ApplicationServices` (`AXIsProcessTrusted`, `AXObserver*`), `Carbon` (global hot keys), `CGEvent*` |
| Stats | Swift (+ `Kit` framework) | x86_64 (+arm64) | 623 (25) | 20 | 386 | not re-run; also imports `libc++` (`operator new/delete`), `____chkstk_darwin`, `UserNotifications`, `QuartzCore`, `NSBackgroundActivityScheduler` |
| MonitorControl | Swift (+ Sparkle, OSD, DDC via IOKit) | x86_64 (+arm64) | 1,434 (26) | 22 | 1,086 | not re-run; `CGDisplay*`, `QuartzCore`, `Sparkle`, `OSD` |

(In parentheses: names recognised by ORVYN, **not** compatibility.) Reason common to all four, VERIFIED: they link `/usr/lib/swift/libswiftCore.dylib`; see [SWIFT-RUNTIME-FEASIBILITY.md](SWIFT-RUNTIME-FEASIBILITY.md). The three non-re-run entries were last observed in the 0.5 phase; their bundles are not on this machine today.

## 3. Objective-C-only applications measured now (VERIFIED)

Downloaded from the projects' own GitHub release pages into a scratch directory outside the repository for **static analysis only** (nothing was installed; the bundles were read with `orvyn inspect/dependencies`, `llvm-objdump` and refused by `orvyn run` at link time). Sizes: `cliclick.zip` 54 KB, `terminal-notifier-3.1.0.zip` 390 KB, `Flycut.1.9.6.zip` 1.2 MB, `sloth-3.6.zip` 1.3 MB, `KeepingYouAwake-1.6.8.zip` 4.4 MB.

| Application | License | Binary | Imports | AppKit classes | Frameworks needed (beyond libSystem/libobjc/Foundation/CF/AppKit/CoreGraphics) | First blocker (run) |
|---|---|---|---|---|---|---|
| **cliclick 5.1** (CLI mouse/keyboard emulation) | BSD-3-Clause (`LICENSE` read) | FAT x86_64 + arm64, macOS ≥ 10.15 | **38** | `NSBitmapImageRep NSEvent NSPasteboard NSWorkspace` | ApplicationServices, Carbon, IOKit, Cocoa, CoreServices | `Missing library: ApplicationServices` (then Carbon, IOKit, Cocoa, CoreServices) |
| terminal-notifier 3.1.0 | NOASSERTION on GitHub (license file not found at the usual path: **unverified**) | FAT, ≥ 10.14 | 57 | `NSApplication NSRunningApplication NSWorkspace` | Cocoa, UserNotifications | `Missing library: Cocoa` |
| Flycut 1.9.6 | MIT (GitHub) | FAT, ≥ 10.10 | 124 | 28 (`NSBezierPath NSBitmapImageRep NSGradient NSToolbar NSStatusBar NSPanel NSSlider...`) | ApplicationServices, CloudKit, Cocoa, ServiceManagement, Carbon, CoreServices | `Missing library: ApplicationServices` |
| Sloth 3.6 | BSD-3-Clause | FAT, ≥ 11.5 | 84 | 14 (`NSOpenPanel NSMenu NSPasteboard NSPopUpButton NSWorkspace...`) | Cocoa, libcompression, CoreServices, IOKit, ImageIO, Security, ServiceManagement, SystemConfiguration, WebKit | `Missing library: Cocoa` (+ many); runs `lsof` through `NSTask` |
| KeepingYouAwake 1.6.8 | MIT | FAT, ≥ 10.13 | 89 (+ Sparkle) | 19 (`NSStatusBar NSMenu NSNib NSTabViewController NSTableCellView NSViewController...`) | libc++, libcompression, CoreServices, IOKit, ImageIO, Security, ServiceManagement, SystemConfiguration, WebKit (Sparkle's dependencies) | `Missing library: libc++.1.dylib` (+ the same set) |

Two structural observations (VERIFIED by the runs above):

1. **`Cocoa.framework` is an umbrella** (`LC_LOAD_DYLIB .../Cocoa`) that re-exports AppKit, Foundation and CoreData. Four of the five applications name it. ORVYN provides AppKit/Foundation but *not the umbrella name*: the link stops there even for programs whose imports are all satisfied. Providing `Cocoa` as an alias that re-exports ORVYN's frameworks is a small loader/linker task with large effect (see the Top-20 list).
2. **Non-Swift applications are an order of magnitude smaller than Swift ones**: 38-124 imported symbols against 623-4,371. The "first blocker" in every case is a **library that does not exist for ORVYN**, not a missing function: ApplicationServices, Carbon, IOKit, CoreServices, ServiceManagement, Security, WebKit, `libc++`, `libcompression`.

## 4. Candidate for the first third-party test: **cliclick** (recommendation)

Selection criteria from the task: open source, macOS x86_64, preferably Objective-C, small dependencies, simple interface, legally available original binary.

| Criterion | cliclick |
|---|---|
| Open source | yes, BSD-3-Clause (https://github.com/BlueM/cliclick, verified `LICENSE`) |
| Binary | release asset `cliclick.zip` (54 KB), universal x86_64 + arm64; **unmodified** original |
| Language | Objective-C (GitHub language), 0 Swift libraries |
| Dependencies | 38 imported symbols; 10 Foundation + 8 CF + 7 libobjc + 6 libSystem + 5 AppKit + 2 Carbon |
| Interface | command line: no window and no nib; whether `-V`/`-h` avoid starting a run loop or touching `NSApplication` was **not verified** (HYPOTHESIS: they print and exit); the *same* binary also drives input devices through `CGEvent`/`IOKit` (not reproducible on Windows: those commands must **fail honestly**) |
| Why not a GUI app first | every GUI candidate measured needs a nib (`NSNib`), `NSStatusBar`/`NSMenu`, `NSImage`/`NSColor`, `NSBundle`, `NSUserDefaults`, `NSWorkspace`, or `NSTask` — all absent. A GUI app is the *second* target |

Staged plan (each stage has a verifiable outcome; **none is claimed done**):

| Stage | Task | Acceptance |
|---|---|---|
| 0 | provide the umbrella `Cocoa` and **stub libraries** for `ApplicationServices`, `Carbon`, `IOKit`, `CoreServices` that load (their symbols bind to call-time traps that name the symbol and end the guest) | `orvyn run cliclick -V` reaches `main` |
| 1 | `__stack_chk_guard`, `_optarg`/`_optind` (getopt), `NSBitmapImageRep`/`NSPasteboard`/`NSWorkspace` classes present for *linking* (behavior not required), `NSLog`/`printf` | `cliclick -V` prints the version string of the real binary and exits 0 (the expected string is the one the binary itself contains; no Mac was available to compare against: PENDING VALIDATION) |
| 2 | `-h` (help text via `NSString`/`fprintf`), unknown command error path, exit codes | outputs identical to documented behavior |
| 3 | commands that need the OS (`m:`, `c:`, `kp:`...) | **fail with a clear message** (`CGEventCreateMouseEvent` unavailable); never silently do nothing |

**Do not declare success before stage 1 is run and its output compared.** The stub-library mechanism in stage 0 is a *design decision* (honest traps, no fake behavior) to be reviewed before implementation; it must not make `compatibility` reports claim support.

Second target (GUI), after 0.6 completes menus, images, text, `NSBundle`, `NSUserDefaults`, nibs (`NSNib`/keyed archives) or a no-nib app: **Flycut** (MIT; status item + preferences + custom drawing — exercises exactly the 0.6 work) or KeepingYouAwake (MIT; status item + preferences) — both need `NSStatusBar`, `NSMenu`, `NSImage`, `NSUserDefaults`; and the Carbon/IOKit stubs above.

## 5. How to reproduce

```
orvyn.exe sdk-scan @implemented > implemented.tsv
python -I tools/app_coverage.py <App.app|binary> --orvyn orvyn.exe --objdump llvm-objdump.exe --json out.json
orvyn.exe inspect <App.app>; orvyn.exe dependencies <App.app>; orvyn.exe run <App.app> --timeout 10   # refused before any guest code runs
```

`orvyn run` of an unmodified Apple-built binary is safe *today* only because the link fails before mapping; once a binary links, **the worker is not a sandbox** (guest code runs with the user's rights) — a real third-party application must be run only after the security model in [LIMITATIONS.md](../LIMITATIONS.md) has been reviewed for that stage.
