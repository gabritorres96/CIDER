# Changelog

## 0.5.0 — AppKit, Native Windows GUI & Apple SDK Research (experimental, 2026-10-11)

Controlled fixtures only; no macOS application runs; no Cocoa; no Swift.

### Added
- **Apple SDK research**: `cider sdk-scan` (TAPI `.tbd` reader `cider::sdk::parse_tbd`, fuzzed by `fuzz_tbd`) compares SDK stubs with what CIDER implements; `tools/pkg_payload.py` and `tools/xip_inventory.py` open pbzx payloads and `.xip` files
  read-only; `tools/app_coverage.py` measures an application against CIDER. Reports: [APPLE-SDK-RESEARCH.md](docs/APPLE-SDK-RESEARCH.md), [COTEDITOR-COMPATIBILITY.md](docs/COTEDITOR-COMPATIBILITY.md). `utilities/` is git-ignored.
- **0.5.1 Blocks**: `_Block_copy/_release`, `_Block_object_assign/_dispose`, `__block` variables, `NSBlock` classes, `objc_retainBlock`, block-based Foundation enumeration. [BLOCKS.md](docs/BLOCKS.md).
- **0.5.2 libdispatch**: main/serial/global queues on host-implemented guest threads, `dispatch_async/sync/after/once/apply/main`, groups, semaphores; main-queue blocks run only on the main thread. [LIBDISPATCH.md](docs/LIBDISPATCH.md).
- **0.5.3 Foundation**: NSMutableString, NSData, NSMutableData, NSDate, NSURL, NSError, NSFileManager (confined to `--root`), NSNotification(Center), NSString path helpers. [FOUNDATION.md](docs/FOUNDATION.md).
- **0.5.4 Forwarding and exceptions**: `+resolveInstanceMethod:`, `-forwardingTargetForSelector:`, `-forwardInvocation:` with NSMethodSignature/NSInvocation; NSException and `objc_exception_throw` for *uncaught* exceptions.
  `@try/@catch` are **not supported**; such binaries are refused at load time.
- **0.5.5 CoreFoundation basics**: CFRetain/Release, CFString/CFArray/CFDictionary/CFNumber on the Foundation objects, CFRunLoop sources and timers; one shared run loop ([EVENT-LOOP.md](docs/EVENT-LOOP.md)).
- **0.5.6 CoreGraphics geometry**: independent geometry module, CGRect/CGPoint/CGSize functions with System V struct passing, AppKit↔Win32 coordinate and DPI conversion, autoresizing. [COREGRAPHICS.md](docs/COREGRAPHICS.md).
- **0.5.7-0.5.10 AppKit and Win32**: NSApplication, NSWindow, NSView, NSControl, NSButton, NSTextField, NSEvent, NSScreen; GUI backend interface with a Win32 backend (real HWNDs, BUTTON/STATIC/EDIT controls, per-monitor DPI,
  WM_COMMAND/WM_CLOSE/WM_SIZE/WM_DPICHANGED) and an offscreen backend; target/action from a real button click into Objective-C code compiled in the Mach-O; `hello_window`. [APPKIT.md](docs/APPKIT.md), [GUI-BACKEND.md](docs/GUI-BACKEND.md).
- **0.5.11 Tests**: headless AppKit tests, interactive GUI tests that drive the real window with `BM_CLICK`/`WM_CLOSE` from a separate process (`ctest -L gui`), geometry and SDK parser unit tests, new fuzzer `fuzz_tbd`.

### Changed
- Version 0.4.0 → 0.5.0. `cider.exe` has a manifest (visual styles, per-monitor DPI, long paths). Programs that link AppKit run with the default timeout lifted (24 h) unless `--timeout` is given.
- The frameworks re-export each other when resolving symbols (Foundation, CoreFoundation, CoreGraphics, AppKit); `libSystem` exports blocks and dispatch symbols.
- `GuestThreads::create_native` and per-thread top-level calls in `Runtime::call_imp`; `Runtime::add_dynamic_provider` replaced the single dynamic-symbol hook.

### Fixed
- Double `Block_release` was silently accepted when the freed memory happened to keep a "global" flag bit (now decided by the image layout).
- NSNotificationCenter no longer retains the sender filter object (it kept whole window trees alive).

### Not done / known limits
Swift, SwiftUI, `@try/@catch`, menus, drawing, NSTextView, images, Auto Layout, multi-monitor, concurrent queue parallelism, libdispatch sources. See [LIMITATIONS.md](docs/LIMITATIONS.md).

## 0.4.0 — Objective-C Runtime & Foundation Subset (experimental, 2026-10-10)

Controlled fixtures only (clang + ld64.lld Mach-O programs); no macOS application runs; no Cocoa. Foundation is a host-written subset, not Apple's.

### Added
- **0.4.0 host→guest callbacks**: `abi::call_guest_nested`, nested `GuestCall` records below the red zone, depth limit 64, validated targets (inside a loaded image's code),
  `exit()` unwinding; `qsort` (stable merge sort), `bsearch`, `atexit` (reverse order, `exit()` in a handler). [CALLBACKS.md](docs/CALLBACKS.md).
- **0.4.1 FILE streams**: `fopen fclose fread fwrite fseek fseeko ftell ftello rewind feof ferror clearerr fflush fgets fgetc getc ungetc fputs fputc fileno setvbuf`; opaque 64-slot pool,
  binary mode, confined to `--root`, closed at exit.
- **0.4.2 threads**: `pthread_create/join/detach/self/equal/exit`, attributes, mutexes (normal/errorcheck/recursive), condition variables, `pthread_once`, keys, `sched_yield`; real Windows threads
  under a fair cooperative scheduler; per-thread `errno`, TLS blocks and autorelease pools; `GS`/TEB never touched. [THREADING.md](docs/THREADING.md).
- **0.4.3 research** [OBJECTIVE-C-RUNTIME.md](docs/OBJECTIVE-C-RUNTIME.md): GNUstep libobjc2 evaluated and rejected (MIT, but not Apple-ABI-equivalent; GNUstep-base is LGPL); own runtime.
- **0.4.4 Objective-C runtime** (`src/objc/`): objc2 metadata (classes, metaclasses, categories, protocols, ivars, selector refs), non-fragile ivar sliding, `objc_msgSend` family as MASM tail-jump stubs
  (`_stret`, `_fpret`, `Super`, `Super2`), nil messaging, unrecognized-selector traps, `+load`, run-time class construction, 80 libobjc exports, `Category::Objc` log lines.
- **0.4.5 memory**: retain/release/autorelease, per-thread pools, `-dealloc` + `.cxx_destruct`, weak references, ARC helpers (`objc_alloc*`, `retainAutoreleasedReturnValue`, `storeStrong`, …), double-release/invalid-token
  diagnoses, leak counter `objc_live_objects`.
- **0.4.6 Foundation subset** (`src/foundation/`): NSObject, NSString/NSConstantString (UTF-16 lengths), NSNumber, NSArray/NSMutableArray, NSDictionary/NSMutableDictionary, NSAutoreleasePool, `NSLog`
  (`%@`), `NSStringFromClass` & co.; literals `@"…" @42 @[…] @{…}`, subscripts, fast enumeration. [FOUNDATION.md](docs/FOUNDATION.md).
- Reference program: `foundation_hello` prints `CIDER result: 42` and exits 42. Fixtures: 15 Objective-C program directories (14 programs + the `slide` dylib scenario), 17 new exec fixtures; suites `test_callbacks`, `test_streams`, `test_threads`, `test_objc`,
  `test_foundation`; fuzzer `fuzz_objc`.
- `printf_format` `%@` hook (Foundation only), `LinkOptions::before_protect`, `LibSystemProvider::set_dynamic_data`, per-library symbol groups (libSystem / libobjc / Foundation).

### Fixed
- Wrapped ULEB backward moves in rebase/bind streams emitted by ld64.lld ("offset overflows").
- `@protocol(X)` handles produced by the compiler were not recognised by `conformsToProtocol:`.
- `exit()` inside an `atexit` handler or a joined thread lost its exit code; `pthread_join` returned normally while the process was exiting.
- Test flakes: `test_tls` stack-limit comparison; `repeated_runs_do_not_leak_memory_or_address_space` under ASan (quarantine).

### Changed
- Version 0.3.0 → 0.4.0. `GuestCall` is 200 bytes. A program importing `libobjc`/Foundation is no longer refused up front; unknown symbols still fail the link (exit 12).

### Not done / known limits
Blocks, exceptions, Swift, AppKit/Cocoa, run loops, `NSData/NSDate/NSURL/NSMutableString`, message forwarding, relative method lists, parallel threads (cooperative only), race detection. See [LIMITATIONS.md](docs/LIMITATIONS.md).


## 0.3.0 — Darwin Runtime & Dynamic Library Support (experimental, 2026-10-10)

Controlled fixtures only; no macOS application runs.

### Fixed
- **Guest stdout lost on a real console** (`cider run hello` printed only the CIDER log lines): the worker was started with
  `CREATE_NO_WINDOW`, which gives it a fresh hidden console and invalidates inherited console handles. Pipe-based tests could not
  see it; a ConPTY test now guards it (it fails without the fix).

### Added
- **Dynamic linker** (`cider::linker::Program`): `MH_DYLIB`, `LC_LOAD_DYLIB/WEAK/REEXPORT`, `LC_RPATH`, `@executable_path`,
  `@loader_path`, `@rpath` (per-owner expansion), transitive dependencies, each file loaded once, cycles detected (tolerated or
  refused), compatibility-version check, export-trie parser, per-(ordinal, symbol) binding, re-exports, unload. System libraries and
  Windows DLLs are never loaded; Windows-style install names are refused.
- **Mach-O initializers** `__mod_init_func` / `__init_offsets`, dependencies first, once, after all fixups and W^X protections.
- **ABI**: raw thunk (`GuestRegs`) for mixed int/double, >6 integer arguments, stack arguments, variadics (AL), aggregate returns;
  `RegsReader` / `VaListReader`; TLV thunk preserving all registers.
- **libSystem**: own printf formatter and the printf family, stdio subset, Darwin descriptor table and `open/read/write/close/lseek/stat/fstat/access/getcwd`,
  `gettimeofday/clock_gettime/time/nanosleep/usleep/sleep`, `getppid/_exit`, `___error`, strings, math, `div/ldiv`, `ldexp/frexp`.
- `cider run --root <dir>` (guest filesystem root), `--verbose` shows `[CIDER][DYLD]` loading/resolving/binding/initializer steps;
  JSON gains `images`, `missing_library`, `initializers_run`.
- **Chained fixups** (`DYLD_CHAINED_PTR_64`, `DYLD_CHAINED_PTR_64_OFFSET`; imports formats 1-3), checked against `llvm-objdump --dyld-info`.
- **TLS** subset: TLV descriptors served by CIDER, no `gs`/TEB access.
- Fixtures: 14 exec programs + 19 dylib/chained/TLS scenarios; suites `test_linker`, `test_abi_raw`, `test_darwin_runtime`,
  `test_chained_fixups`, `test_tls`; fuzzers `fuzz_chained`, `fuzz_linker`. Docs: DYNAMIC-LINKING, DARWIN-RUNTIME, TLS.

### Changed
- Version 0.2.0 → 0.3.0. Unsupported-feature checks moved: chained fixups and TLS are no longer refused up front (the 0.2 tests that
  asserted refusal, and the `printf`/`open`/`time` "not implemented" assertions, were updated); the `0x15` thread-local init sections are still refused.
- `cider run` pre-validation delegates dependencies to the linker (missing libraries still exit 12).

## 0.2.0 — Native Mach-O Execution Runtime (experimental, 2026-10-10)

CIDER executes machine code from Mach-O x86_64 files on Windows. **Controlled fixtures only; no macOS application runs.**

### Added
- `cider run <program|App.app> [-- args] [--json] [--verbose] [--timeout N]` (previously "not available"). Dedicated exit
  codes: 0 guest completed, 10 internal, 11 incompatible file, 12 dependency/symbol not available, 13 load failure,
  14 guest crash/trap/timeout. JSON output separates `runtime` status from the `guest` result.
- Runtime (`cider_runtime`, Windows x64): memory layer (VirtualAlloc/Protect/Query, W^X policy), pre-execution validator,
  layout planner, segment mapper, fixups (rebase / bind / lazy-bind, applied eagerly), symbol resolver, System V → Windows
  x64 ABI bridge in MASM (`entry_trampoline.asm`), guest stack + argv/envp/apple builder, libSystem subset (24 functions + a
  trapping `dyld_stub_binder`), private guest heap, in-process runner, worker process (`cider __worker`, internal), supervisor
  with Job Object limits, result protocol.
- Log categories `[CIDER][LOADER|MEMORY|ABI|DYLD|SYMBOLS|LIBSYSTEM|EXECUTION|PROCESS]`.
- 20 execution fixtures (`tests/fixtures/exec`, built by `tools/gen-exec-fixtures.cmd` with clang + ld64.lld) and their
  llvm-objdump snapshots; 8 new test suites (`test_memory`, `test_abi`, `test_loader`, `test_entry`, `test_symbols`,
  `test_runtime`, `test_execution`, `test_execution_errors`); `fuzz_loader`; docs/EXECUTION.md.
- Tests: 294 (166 from 0.1.1 + 128), all passing; ASan clean.

### Changed
- Version 0.1.1 → 0.2.0. `process.hpp`: `run_process_ex` (inherit/separate stdio, job limits) and `current_executable()`.
- The 0.1.1 test asserting that `cider run` is unavailable was replaced by real `run` tests (intended behaviour change).
- CMake: ASM_MASM enabled on Windows x64; C++ flags restricted to C++ sources.

### Fixed during development
- Guest `malloc` under ASan aborted the worker on large requests: guest allocations now use a private Win32 heap.
- 64-bit return values of host functions (`write` → `-1`) were truncated to 32 bits.

### Not done / limits
See docs/LIMITATIONS.md and docs/EXECUTION.md: no dependent dylibs/frameworks, Objective-C, Foundation, TLS, threads, signals,
printf/stdio, chained fixups, arm64; the worker is not a sandbox; CI not validated on GitHub.

## 0.1.1 — Real World Validation (2026-10-10)

Validation release: no new runtime features (CIDER still cannot run macOS programs).

### Validated
- Mach-O: 9 real fixtures built with LLVM 23.1.3 (`clang` + `ld64.lld` + `llvm-lipo` from Windows) agree field by
  field with `llvm-objdump`; 63 real Mach-O files / 93 slices extracted from three official open-source DMGs
  (Rectangle 2.0.3, Stats 3.0.20, MonitorControl 4.4.0) agree with live `llvm-objdump`.
- DMG: three genuine hdiutil UDIF images parsed (8 partitions, zlib/raw/ignore chunks, HFS+ partition) and
  extracted with 7-Zip 25.01; file trees are byte-identical (SHA-256) to a direct `7z x`; Info.plist metadata
  equals Python `plistlib`.
- AddressSanitizer: whole suite (166 tests, incl. the real-world ones) clean with clang-cl ASan (portable LLVM).
- libFuzzer: `fuzz_macho`, `fuzz_plist`, `fuzz_udif` built (libFuzzer from source, /MD) and run with coverage
  guidance: 1.77 M / 2.40 M / 0.21 M executions in 5 minutes each, no findings after the fix below.
  (An earlier, blind run found the crash below.)
- Details, files, hashes and caveats: [docs/REAL-WORLD.md](docs/REAL-WORLD.md).

### Fixed
- **Crash** (found by libFuzzer): `LC_SYMTAB` with `strsize == 0` and a wild `stroff` made symbol parsing throw.
- DMG: hdiutil metadata partitions (MBR/GPT/free) were extracted as filesystems and failed.
- DMG: symlinks from HFS+ images aborted extraction; they are now removed (never followed) and reported.
- DMG: paths longer than MAX_PATH failed; extended-length paths are used for CIDER's own file operations.
- `path_from_utf8` returns native separators (required for extended-length paths).
- Symbols: `N_UNDF` without `N_EXT` is undefined (matches llvm-objdump/nm).
- `cider inspect` on disk images: concise output (transitive duplicates summarised, paths relative to the app,
  link removals as one warning).

### Added
- `tools/gen-real-fixtures.cmd`, `tools/llvm-env.cmd`, `tools/validate-real-dmgs.ps1`.
- `tests/fixtures/real` (LLVM-built binaries, hand-written libSystem stub, llvm-objdump goldens),
  `tests/fixtures/regression` (fuzz crash input), `test_real_macho`, optional real-world tests
  (`CIDER_REAL_DMG_DIR`, `CIDER_REAL_APPS_DIR`, `CIDER_LLVM_BIN`).
- `fuzz_udif`; fuzz/ASan support for clang-cl (`CIDER_SANITIZE`, `CIDER_BUILD_FUZZERS`, `CIDER_LIBFUZZER_SRC`).
- `dmg::verify_extracted_tree`, `extended_path`, `ExtractReport::links_removed`, test-builder option `real_layout`.
- Test harness: enum printing in `CHECK_EQ`.

### Changed
- Version 0.1.0 → 0.1.1. CI workflow gained an experimental ASan job (**not run on GitHub yet**).

## 0.1.0 — Inspector (2026-10-10)

First release: Mach-O/FAT/plist/.app inspector, CLI, logging, DMG (UDIF + optional 7-Zip), 144 tests.
