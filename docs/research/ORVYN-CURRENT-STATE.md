# ORVYN — current state (verified)

Everything here was checked in the repository and by running the tests on **2026-10-11**. Labels: **VERIFIED** (observed in code or by a run), **INFERRED**, **HYPOTHESIS**,
**PENDING VALIDATION**. Nothing comes from memory of an earlier report: the last communicated commit `6510583` was already superseded.

## 1. Repository

| Item | Value |
|---|---|
| Branch / remote | `main`; `origin https://github.com/gabritorres96/CIDER.git` (fetch/push) — **not modified, nothing was ever pushed or pulled** |
| HEAD at the start of this research | `aa8f0bf docs: document CoreGraphics guest rendering and NSView drawRect; harden GDI leak check` |
| Commits since `6510583` | `c12f8a7` guest CoreGraphics API + drawRect bridge · `6f01b58` fixtures and tests · `39be4da` dead-handle fix · `0bce9f8` fuzz-found arc hang · `aa8f0bf` docs · `1b77aa7` rename CIDER → ORVYN |
| Version | **0.5.0** (`CMakeLists.txt` `project(ORVYN VERSION 0.5.0)`, `include/orvyn/util.hpp kVersion`). The 0.6 work and the rename did **not** change it |
| `utilities/` | untracked, ignored (`.gitignore:13:/utilities/`), never touched |
| Tree | clean after each commit; 1,093 tracked files |
| Size of the host runtime (lines of `src/`, `.cpp/.hpp/.inc/.asm`) | foundation 3,353 · graphics 2,959 · libsystem 2,683 · appkit 1,831 · cli 1,605 · objc 1,587 · corefoundation 1,301 · dmg 776 · macho 732 · dyld 700 · linker 610 · dispatch 583 · process 539 · abi 511 · core 482 · loader 277 · memory 188 · analysis 184 · sdk 155 · bundle 151 · logging 103 · security 79 |

## 2. Baseline of builds and tests (Windows 11 Pro 10.0.26200, MSVC 14.51 + ml64, clang-cl/LLVM 23.1.3, CMake 4.2.3)

| Check | Result |
|---|---|
| Debug build + `ctest` | 39/39 suites pass; **509 tests run, 506 passed, 0 failed, 3 skipped** (per-executable counts) |
| Release build + `ctest` | same: 39/39; 509 / 506 / 0 / 3 |
| Fresh build from scratch (new directory, Release) before the rename | configure + build rc 0; 39/39 |
| AddressSanitizer (clang-cl; all 39 suites, GUI tests included), before and after the rename | 39/39 pass |
| Same checks after the rename (`1b77aa7`), Debug and Release | 39/39 |
| libFuzzer, new this milestone: `fuzz_cg` | 944,443 runs / 150 s, after fixing the hang it found |
| libFuzzer: `fuzz_linker`, `fuzz_objc` (120 s each) | 1,398,896 and 6,065 runs; no crash, leak or timeout. `fuzz_macho`, `fuzz_loader`, `fuzz_chained`, `fuzz_tbd`, `fuzz_plist`, `fuzz_udif` were **not** re-run in this session (their code did not change) |
| Interactive GUI tests (`ctest -L gui`) | pass on this machine (a real desktop is available); they skip automatically without one. The 3 skipped tests are the ones that need a resource this machine does not provide |
| GitHub Actions | **never ran** (no push). Not validated |
| Platforms | one machine, Windows x86_64 only, one monitor |

The 3 skipped tests (VERIFIED by running every suite): `test_dmg::real_world_dmgs_are_parsed_and_extracted` (needs `ORVYN_REAL_DMG_DIR`), `test_real_macho::live_llvm_objdump_still_agrees_with_the_golden_snapshots`
(needs `ORVYN_LLVM_BIN`) and `test_real_macho::real_world_extracted_apps_match_llvm_objdump` (needs `ORVYN_REAL_APPS_DIR`). They are opt-in checks against the real Apple downloads and a live `llvm-objdump`, not part of the default run.

## 3. Component status (what exists, what was proven)

Legend: **Working (fixture)** = exercised by a clang-built Mach-O test program; no Apple-built application has ever run.

| Area | State | Evidence |
|---|---|---|
| Mach-O parsing / FAT / `.app` / DMG inspection | Working | `test_macho`, `test_fat`, `test_bundle`, `test_dmg`, `fuzz_macho`, `fuzz_udif` |
| Loader (x86_64 `MH_EXECUTE`, `MH_DYLIB`, W^X mapping) | Working (fixture) | `test_loader`, `test_entry`, `src/loader/` (277 lines), `src/linker/linker.cpp` (610) |
| Fixups | classic rebase/bind/lazy-bind (applied eagerly), chained fixups formats `DYLD_CHAINED_PTR_64` and `_64_OFFSET`; **arm64e formats, weak-binding streams, threaded binds rejected** | `src/dyld/chained_fixups.cpp:1-10`, `src/dyld/relocation_engine.cpp:243`, `src/security/execution_validator.cpp:39` |
| TLS | `__thread_vars` served by a host `tlv_get_addr` (the guest never touches `%gs`) | `src/libsystem/tls.cpp:1-60`, `test_tls` |
| libSystem / libc subset | 169 host functions + data symbols (stdio, FILE streams, POSIX files, pthreads, time, string, math, qsort/bsearch/atexit, blocks, dispatch) | `orvyn sdk-scan @implemented`; exact registry asserted by `test_runtime` |
| Objective-C runtime | own implementation of the Apple ABI (class/metaclass/selector/method lists, `objc_msgSend` family, ARC entry points, weak refs, autorelease pools, categories, protocols, forwarding), 82 `libobjc` symbols; `@try/@catch` **refused** | `src/objc/` (1,587 lines), `test_objc`, `test_objc_dynamic`, `fuzz_objc` |
| Foundation | 20 function symbols + classes `NSObject NSString NSMutableString NSNumber NSArray NSMutableArray NSDictionary NSMutableDictionary NSData NSMutableData NSDate NSURL NSError NSFileManager NSNotification(Center) NSMethodSignature NSInvocation NSException NSAutoreleasePool` | `src/foundation/` (3,353 lines) |
| CoreFoundation | 62 symbols: CFRetain/Release, CFString/CFArray/CFDictionary/CFNumber, CFRunLoop (one mode), sources and timers; objects *are* the Foundation objects | `src/corefoundation/` |
| Blocks / libdispatch | Blocks ABI subset; main/serial/global queues, groups, semaphores, once, after, apply; concurrent queues run serially; **no sources, no dispatch_data/io** | `test_blocks`, `test_dispatch`, `src/dispatch/dispatch.cpp` (583 lines) |
| CoreGraphics | **0.6.1/0.6.2 done for fixtures**: geometry (0.5) + ~100 drawing functions, contexts/colors/paths/images (handles only), affine transforms, Direct2D device, real pixels | `test_cg`, `test_cg_abi`, `test_cg_rendering`, `test_gui_win32`, `fuzz_cg`; [COREGRAPHICS-RENDERING.md](../COREGRAPHICS-RENDERING.md) |
| AppKit | NSApplication, NSWindow, NSView (incl. `-drawRect:`, `setNeedsDisplay:`, `display`), NSControl, NSButton, NSTextField, NSEvent, NSScreen, NSGraphicsContext on Win32 HWNDs or an offscreen backend; **no menus, text system, images, panels, nibs, Auto Layout, NSColor/NSBezierPath** | `src/appkit/` (1,831 lines), `test_appkit`, [APPKIT.md](../APPKIT.md), [APPKIT-DRAWING.md](../APPKIT-DRAWING.md) |
| Swift | **Not supported.** All four real applications analysed are Swift; they are refused when their first Swift library is missing | `orvyn run CotEditor.app` (section 5) |
| Worker / security | The worker is a Job Object-restricted process, **not a sandbox**: guest code runs with the user's rights | [LIMITATIONS.md](../LIMITATIONS.md), `src/process/` |

## 4. Discrepancies found while auditing (documentation or text that does not match the code)

| # | Finding | Evidence | Status |
|---|---|---|---|
| 1 | `orvyn doctor` says `loader / dyld / darwin runtime: missing — not implemented in 0.1: 'orvyn run' unavailable`, but `orvyn run` works | `src/cli/commands.cpp:559`; a test asserts the stale text (`tests/integration/test_cli.cpp:212`) | open; trivial fix together with the test |
| 2 | `orvyn --help` prints the banner `... 0.2.0` while `--version` prints `0.5.0` | `src/cli/main.cpp:19` | open |
| 3 | `docs/LOADER.md` still describes the 0.2 validator ("no chained fixups, no weak-binding info, no TLS", "only libSystem") although chained fixups, TLS, `@rpath` and dependent dylibs exist since 0.3 | `docs/LOADER.md` vs `src/dyld/chained_fixups.cpp` | open: doc is historical, [DYNAMIC-LINKING.md](../DYNAMIC-LINKING.md) is the current one |
| 4 | Golden snapshots under `tests/fixtures/*/golden` carry the absolute path of the repository folder on their first line (340 files) | `git grep -l` | open, harmless for the tests; see [CIDER-TO-ORVYN-MIGRATION.md](CIDER-TO-ORVYN-MIGRATION.md) |
| 5 | A fuzzer in this milestone found a real hang (`Path::arc` with a `1e300` sweep) | `0bce9f8` | fixed |

## 5. Real applications today (re-measured with the current build)

`python -I tools/app_coverage.py utilities/extracted/coteditor/CotEditor.app --orvyn orvyn.exe --objdump llvm-objdump.exe` → **CotEditor: 4,371 imported symbols, 105 recognised as implemented**
(a count of names, *not* compatibility), 26 Swift libraries linked. `orvyn run CotEditor.app` → worker starts, the main Mach-O parses (5 segments, 71 load commands), Sparkle loads, then the link stops with
**"Missing library"**: first `/usr/lib/libcompression.dylib`, `IOKit`, `Security`, `ServiceManagement` (Sparkle), then `WebKit`, `AVFoundation`, `SwiftUI`, `Combine`, `/usr/lib/libc++.1.dylib` and
`/usr/lib/swift/libswiftCore.dylib` (the full list is long). No guest code ran. The other three apps were analysed in the previous phase (reports under `utilities/reports/`, not tracked); their bundles are not present on this
machine any more, so they were **not** re-run: see [REAL-APP-COMPATIBILITY.md](REAL-APP-COMPATIBILITY.md).

## 6. What is not known

* Whether the three opt-in tests still pass today: they were not enabled in this run.
* Behaviour on other Windows versions, on ARM64 Windows, with several monitors or with different DPI per monitor.
* Behaviour of any Apple-built binary other than the four Swift applications (none has been executed).
