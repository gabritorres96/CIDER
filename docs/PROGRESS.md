# Progress

**Current phase: CIDER 0.4.0 — Objective-C runtime and Foundation subset. Implemented and verified for controlled fixtures only.
No macOS application runs; Cocoa/AppKit/SwiftUI do not exist; Foundation is a host-written subset, not Apple's.**
Last verified: 2026-10-10, Windows 11 Pro 10.0.26200, MSVC 14.51 + ml64, clang-cl/LLVM 23.1.3 (portable), CMake 4.2.3.

## What now works (0.4, on top of 0.3)

| Capability | Evidence |
| --- | --- |
| Host→guest callbacks: `qsort` with a Mach-O comparator, `bsearch`, `atexit`, nested callbacks, invalid targets refused | `test_callbacks` (18) |
| `FILE` streams (`fopen` … `fclose`), byte-exact, opaque `FILE*`, `--root` | `test_streams` (6) |
| pthreads (cooperative): create/join, mutex, condvar, once, keys, per-thread TLS/errno; host TEB/gs untouched | `test_threads` (13) |
| Objective-C objc2 runtime: classes, metaclasses, categories, protocols, `+load`, ivar sliding, dispatch (all ABI return classes), run-time classes, MRC + ARC + weak + pools, traps | `test_objc` (16); parser checked against `llvm-objdump --objc-meta-data` |
| Foundation subset + the reference program: `@"CIDER"`, `@[@20,@22]`, `NSLog(@"%@ result: %ld")` → stderr `CIDER result: 42`, `Guest exit code: 42`, 0 live objects | `test_foundation` (7), `test_cli`; patching `CIDER` in the file changes the output to `SIDER result: 42` |

## Verification evidence (real runs, 2026-10-10)

| Check | Result |
| --- | --- |
| Debug build + `ctest` | 27/27 suites pass |
| Release build + `ctest` | 27/27 suites pass |
| Test count (Release, per executable) | **462 run, 0 failed, 3 skipped** (0.3: 401 run, 2 skipped) |
| Fresh build from scratch (new directory, Release) | configure + build rc 0; ctest 27/27 pass |
| AddressSanitizer (clang-cl 23.1.3, all 27 suites incl. Objective-C/Foundation/threads/callbacks) | 0 errors, 27/27 pass (one memory-growth test needed a wider limit under ASan: quarantine) |
| libFuzzer, 150 s each, seeds = all `tests/fixtures/objc/bin` + `dylib/bin` files | `fuzz_objc` (new) 27,766 runs; `fuzz_macho` 1,093,406; `fuzz_loader` 532,032; `fuzz_linker` 303,376; `fuzz_chained` 625,043 — no crash, leak, timeout or OOM |
| GitHub Actions | **NOT validated**: never ran on GitHub |
| Real macOS apps | Rectangle, Stats, MonitorControl still refused (not re-run in 0.4) |

## Commits (local, no remote)

`9ae9e64` 0.4.0 callbacks · `393f563` 0.4.1 FILE streams · `1e6789b` 0.4.2 threads/TLS · `3b7c863` 0.4.3/0.4.4 Objective-C runtime ·
`35a5479` 0.4.5 memory management · `15933fa` 0.4.6 Foundation subset · plus the validation/documentation/version commit that follows.

## Problems found while building 0.4 (all fixed, covered by tests)
1. ld64.lld emits rebase/bind ULEB deltas that wrap backwards: the relocation engine accepts wrapped advances.
2. `+load` of a category called a method from the category for the class → capture the class' own `+load` before attaching categories.
3. `class_getInstanceSize` must be word-aligned; ARC fixtures need `-fno-objc-exceptions`; `class_createInstance` result needs `ns_returns_retained` in the hand-written header.
4. `object_dispose` must run `.cxx_destruct` (ARC ivars).
5. `exit()` in an atexit handler / thread join during exit lost the exit code.
6. Compiler `@protocol(X)` handles were unknown to `conformsToProtocol:`; non-ASCII `@"…"` literals are UTF-16; `@[]`/`@{}` need `___NSArray0__`/`___NSDictionary0__`.
7. Flaky `test_tls` stack-limit comparison; ASan quarantine vs. the 24 MiB growth limit.

## Known gaps
CI never ran on GitHub; one machine; x86_64 only; threads are cooperative (no parallelism, no race detection); no signals/`dlopen`/`fork`/`mmap`; no blocks, exceptions, Swift; Foundation covers 9 classes only;
`NSLog` has no timestamp prefix; lazy binds applied eagerly; worker is not a sandbox; fuzzing sessions are minutes long; no Cocoa. See [LIMITATIONS.md](LIMITATIONS.md).

## Next phase (recommended, not started)
0.5: blocks + a minimal libdispatch (queues on the cooperative scheduler), `NSString` mutability/`NSData`/`NSDate`/`NSFileManager`, `forwardInvocation:`/`resolveInstanceMethod:`, exceptions (`@try`), CoreFoundation basics,
then the first AppKit skeleton on a Win32 backend.
