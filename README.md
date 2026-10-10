<p align="center"><img src="assets/cider-icon.svg" width="96" alt="CIDER icon"></p>

# CIDER — Cider Is Darwin Executable Runtime

A macOS → Windows compatibility layer, in the spirit of Wine but in the opposite direction.
Not an emulator, not a VM, no macOS and no XNU kernel required.

> **Status: CIDER 0.3.0 — Darwin runtime and dynamic libraries (experimental).**
> CIDER executes machine code from Mach-O x86_64 files on Windows (no emulator, no VM, no XNU) and, since 0.3, links
> **real Mach-O dylibs**: `cider run one` loads `one` + `libmath.dylib`, resolves `_cider_add` through the dylib's export trie, runs the
> dylib's code and prints `Guest exit code: 42`. 0.3 also adds Mach-O initializers, a variadic-capable ABI bridge, an own `printf`
> family, a Darwin file/time layer, chained fixups and a thread-local-storage subset.
> **Only controlled test programs run** (clang + ld64.lld fixtures). **No macOS application runs**: Cocoa, SwiftUI, Foundation,
> AppKit, libobjc and the Swift runtime do not exist; the three real apps tried (Rectangle, Stats, MonitorControl) are refused.
> Static verdicts such as "analysis compatible" still mean "static analysis passed", never "it runs". Execution happens in a
> separate worker process that limits crashes and resources but is **not a sandbox**. See [docs/EXECUTION.md](docs/EXECUTION.md),
> [docs/DYNAMIC-LINKING.md](docs/DYNAMIC-LINKING.md), [docs/DARWIN-RUNTIME.md](docs/DARWIN-RUNTIME.md), [docs/TLS.md](docs/TLS.md) and
> [docs/LIMITATIONS.md](docs/LIMITATIONS.md).

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
| Objective-C, Foundation, AppKit, frameworks, threads, signals, `FILE` streams (`fopen`) | **not implemented** (0.4 – 0.5) |

## Commands

```text
cider --version
cider inspect <program | App.app | image.dmg>   # type is detected from content
cider dmg-info <image.dmg>
cider extract <image.dmg> --output <dir>
cider dependencies <program | App.app>
cider compatibility <program | App.app>
cider run <program | App.app> [-- args]          # EXPERIMENTAL: execute a Mach-O x86_64 program
cider logs [file]
cider doctor
```

Global options: `--json`, `--symbols`, `--log-level trace|debug|info|warn|error|fatal|off`,
`--log-file <path>`, `--7z <path>`, `--max-size <MiB>`, `--temp-dir <dir>`.
Exit codes: `0` ok · `1` invalid input or failure · `2` usage error · `3` partial result (disk image structure
read but the 7-Zip backend is missing). `cider run` has its own table: `0` the guest ran to completion (its exit
code is printed / in `--json`), `10` internal error, `11` file incompatible, `12` library or symbol not available,
`13` load failure, `14` guest crash/trap/timeout. Extra option: `--timeout <seconds>` (run).

Example:

```text
> cider inspect Example.app
CIDER - Darwin Executable Runtime

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
`cider run`.

```text
> cider run tests/fixtures/exec/bin/ret42
[CIDER][LOADER] Mach-O x86_64 detected (3 segment(s), 13 load command(s))
[CIDER][MEMORY] mapping __TEXT at 0x100000000 (0x1000 bytes from file, 0x0 bytes zero-fill)
[CIDER][ABI] preparing the guest execution environment
[CIDER][EXECUTION] entering guest code at 0x1000002d0
[CIDER][EXECUTION] guest returned 42
[CIDER][PROCESS] cleanup completed

Guest exit code: 42
```

## Building

Requirements: Windows 10/11 x64, Visual Studio 2022+ (C++ workload, includes MASM) , CMake ≥ 3.21.
Optional: [7-Zip](https://www.7-zip.org/) for `.dmg` extraction.

```bash
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build/bin/Release/cider.exe --version
```

More: [docs/BUILDING.md](docs/BUILDING.md), [docs/TESTING.md](docs/TESTING.md).

## Documentation

[ARCHITECTURE](docs/ARCHITECTURE.md) · [MACHO](docs/MACHO.md) · [DMG](docs/DMG.md) ·
[EXECUTION](docs/EXECUTION.md) · [LOADER](docs/LOADER.md) · [DARWIN](docs/DARWIN.md) · [ABI](docs/ABI.md) ·
[COMPATIBILITY](docs/COMPATIBILITY.md) · [ROADMAP](docs/ROADMAP.md) · [TESTING](docs/TESTING.md) ·
[REAL-WORLD](docs/REAL-WORLD.md) · [LIMITATIONS](docs/LIMITATIONS.md) · [CHANGELOG](CHANGELOG.md) · [LICENSES](docs/LICENSES.md) · [PROGRESS](docs/PROGRESS.md)

Contributing: [CONTRIBUTING.md](CONTRIBUTING.md). Security: [SECURITY.md](SECURITY.md).
License: MIT ([LICENSE](LICENSE)); third-party notes in [docs/LICENSES.md](docs/LICENSES.md).
