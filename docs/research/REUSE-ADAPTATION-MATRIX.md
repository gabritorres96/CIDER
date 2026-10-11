# Reuse / adaptation matrix

Decides, per component, how ORVYN obtains each capability. Strategies: **A** keep own implementation · **B** adapt open-source code (copy with attribution, modified) · **C** reimplement from the documented behavior using the source only as a reference · **D** link an external library unchanged · **E** do not do.
Labels: **VERIFIED**, **INFERRED**, **HYPOTHESIS**, **PENDING VALIDATION**. Nothing from Apple or third parties has been copied into ORVYN so far. Licence consequences are in [THIRD-PARTY-LICENSE-REVIEW.md](THIRD-PARTY-LICENSE-REVIEW.md); sources and tags in [APPLE-OPEN-SOURCE-INVENTORY.md](APPLE-OPEN-SOURCE-INVENTORY.md).

## 1. Matrix

| Capability | Candidate source | Strategy | Why | Licence impact | Effort | Detail |
|---|---|---|---|---|---|---|
| Mach-O loading, fixups, binding | dyld-1323.3 | **C** (spec only) | dyld assumes Mach VM, shared cache, libSystem helper vtable; ORVYN's loader is small and tested | none (nothing copied) | S per feature | [DYLD-COMPATIBILITY.md](DYLD-COMPATIBILITY.md) |
| Mach-O structure decoding | `dyld common/MachOFile.cpp` | **C** | reference for edge cases of load commands | none | S | same |
| `dlopen`/`dlsym`/`_dyld_*` | dyld (semantics) | **C** | needs ORVYN's own "load more images" API | none | M | same |
| Objective-C runtime | objc4-950 (APSL), libobjc2 (MIT) | **A** + **C** | libobjc2 has a different class layout (not Apple-ABI); objc4 is Darwin-bound; ORVYN already has its own Apple-ABI runtime | none | S–M per function | [OBJC-RUNTIME-COMPATIBILITY.md](OBJC-RUNTIME-COMPATIBILITY.md) |
| Relative (small) method lists | objc4 `runtime/objc-runtime-new.h` | **C** | layout is public ABI; trivial decoder | none | S | same |
| `@try/@catch`, C++ exceptions | LLVM libunwind / libc++abi (Apache-2.0 w/ LLVM exception) | **HYPOTHESIS** (B or D) | needs an unwinder over `__unwind_info`/`__eh_frame` for guest frames; not studied beyond feasibility | PENDING review | L | OBJC doc §5 |
| libc string/stdio/ctype | Libc-1725.0.11 (APSL + per-file BSD) | **C**; **B** only for per-file permissive code after review (e.g. `gdtoa`) | Windows CRT already covers most; semantics differ in a few places | per-file | S each | [LIBSYSTEM-LIBC-ANALYSIS.md](LIBSYSTEM-LIBC-ANALYSIS.md) |
| `os_log`, `os_unfair_lock` | libplatform / Libc (APSL) | **C** | tiny; `os_log` needs its own descriptor decoder | none | S–M | same |
| Mach / XNU interfaces | xnu-12377.1.9 headers | **C** | emulate a small explicit table; never raw syscalls | none | S | [DARWIN-XNU-INTERFACES.md](DARWIN-XNU-INTERFACES.md) |
| Blocks runtime | libclosure | **A** (exists) | ABI subset implemented and tested | none | — | [BLOCKS.md](../BLOCKS.md) |
| libdispatch | libdispatch-1542.0.4 (Apache), swift-corelibs-libdispatch (Apache) | **A** extended; **C** from `source.c`, `event_windows.c` | real libdispatch runs guest blocks on host threads; the bridge is single-threaded | Apache NOTICE only if code is copied | M per feature | [LIBDISPATCH-RESEARCH.md](LIBDISPATCH-RESEARCH.md) |
| Hash tables with CF semantics | swift-corelibs-foundation `CFBasicHash.c` | **B** or **C** | removes the linear-scan `NSDictionary`; self-contained C | Apache-2.0 + Runtime Library Exception | M | [FOUNDATION-COREFOUNDATION.md](FOUNDATION-COREFOUNDATION.md) |
| String encodings, Unicode tables | corelibs `CFStringEncodingConverter*.c`, `CFUnicode*.c` | **B** | tables + code, no Darwin dependency visible | same | M | same |
| URL parsing | corelibs `CFURL.c` | **C**; **B** if edge cases cost too much | big (5,494 lines) | same | M–L | same |
| Property lists | corelibs `CFPropertyList.c` | **A** (own reader) + oracle | already implemented | none | — | same |
| Run loop | corelibs `CFRunLoop.c` (Windows branch) | **C** | design reference for waiting on messages/handles/timers | none | M | same |
| Calendar / number / date formatting | ICU via Windows `icu.dll` or bundled | **D** | ICU availability per Windows version **PENDING VALIDATION** | ICU licence (Unicode) if bundled | L | same |
| Foundation/AppKit Swift overlays | Apple frameworks (closed) | **E** | compiled Swift inside closed frameworks | — | — | [SWIFT-RUNTIME-FEASIBILITY.md](SWIFT-RUNTIME-FEASIBILITY.md) |
| Swift runtime for Mach-O guests | swiftlang/swift `stdlib/public/runtime` | **B** (PoC first) | open, finite measured import list; large investment | Apache-2.0 + Runtime Library Exception | XL | same |
| AppKit | none (closed) | **A**; **C** with GNUstep as behavior reference | GNUstep targets GNUstep Base and the GNU runtime | reading LGPL code is free; copying is not | XL | [APPKIT-COREGRAPHICS.md](APPKIT-COREGRAPHICS.md) |
| CoreGraphics | none (closed) | **A** (Direct2D) | done for fixtures | none | — | same |
| Text layout | DirectWrite | **D** (Windows API) | right layer for glyphs/metrics | none | L | same |
| Images | WIC | **D** (Windows API) | decode/encode exist | none | S–M | same |

## 2. Rules for any copy (strategy B)

1. One file or function at a time, recorded in `THIRD-PARTY-NOTICES` (does not exist yet; create it with the first copy) with source URL, tag/commit, original licence and the list of modifications.
2. Original licence header kept; modified files marked as modified.
3. The file's licence is read **in the file**, not inferred from the repository: dyld, objc4, Libc and xnu carry per-file APSL headers, Libc also BSD-style files.
4. A regression fixture (clang-built) must fail before and pass after; the exact-registry test must be updated.
5. APSL code is not copied until the review in the licence document is closed (see its section 4).

## 3. Not recommended

* Porting dyld's `Loader`/closure machinery.
* Swapping ORVYN's Objective-C runtime for libobjc2 or objc4.
* Linking swift-corelibs-libdispatch wholesale before the bridge supports parallel guest execution.
* Treating GNUstep GUI as an AppKit substitute for Mach-O guests.
