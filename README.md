<p align="center"><img src="assets/orvyn-icon.svg" width="96" alt="ORVYN icon"></p>

# ORVYN — Darwin Executable Runtime (formerly CIDER, “Cider Is Darwin Executable Runtime”)

A macOS → Windows compatibility layer, in the spirit of Wine but in the opposite direction.
Not an emulator, not a VM, no macOS and no XNU kernel required.

> **Status: ORVYN 0.5.0 — AppKit subset, native Windows GUI, Blocks, libdispatch (experimental).**
> ORVYN executes machine code from Mach-O x86_64 files on Windows (no emulator, no VM, no XNU). 0.3 added dylibs, initializers, an own `printf`, files, chained fixups and TLS; 0.4 added callbacks, `FILE` streams,
> cooperative pthreads, an own Apple-ABI Objective-C runtime and a small Foundation. **0.5 adds Blocks, a minimal libdispatch, more Foundation, CoreFoundation basics, CoreGraphics geometry, message forwarding and an
> AppKit subset that opens real Windows windows**: `orvyn run tests\fixtures\appkit\bin\hello_window\hello_window` shows a window created by Objective-C code compiled for macOS; its buttons run methods of that Mach-O
> (`-[AppController buttonClicked:]` increments a counter kept by the program and updates a label), and "Close" ends the program cleanly.
> **Only controlled test programs run** (clang + ld64.lld fixtures). **No macOS application runs and no Cocoa exists**: AppKit is a host-written subset of 9 classes ([docs/APPKIT.md](docs/APPKIT.md)), Foundation a subset
> too; Swift and SwiftUI are not supported, and the real applications analysed (CotEditor, Rectangle, Stats, MonitorControl) are all Swift apps and are refused. Static verdicts such as "analysis compatible" still mean
> "static analysis passed", never "it runs". Execution happens in a separate worker process that limits crashes and resources but is **not a sandbox**.
> See [docs/EXECUTION.md](docs/EXECUTION.md), [docs/APPKIT.md](docs/APPKIT.md), [docs/GUI-BACKEND.md](docs/GUI-BACKEND.md), [docs/EVENT-LOOP.md](docs/EVENT-LOOP.md), [docs/BLOCKS.md](docs/BLOCKS.md),
> [docs/LIBDISPATCH.md](docs/LIBDISPATCH.md), [docs/COREGRAPHICS.md](docs/COREGRAPHICS.md), [docs/APPLE-SDK-RESEARCH.md](docs/APPLE-SDK-RESEARCH.md), [docs/COTEDITOR-COMPATIBILITY.md](docs/COTEDITOR-COMPATIBILITY.md),
> [docs/OBJECTIVE-C-RUNTIME.md](docs/OBJECTIVE-C-RUNTIME.md), [docs/FOUNDATION.md](docs/FOUNDATION.md), [docs/LIMITATIONS.md](docs/LIMITATIONS.md).

## What works (verified by the test-suite and real-world runs, see [docs/PROGRESS.md](docs/PROGRESS.md), [docs/REAL-WORLD.md](docs/REAL-WORLD.md))

| Area | Status |
| --- | --- |
| Mach-O 64-bit parser (x86_64, arm64 headers), FAT / FAT64 | works, strict validation |
| Load commands: segments/sections, `LC_MAIN`, `LC_UNIXTHREAD`, dylibs (load/weak/reexport/lazy/upward), `LC_RPATH`, `LC_ID_DYLIB`, symtab/dysymtab, dyld info, chained fixups/exports trie ranges, build version, code signature (parsed, **not** verified), encryption flag | works |
| Property lists (XML + binary) | works |
| `.app` bundle analysis (Info.plist, executable, frameworks, resources) | works |
| Dependency resolution (`@rpath`, `@executable_path`, `@loader_path`), transitive | static only |
| DMG: UDIF `koly` trailer, partitions, compression types | works |
| DMG extraction | **optional**, through external 7-Zip; limits and path-traversal protection |
| **Run** controlled Mach-O x86_64 programs (loader, W^X mapping, rebase/bind/lazy-bind, SysV→Win64 bridge, 24 libSystem functions, worker process) | works for the fixtures in `tests/fixtures/exec` — [EXECUTION.md](docs/EXECUTION.md) |
| Dependent Mach-O dylibs (`@rpath`/`@loader_path`/`@executable_path`, transitive, re-exports, cycles), `__mod_init_func`/`__init_offsets` | works for the fixtures in `tests/fixtures/dylib` — [DYNAMIC-LINKING.md](docs/DYNAMIC-LINKING.md) |
| Variadic / mixed-signature / stack-argument ABI, `printf` family (own formatter), files, time, errno (Darwin values) | subset, documented in [DARWIN-RUNTIME.md](docs/DARWIN-RUNTIME.md) |
| Chained fixups (`DYLD_CHAINED_PTR_64`, `_64_OFFSET`), TLS (Mach-O TLV protocol, single thread) | works for the fixtures; other formats refused by name — [TLS.md](docs/TLS.md) |
| Host→guest callbacks (`qsort`, `bsearch`, `atexit`), `FILE` streams, cooperative pthreads | subset, fixtures only — [CALLBACKS.md](docs/CALLBACKS.md), [DARWIN-RUNTIME.md](docs/DARWIN-RUNTIME.md), [THREADING.md](docs/THREADING.md) |
| Objective-C (Apple objc2 ABI: classes, categories, protocols, `objc_msgSend`, MRC/ARC) and a Foundation subset (NSObject, NSString, NSNumber, NSArray, NSDictionary, NSLog, pools) | works for the clang-built fixtures in `tests/fixtures/objc` — [OBJECTIVE-C-RUNTIME.md](docs/OBJECTIVE-C-RUNTIME.md), [FOUNDATION.md](docs/FOUNDATION.md) |
| Blocks, libdispatch (main/serial/global queues, groups, semaphores), CoreFoundation basics, CoreGraphics geometry, message forwarding | subset, fixtures only — [BLOCKS.md](docs/BLOCKS.md), [LIBDISPATCH.md](docs/LIBDISPATCH.md), [COREGRAPHICS.md](docs/COREGRAPHICS.md) |
| AppKit subset (NSApplication, NSWindow, NSView, NSControl, NSButton, NSTextField, NSEvent) on native Win32 windows | works for the fixtures in `tests/fixtures/appkit` (headless and with a real window) — [APPKIT.md](docs/APPKIT.md) |
| `@try/@catch`, Swift, SwiftUI, menus, drawing, other frameworks, signals | **not implemented** |

## Commands

```text
orvyn --version
orvyn inspect <program | App.app | image.dmg>   # type is detected from content
orvyn dmg-info <image.dmg>
orvyn extract <image.dmg> --output <dir>
orvyn dependencies <program | App.app>
orvyn compatibility <program | App.app>
orvyn run <program | App.app> [-- args]          # EXPERIMENTAL: execute a Mach-O x86_64 program
orvyn logs [file]
orvyn doctor
orvyn sdk-scan <dir|file.tbd>                  # compare Apple SDK text stubs with what ORVYN implements
```

Global options: `--json`, `--symbols`, `--log-level trace|debug|info|warn|error|fatal|off`,
`--log-file <path>`, `--7z <path>`, `--max-size <MiB>`, `--temp-dir <dir>`.
Exit codes: `0` ok · `1` invalid input or failure · `2` usage error · `3` partial result (disk image structure
read but the 7-Zip backend is missing). `orvyn run` has its own table: `0` the guest ran to completion (its exit
code is printed / in `--json`), `10` internal error, `11` file incompatible, `12` library or symbol not available,
`13` load failure, `14` guest crash/trap/timeout. Extra option: `--timeout <seconds>` (run).

Example:

```text
> orvyn inspect Example.app
ORVYN - Darwin Executable Runtime

Application: Example
Bundle Identifier: com.example.app
Format: Mach-O 64-bit
Architecture: x86_64
Platform: macOS (minimum 10.15, SDK 12.0)
Entry Point: Detected (LC_MAIN, file offset 0xf00, vmaddr 0x100000f00 in __TEXT)
...
Compatibility: analysis compatible
- Static analysis completed
- Execution support not yet confirmed
```

Analysis commands are strictly static: nothing from a Mach-O file or a disk image is executed except by an explicit
`orvyn run`.

```text
> orvyn run tests/fixtures/exec/bin/ret42
[ORVYN][LOADER] Mach-O x86_64 detected (3 segment(s), 13 load command(s))
[ORVYN][MEMORY] mapping __TEXT at 0x100000000 (0x1000 bytes from file, 0x0 bytes zero-fill)
[ORVYN][ABI] preparing the guest execution environment
[ORVYN][EXECUTION] entering guest code at 0x1000002d0
[ORVYN][EXECUTION] guest returned 42
[ORVYN][PROCESS] cleanup completed

Guest exit code: 42
```

AppKit (0.5): a real window, created by Objective-C code in a Mach-O file:

```text
> tools\llvm-env.cmd tools\gen-appkit-fixtures.cmd
> orvyn run tests\fixtures\appkit\bin\hello_window\hello_window       # title "ORVYN — Hello from macOS", buttons "Click me" and "Close"
```

Objective-C and Foundation (0.4), all built from the sources in `tests/fixtures/objc/src` by `tools\llvm-env.cmd tools\gen-objc-fixtures.cmd`:

```text
> orvyn run tests/fixtures/objc/bin/objc_calc/objc_calc                 # Guest exit code: 42 (Objective-C, no Foundation)
> orvyn run tests/fixtures/objc/bin/foundation_hello/foundation_hello   # stderr: ORVYN result: 42 · Guest exit code: 42
```

## Building

Requirements: Windows 10/11 x64, Visual Studio 2022+ (C++ workload, includes MASM) , CMake ≥ 3.21.
Optional: [7-Zip](https://www.7-zip.org/) for `.dmg` extraction.

```bash
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build/bin/Release/orvyn.exe --version
```

More: [docs/BUILDING.md](docs/BUILDING.md), [docs/TESTING.md](docs/TESTING.md).

## Documentation

[ARCHITECTURE](docs/ARCHITECTURE.md) · [MACHO](docs/MACHO.md) · [DMG](docs/DMG.md) ·
[EXECUTION](docs/EXECUTION.md) · [LOADER](docs/LOADER.md) · [DARWIN](docs/DARWIN.md) · [ABI](docs/ABI.md) ·
[COMPATIBILITY](docs/COMPATIBILITY.md) · [ROADMAP](docs/ROADMAP.md) · [TESTING](docs/TESTING.md) ·
[APPKIT](docs/APPKIT.md) · [GUI-BACKEND](docs/GUI-BACKEND.md) · [EVENT-LOOP](docs/EVENT-LOOP.md) · [BLOCKS](docs/BLOCKS.md) · [LIBDISPATCH](docs/LIBDISPATCH.md) · [COREGRAPHICS](docs/COREGRAPHICS.md) ·
[APPLE-SDK-RESEARCH](docs/APPLE-SDK-RESEARCH.md) · [COTEDITOR-COMPATIBILITY](docs/COTEDITOR-COMPATIBILITY.md) · [OBJECTIVE-C-RUNTIME](docs/OBJECTIVE-C-RUNTIME.md) · [FOUNDATION](docs/FOUNDATION.md) · [CALLBACKS](docs/CALLBACKS.md) · [THREADING](docs/THREADING.md) · [TLS](docs/TLS.md) ·
[REAL-WORLD](docs/REAL-WORLD.md) · [LIMITATIONS](docs/LIMITATIONS.md) · [CHANGELOG](CHANGELOG.md) · [LICENSES](docs/LICENSES.md) · [PROGRESS](docs/PROGRESS.md)

Contributing: [CONTRIBUTING.md](CONTRIBUTING.md). Security: [SECURITY.md](SECURITY.md).
License: MIT ([LICENSE](LICENSE)); third-party notes in [docs/LICENSES.md](docs/LICENSES.md).
