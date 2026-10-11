# Apple / Swift / GNUstep open-source inventory for ORVYN

Date of reference: **2026-10-11**. All repositories were cloned **read-only** with `--depth 1` (large ones with a partial/sparse clone) into a scratch directory outside the repository; nothing was built, run or copied into ORVYN.
Sizes were checked first through the GitHub API (`api.github.com/repos/<org>/<repo>` → `size`), which is why `swift` (1.4 GB) and `xnu` (178 MB) were fetched sparsely and the 7 GB community mirror was not cloned at all.
Labels: **VERIFIED** (read in the clone or API), **INFERRED**, **HYPOTHESIS**.

## 1. Where the code lives — official vs mirrors

| Source | What it is | Classification |
|---|---|---|
| `github.com/apple-oss-distributions/*` | organisation described as "OSS Code distributed by Apple, Inc." with blog link `https://opensource.apple.com`; 509 public repositories; **not GitHub-verified** (`is_verified: false`) | **Official Apple distribution (INFERRED from the organisation profile and from `opensource.apple.com` saying the code is on "their respective GitHub pages"; the page does not name the organisation)** |
| `github.com/apple-oss-distributions/distribution-macOS` | the *index*: one directory per project and `release.json` listing the tag of every project for a release; tag `macos-260` = "macOS 26.0", 171 projects | official (same org) |
| `github.com/apple-open-source/macos` | "The open-source components of macOS" in a single repository, 7.2 GB, 253 stars; the organisation itself says **"Unofficial mirror of code at opensource.apple.com"**; created 2019 | **community mirror, not the source**; not cloned. Useful for *old* releases and history only |
| `github.com/swiftlang/swift`, `swift-corelibs-foundation`, `swift-corelibs-libdispatch` | the Swift project (Apache-2.0 with Runtime Library Exception) | official Swift project. **URL change:** `github.com/apple/swift` and `github.com/apple/swift-corelibs-libdispatch` answer "Moved Permanently": the repositories now live under `swiftlang` |
| `github.com/gnustep/libobjc2`, `gnustep/libs-gui` | GNUstep, independent of Apple | third party |
| `opensource.apple.com` | Apple's portal (releases listing is JavaScript-rendered; not machine-readable here) | official portal |

`distribution-macOS` shows what is **not** open: there is no `AppKit`, `Foundation`, `CoreGraphics`, `CoreText`, `CoreAnimation`, `SwiftUI`, `libswiftCore` (as Darwin library) or `libc++` directory in its 171 projects (checked by name in `release.json`).
Open: `CF` is a *separate, old* repository (see below), `Security`, `CommonCrypto`, `WebKit`, `ICU`, `libclosure`, `libplatform`, `libmalloc`, `libpthread`, `Libc`, `Libsystem`, `xnu`, `dyld`, `objc4`, `libdispatch`, `llvmCore`.

## 2. Components studied

Versions come from `release.json` of tag `macos-260` ("macOS 26.0"). Where the repository's `main` is newer than that release, **the release tag was studied** (and `main` is only noted) so that no two macOS versions are mixed.

| Component | URL | Org | Tag / version studied | Commit | Date | License (verified file) | Depends on | Archs | Windows? | Relevance to ORVYN |
|---|---|---|---|---|---|---|---|---|---|---|
| dyld | github.com/apple-oss-distributions/dyld | apple-oss-distributions | `dyld-1323.3` (macOS 26.0). `main` = `dyld-1378` `fd8d0c4` 2026-06-15 | `6379117` | 2025-10-16 | APSL 2.0 (`APPLE_LICENSE`, Aug 6 2003) | Mach VM, libSystem helpers, shared cache | arm64, arm64e, x86_64 | No (Darwin-only) | **Reference** for the loader: [DYLD-COMPATIBILITY.md](DYLD-COMPATIBILITY.md) |
| objc4 | .../objc4 | same | `objc4-950` (macOS 26.0). `main` = `objc4-951.1` `fb26509` 2025-12-01 | `6dee1d7` | 2025-10-16 | APSL 2.0 | dyld notifications, libSystem, `libc++`, Mach | same | No | **Reference** for the Apple ABI that guests were compiled against: [OBJC-RUNTIME-COMPATIBILITY.md](OBJC-RUNTIME-COMPATIBILITY.md) |
| libdispatch | .../libdispatch | same | `libdispatch-1542.0.4` (= macOS 26.0 and `main`) | `2361ffb` | 2025-10-16 | **Apache 2.0** (`LICENSE`; GitHub reports Apache-2.0) | Mach/kqueue/workqueue on Darwin | all | No (the Swift fork has a Windows port, below) | Reference + design source: [LIBDISPATCH-RESEARCH.md](LIBDISPATCH-RESEARCH.md) |
| libpthread | .../libpthread | same | `libpthread-539` | `42d026d` | 2025-10-16 | no root license file; per-file APSL 2.0 headers (`src/pthread.c`: "@APPLE_LICENSE_HEADER_START@ ... Version 2.0") | Mach, kernel pthread syscalls | all | No | Reference for pthread semantics |
| Libc | .../Libc | same | `Libc-1725.0.11` (macOS 26.0). `main` = `Libc-1752.100.10` `71bbe35` | `55b54c0` | 2025-10-16 | APSL 2.0 (+ BSD-licensed files inherited from FreeBSD, per-file) | Libsystem, kernel syscalls | all | No | Reference for libc semantics: [LIBSYSTEM-LIBC-ANALYSIS.md](LIBSYSTEM-LIBC-ANALYSIS.md) |
| Libsystem | .../Libsystem | same | `Libsystem-1356` | `c23b511` | 2025-10-16 | APSL 2.0 | umbrella | all | No | the list of what `libSystem.B.dylib` re-exports |
| xnu | .../xnu (sparse: `osfmk/mach`, `libsyscall/mach`, `bsd/sys/syscall.h`) | same | `xnu-12377.1.9` | `f6217f8` | 2025-10-16 | APSL 2.0 (some BSD files) | — | arm64, x86_64 | No | Reference for Mach ports/IPC and syscall numbers: [DARWIN-XNU-INTERFACES.md](DARWIN-XNU-INTERFACES.md) |
| CF (CoreFoundation) | .../CF | same | `CF-1153.18` | `dc54c6b` | **2021-10-06** (last push: the repository is **old**; it is absent from the macOS 26.0 `release.json`, and the CF version/era was not mapped to a macOS release in this research) | APSL 2.0 | Darwin | — | no | old reference; **superseded for our purpose by the Swift fork's CoreFoundation** |
| Swift compiler/runtime/stdlib | github.com/swiftlang/swift (sparse: `stdlib/public/runtime`, `SwiftShims`, `include/swift/ABI`, `docs/ABI`, `include/swift/Runtime`) | swiftlang | `main` | `c2b0f68` | 2026-10-10 | Apache 2.0 **with Runtime Library Exception** (`LICENSE.txt:205`) | LLVM, ICU optional, per-platform shims | x86_64, arm64, others | **Yes** (builds for Windows PE; has `ImageInspectionCOFF.cpp`, `CrashHandlerWindows.cpp`, `SwiftRT-COFF.cpp`, `WindowsUnicode.cpp`) | the runtime ABI and metadata: [SWIFT-RUNTIME-FEASIBILITY.md](SWIFT-RUNTIME-FEASIBILITY.md) |
| swift-corelibs-foundation | github.com/swiftlang/swift-corelibs-foundation | swiftlang | `main` | `384ff68` | 2026-10-09 | Apache 2.0 (`LICENSE`); the vendored C sources carry "Licensed under Apache License v2.0 with Runtime Library Exception" (`Sources/CoreFoundation/CFString.c`, header) | CoreFoundation C sources (96 files, 11 MB), libcurl, libxml2, ICU | all supported by Swift | **Yes** (`TARGET_OS_WIN32` branches, e.g. `CFString.c:441`) | the *only* open, permissively licensed CoreFoundation implementation: [FOUNDATION-COREFOUNDATION.md](FOUNDATION-COREFOUNDATION.md) |
| swift-corelibs-libdispatch | github.com/swiftlang/swift-corelibs-libdispatch | swiftlang | `main` | `bcae657` | 2026-10-09 | Apache 2.0 | on Windows `src/event/event_windows.c` + thread pool | all | **Yes** | candidate for adaptation: [LIBDISPATCH-RESEARCH.md](LIBDISPATCH-RESEARCH.md) |
| libobjc2 | github.com/gnustep/libobjc2 | GNUstep | `main` | `aabc524` | 2026-10-08 | **MIT** (`COPYING`, "Copyright (c) 2009 David Chisnall") | LLVM-compatible compilers | x86_64, arm64, ... | yes | NOT Apple-ABI compatible (class layout differs): see OBJC doc |
| GNUstep GUI (`libs-gui`) | github.com/gnustep/libs-gui | GNUstep | `master` | (not cloned) | pushed 2026-10-10 | **LGPL v2 or later** in the source headers (`Source/NSView.m`) and `COPYING.LIB`; GitHub's auto-detection shows GPL-3.0 because `COPYING` is also present | gnustep-base, gnustep-back | — | partial (the `gnustep-back` "win32" backend) | reference only: [APPKIT-COREGRAPHICS.md](APPKIT-COREGRAPHICS.md) |

## 3. Other repositories mentioned by the task

| URL | Result |
|---|---|
| `github.com/apple-open-source/macos` | exists; unofficial mirror (section 1) |
| `github.com/apple-oss-distributions/distribution-macOS` | exists; `macos-260` is the latest tag seen; `release.json` pins every project |
| `github.com/apple/swift-corelibs-libdispatch` | moved to `swiftlang/swift-corelibs-libdispatch` |
| `github.com/swiftlang/swift` | exists (1.4 GB; sparse clone, 17 MB checked out) |
| `github.com/swiftlang/swift-corelibs-foundation` | exists (27 MB) |
| `github.com/apple-oss-distributions/{dyld,xnu}` | exist |
| `opensource.apple.com` | up (HTTP 200); contents are rendered by JavaScript, so tags were taken from `distribution-macOS` instead |

## 4. Rules adopted for this research

* No file from these repositories was copied into ORVYN; no code was compiled or executed. Everything here is reading, line citations and licence review ([THIRD-PARTY-LICENSE-REVIEW.md](THIRD-PARTY-LICENSE-REVIEW.md)).
* Citations name the **tag** (`dyld-1323.3`, `objc4-950`, `libdispatch-1542.0.4`, `Libc-1725.0.11`, `xnu-12377.1.9`) or, for the Swift project, the commit of `main`.
* A behavior that only exists on Darwin is called out as such; a behavior shown in the Windows-capable Swift forks is **not** assumed to apply to Darwin binaries.
* Reproduce the clones with: `git clone --depth 1 --branch <tag> https://github.com/apple-oss-distributions/<repo>`; for Swift: `git clone --depth 1 --filter=blob:none --sparse https://github.com/swiftlang/swift` then `git sparse-checkout set stdlib/public/runtime stdlib/public/SwiftShims include/swift/ABI docs/ABI include/swift/Runtime`.
