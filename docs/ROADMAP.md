# Roadmap

| Version | Goal | Acceptance | State |
| --- | --- | --- | --- |
| 0.1 Inspector | Mach-O/FAT/plist/.app/DMG static analysis, CLI, logs, tests, docs | builds on Windows; valid and malformed fixtures analysed correctly | **done** (see PROGRESS.md for evidence and gaps) |
| 0.2 Runtime experimental | memory mapping, minimal loader, ABI trampolines, basic symbol resolution, first functions | run a controlled x86_64 Mach-O and get its real exit code | **done for controlled fixtures** (see EXECUTION.md; not general macOS compatibility) |
| 0.3 Darwin runtime | files, memory, threads, time, sync, partial libSystem | several console test programs | not started |
| 0.4 Objective-C | evaluate/integrate a free runtime (GNUstep libobjc2), partial Foundation | controlled Objective-C program with behavioural tests | not started |
| 0.5 Cocoa experimental | partial NSApplication/NSWindow/NSView, events, Win32 backend | graphical test app showing a working window | not started |
| 1.0 | — | reproducible compatibility with a defined set of real applications; **no date promised** | — |

Gate: 0.2 does not start before 0.1's acceptance criteria are met and reviewed. ARM64 support is a
later extension (see ABI.md). Metal, Vulkan/DirectX backends are out of scope for early phases.
