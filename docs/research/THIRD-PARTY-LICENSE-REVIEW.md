# Third-party licence review

**This is an engineering reading of licence files, not legal advice.** Before ORVYN copies or distributes any third-party code, a lawyer should confirm the conclusions below. Licence texts were read in the cloned repositories on 2026-10-11 (tags in [APPLE-OPEN-SOURCE-INVENTORY.md](APPLE-OPEN-SOURCE-INVENTORY.md)). ORVYN itself is MIT (`LICENSE`, [LICENSES.md](../LICENSES.md)).
Labels: **VERIFIED** (file read), **INFERRED**, **PENDING** (to confirm per file or by a lawyer).

## 1. Summary

| Component | Licence | Verified in | Copy into ORVYN? | Obligations if copied |
|---|---|---|---|---|
| dyld, objc4, Libc, Libsystem, libpthread, xnu, CF (old) | **APSL 2.0** | `APPLE_LICENSE` and per-file headers ("@APPLE_LICENSE_HEADER_START@ … Version 2.0") | **Not now.** Reference only | file-level copyleft: modified APSL files must be published under APSL; notices kept; patent clauses; changes must be described; terms differ from Apache/MIT and need review for combination with ORVYN's licence |
| libdispatch (Apple) | **Apache-2.0** | `LICENSE` | Possible after review | licence copy, NOTICE, mark modifications |
| swiftlang/swift (runtime, stdlib) | Apache-2.0 **with Runtime Library Exception** | `LICENSE.txt:205` | Possible after review | as above; the exception removes the attribution requirement for the compiled-output case, not for source copies |
| swift-corelibs-foundation / CoreFoundation C | Apache-2.0; vendored C files say "Apache License v2.0 with Runtime Library Exception" | `LICENSE`, `Sources/CoreFoundation/CFString.c` header | Possible per file | as above |
| swift-corelibs-libdispatch | Apache-2.0 | `LICENSE` | Possible after review | as above |
| libobjc2 (GNUstep) | **MIT** | `COPYING` ("Copyright (c) 2009 David Chisnall") | Possible; **ABI differs**, little reuse value | keep copyright and permission notice |
| GNUstep GUI (`libs-gui`) | **LGPL v2 or later** | source headers (`Source/NSView.m`), `COPYING.LIB`; GitHub shows GPL-3.0 because `COPYING` is also present | **No.** Behavior reference only | LGPL relinking / replacement obligations would apply to ORVYN's distribution |
| libclosure (Blocks) | header uses the placeholder `@APPLE_LLVM_LICENSE_HEADER@`; the effective licence **needs to be resolved** | repository read | **Not copied** (ORVYN has its own Blocks implementation) | PENDING |
| Test sample apps (cliclick BSD-3, Flycut MIT, KeepingYouAwake MIT, Sloth BSD-3; terminal-notifier unverified) | see [REAL-APP-COMPATIBILITY.md](REAL-APP-COMPATIBILITY.md) | licence files / GitHub | Not redistributed in ORVYN's repo | if ever bundled as fixtures: keep their licence files |

## 2. Reading Apple's code vs copying it

* Reading, studying and writing a behaviorally equivalent implementation does not copy the code (INFERRED; the clean-room discipline is: write from documented behavior, cite source lines in docs, never paste).
* ORVYN's research documents quote **file names, line numbers and short facts**, not code blocks.
* Apple's closed frameworks (AppKit, Foundation, CoreGraphics, SwiftUI, libc++ as a Darwin library) are **not** to be extracted from a Mac, a DMG or an IPSW and redistributed with ORVYN. Using a user's own macOS app bundles as *input* is different from shipping Apple libraries.

## 3. Trademarks and naming

Apple, macOS, Cocoa, AppKit, Mach-O's marks and Swift are owned by their holders. ORVYN is independent and not affiliated with or endorsed by Apple; keep that disclaimer in `README.md` and release notes. The planned launcher (0.7) may be *inspired by* the macOS look but must not use Apple logos, icons, fonts (San Francisco is not redistributable) or the wording that suggests endorsement.

## 4. Open questions (must be closed before any copy)

1. Are APSL 2.0 and ORVYN's own licence compatible for the intended distribution? A lawyer should answer; until then the default is **strategy C** (reimplement) for all APSL code.
2. Per-file licence of `Libc/gdtoa` and the BSD-origin files in Libc (permissive notices expected, **PENDING per file**).
3. Effective licence of libclosure after the header placeholder.
4. Whether an Apache-2.0 NOTICE file is needed for ORVYN as a whole once any Apache code is included.
5. ICU: system `icu.dll` (no redistribution) vs bundled ICU (Unicode licence terms).
6. Whether distributing compatibility stubs named after Apple libraries (`Cocoa`, `ApplicationServices`) raises any trademark concern (INFERRED: they are interface names; PENDING).

## 5. Practical rules

* Extend [LICENSES.md](../LICENSES.md) and add `THIRD-PARTY-NOTICES.md` with the first copied file.
* Every copied file keeps its header and gets a "modified by ORVYN" line.
* CI check (to design): grep the tree for `APPLE_LICENSE_HEADER` outside `docs/` and fail if found.
