# Executive summary

**State (2026-10-11, version 0.5.0):** ORVYN statically analyses Mach-O/FAT/`.app`/`.dmg` and runs **controlled, clang-built x86_64 Mach-O fixtures** on Windows with its own loader, Apple-ABI Objective-C runtime, Foundation/CoreFoundation/CoreGraphics subsets, libdispatch subset and a Win32/Direct2D AppKit subset. 39 suites, 509 tests: 506 pass, 3 skipped (opt-in). **No Apple-built application has ever run.** Details: [ORVYN-CURRENT-STATE.md](ORVYN-CURRENT-STATE.md).

## Findings that change priorities

1. **Every Objective-C binary built for macOS 11+ is refused** because relative method lists are not supported (`objc_metadata.cpp:30`). Small fix, large effect.
2. **Weak binding is refused** (`execution_validator.cpp:39`), blocking all C++ and most Swift images.
3. The first blocker of real apps is rarely a missing function; it is a **library that does not exist for ORVYN** (`Cocoa`, `ApplicationServices`, `Carbon`, `IOKit`, `CoreServices`, `libc++`). A `Cocoa` alias plus honest stub libraries is cheap.
4. **Swift is viable but a major investment**: all four analysed Swift apps need `libswiftCore` (Apple's own 5.0 build needs 261 imports) plus Swift overlays inside closed frameworks. Objective-C-only apps come first.
5. Open sources help as **references**, not as drop-in code: libobjc2's class layout is not Apple's, real libdispatch needs parallel guest execution, GNUstep is LGPL and targets another runtime. The best reuse candidates are Apache-licensed corelibs CoreFoundation pieces (e.g. `CFBasicHash`).
6. **Licensing**: Apple's dyld/objc4/Libc/xnu are APSL 2.0; nothing has been copied. Default is reimplementation. See [THIRD-PARTY-LICENSE-REVIEW.md](THIRD-PARTY-LICENSE-REVIEW.md) (not legal advice).

## Recommendation

Do the small blockers first (Top-20 items 1–5, 9, 14), then make **cliclick 5.1** (BSD-3, 38 imports) the first unmodified third-party binary, and a status-bar ObjC app (Flycut/KeepingYouAwake, MIT) the first GUI target. Defer exceptions and Swift to a proof-of-concept after 1.0 planning.

## Documents

[Inventory](APPLE-OPEN-SOURCE-INVENTORY.md) · [dyld](DYLD-COMPATIBILITY.md) · [libSystem](LIBSYSTEM-LIBC-ANALYSIS.md) · [libdispatch](LIBDISPATCH-RESEARCH.md) · [Objective-C](OBJC-RUNTIME-COMPATIBILITY.md) · [Foundation/CF](FOUNDATION-COREFOUNDATION.md) · [AppKit/CG](APPKIT-COREGRAPHICS.md) · [Swift](SWIFT-RUNTIME-FEASIBILITY.md) · [XNU](DARWIN-XNU-INTERFACES.md) · [Real apps](REAL-APP-COMPATIBILITY.md) · [Reuse matrix](REUSE-ADAPTATION-MATRIX.md) · [Licences](THIRD-PARTY-LICENSE-REVIEW.md) · [Top 20](TOP-20-COMPATIBILITY-IMPROVEMENTS.md) · [Roadmap](ORVYN-ROADMAP.md) · [Migration](CIDER-TO-ORVYN-MIGRATION.md)

## Not known

Behaviour on other Windows versions/ARM64/multi-monitor; the three opt-in tests were not re-run; expected output of real binaries (no Mac); ICU availability on supported Windows versions; legal compatibility of APSL with ORVYN's licence.
