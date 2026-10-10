<p align="center"><img src="assets/cider-icon.svg" width="96" alt="CIDER icon"></p>

# CIDER — Cider Is Darwin Executable Runtime

A macOS → Windows compatibility layer, in the spirit of Wine but in the opposite direction.
Not an emulator, not a VM, no macOS and no XNU kernel required.

> **Status: CIDER 0.1.1 — static inspector only, validated against real binaries and DMGs.**
> CIDER 0.1 **cannot run any macOS program.** It reads and validates Mach-O files, `.app`
> bundles and `.dmg` images. `cider run` exists only to say so. No application is claimed to be
> compatible: every "compatible" verdict in this release means *"static analysis passed"*, never
> *"it runs"*. See [docs/LIMITATIONS.md](docs/LIMITATIONS.md).

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
| Loader, dyld, libSystem, Objective-C, Foundation, AppKit | **not implemented** (0.2 – 0.5) |

## Commands

```text
cider --version
cider inspect <program | App.app | image.dmg>   # type is detected from content
cider dmg-info <image.dmg>
cider extract <image.dmg> --output <dir>
cider dependencies <program | App.app>
cider compatibility <program | App.app>
cider run <...>                                 # not available in 0.1 (exit code 2)
cider logs [file]
cider doctor
```

Global options: `--json`, `--symbols`, `--log-level trace|debug|info|warn|error|fatal|off`,
`--log-file <path>`, `--7z <path>`, `--max-size <MiB>`, `--temp-dir <dir>`.
Exit codes: `0` ok · `1` invalid input or failure · `2` usage error / unavailable command ·
`3` partial result (disk image structure read but the 7-Zip backend is missing).

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

Analysis is strictly static: nothing from a Mach-O file or a disk image is ever executed.

## Building

Requirements: Windows 10/11, Visual Studio 2022+ (C++ workload) or any C++20 compiler, CMake ≥ 3.21.
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
[LOADER](docs/LOADER.md) · [DARWIN](docs/DARWIN.md) · [ABI](docs/ABI.md) ·
[COMPATIBILITY](docs/COMPATIBILITY.md) · [ROADMAP](docs/ROADMAP.md) · [TESTING](docs/TESTING.md) ·
[REAL-WORLD](docs/REAL-WORLD.md) · [LIMITATIONS](docs/LIMITATIONS.md) · [CHANGELOG](CHANGELOG.md) · [LICENSES](docs/LICENSES.md) · [PROGRESS](docs/PROGRESS.md)

Contributing: [CONTRIBUTING.md](CONTRIBUTING.md). Security: [SECURITY.md](SECURITY.md).
License: MIT ([LICENSE](LICENSE)); third-party notes in [docs/LICENSES.md](docs/LICENSES.md).
