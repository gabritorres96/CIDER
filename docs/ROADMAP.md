# Roadmap

| Version | Goal | Acceptance | State |
| --- | --- | --- | --- |
| 0.1 Inspector | Mach-O/FAT/plist/.app/DMG static analysis, CLI, logs, tests, docs | builds on Windows; valid and malformed fixtures analysed correctly | **done** (see PROGRESS.md for evidence and gaps) |
| 0.2 Runtime experimental | memory mapping, minimal loader, ABI trampolines, basic symbol resolution, first functions | run a controlled x86_64 Mach-O and get its real exit code | **done for controlled fixtures** (see EXECUTION.md; not general macOS compatibility) |
| 0.3 Darwin runtime | dylibs, initializers, variadic ABI, printf/stdio, files, time, chained fixups, TLS design | executable + dylibs returning 42, printf, files, chained fixups | **implemented for controlled fixtures** (see PROGRESS.md; threads, sync and FILE streams still missing) |
| 0.4 Objective-C | host→guest callbacks, FILE streams, cooperative threads, own Apple-ABI Objective-C runtime (libobjc2 evaluated, not used), Foundation subset | controlled Objective-C Mach-O program printing `ORVYN result: 42` and returning 42 | **implemented for controlled fixtures** (see PROGRESS.md; not Cocoa) |
| 0.5 AppKit experimental | Blocks, libdispatch, Foundation/CoreFoundation/CoreGraphics subsets, forwarding, NSApplication/NSWindow/NSView/NSButton/NSTextField on native Win32 windows, target/action | a Mach-O Objective-C program creates and controls a real Windows window | **implemented for controlled fixtures** (see PROGRESS.md; not Cocoa, no Swift) |
| 0.6 Drawing foundations | guest CoreGraphics and `drawRect:` (done for fixtures), then relative method lists, weak binding, libc basics, `Cocoa` alias, `@synchronized`/associated objects | a macOS 11+ Objective-C fixture links and runs | **partly done** (0.6.1–0.6.2 drawing; relative method lists, weak binding, Cocoa umbrella, framework stubs, libc basics, `@synchronized`/associated objects and exception-tolerant linking done for fixtures - no external binary runs yet); full plan 0.6 → 1.0 in [research/ORVYN-ROADMAP.md](research/ORVYN-ROADMAP.md) |
| 1.0 | — | reproducible compatibility with a defined set of real applications; **no date promised** | — |

Gate: 0.2 does not start before 0.1's acceptance criteria are met and reviewed. ARM64 support is a
later extension (see ABI.md). Metal, Vulkan/DirectX backends are out of scope for early phases.
