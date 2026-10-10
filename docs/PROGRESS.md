# Progress

**Current phase: CIDER 0.3.0 — Darwin runtime and dynamic library support. Implemented and verified for controlled fixtures only.
No macOS application runs; Cocoa/SwiftUI/Foundation do not exist.**
Last verified: 2026-10-10, Windows 11 Pro 10.0.26200, MSVC 14.51 + ml64, clang-cl/LLVM 23.1.3 (portable), CMake 4.2.3.

## What now works (0.3)

| Capability | Evidence |
| --- | --- |
| Executable + one real Mach-O dylib: `cider run tests/fixtures/dylib/bin/one/one` → `Guest exit code: 42`, the 42 computed inside `libmath.dylib` | `test_linker`, `test_cli`; patching the dylib's `lea` changes the result |
| Two dylibs, transitive A→B, shared dependency loaded once, exported data symbols both ways, `@rpath`, `@loader_path`, `@executable_path`, re-exports, weak missing library, cycles, missing library (exit 12), bad compat version | `test_linker` (37) |
| Mach-O initializers (`__mod_init_func`, `__init_offsets`): dependencies first, once, executable sees the global changed by the dylib's initializer; `exit()` inside an initializer; crashing initializer contained | `test_linker`, `test_cli` |
| ABI: mixed int/double, 8 integers, 10 doubles, stack arguments, variadics (AL), rax:rdx / xmm0:xmm1 returns, callee-saved registers, red zone | `test_abi_raw` (14), hand-assembled code |
| `printf("Number: %d\n", 42)`, `printf("Float: %.2f\n", 3.14)`, `printf("Text: %s\n", "CIDER")`, mixed, stack args, `va_list` built by clang → `vsnprintf` | `test_darwin_runtime`, byte-exact output |
| stdout regression fixed (hello prints "Hello from macOS!" on a real console) | ConPTY test fails without the fix |
| Files (`open/read/write/close/lseek/stat/fstat/access/getcwd`), time (`gettimeofday`, `clock_gettime`, `time`, `nanosleep`), ids, Darwin errno, guest root | `test_darwin_runtime` (27) |
| Chained fixups `DYLD_CHAINED_PTR_64` / `_64_OFFSET` (exe + dylib), matching `llvm-objdump --dyld-info`; other formats refused by name | `test_chained_fixups` (14) |
| TLS (Mach-O TLV protocol, no `gs`/TEB access; host TEB/gs/TlsAlloc/thread_local verified unchanged) | `test_tls` (11) |

## Verification evidence (real runs, 2026-10-10)

| Check | Result |
| --- | --- |
| Debug build + `ctest` | 22/22 suites pass |
| Release build + `ctest` | 22/22 suites pass |
| Test count (Release, per-executable, optional real-DMG/app inputs not set) | **401 run, 0 failed, 2 skipped** (0.2: 294) |
| Fresh build from scratch (new build directory, Release) | configure + build (Release, empty directory) rc 0; ctest 22/22 pass |
| AddressSanitizer (clang-cl 23.1.3, every suite incl. guest execution, linker and TLS) | 0 errors, 0 failed tests |
| libFuzzer `fuzz_chained` (export trie + chained/classic fixup decoders), 180 s | 1,911,347 runs, no crash/leak/timeout |
| libFuzzer `fuzz_linker` (whole multi-image link, never executes the guest), 180 s | 366,991 runs, no crash/leak/timeout |
| Earlier fuzzers (`fuzz_macho`, `fuzz_loader`, …) | not re-run in 0.3 (code unchanged except the validator/fixup entry points) |
| GitHub Actions | **NOT validated**: never ran on GitHub |
| Real macOS apps | Rectangle, Stats, MonitorControl still refused (Foundation/AppKit/libobjc missing) — not re-run in 0.3 |

## Commits (local, no remote)

`5780375` stdout fix · `fb627de` dynamic linker + initializers · `cf175d2` ABI/printf/stdio/POSIX · `6cd9722` chained fixups ·
`a310d26` TLS · plus the documentation/version commit that follows.

## Problems found while building 0.3 (all fixed, covered by tests)
1. `CREATE_NO_WINDOW` hid the guest's console output (0.2 regression, see above).
2. GlobalOpt evaluated a static constructor at compile time at `-O1`, removing `__mod_init_func`: initializer fixtures use `-O0`.
3. Recursive export-trie walking overflowed the native stack on a 600-deep trie: the walker is iterative.
4. Mutated install names with invalid UTF-8 made `std::filesystem` throw: install names/rpaths are validated before use; `link()` also catches exceptions.
5. Darwin TLV calls require every register except `rax` preserved; the generic Windows thunk clobbered `edx` and the guest read wrong values → dedicated register-preserving thunk.
6. Test-code bug: `/J` was rewritten to `\J` by a path-separator replace (junction test silently skipped).

## Known gaps
CI never ran on GitHub; one machine; x86_64 only; one guest thread; no signals/`atexit`/`dlopen`/`fork`/`mmap`; `FILE` streams limited to
stdin/stdout/stderr; no callbacks host→guest (`qsort`); lazy binds applied eagerly; worker is not a sandbox; fuzzing sessions are minutes long;
no Cocoa/ObjC/Swift. See [LIMITATIONS.md](LIMITATIONS.md).

## Next phase (recommended, not started)
0.4: Objective-C runtime evaluation (libobjc2), `atexit`/destructors and host→guest callbacks (`qsort`, signal handlers), `FILE` streams
and `readdir`, threads (`pthread` over Win32) and per-thread TLS blocks, shared-cache-less frameworks loader, first Foundation subset.
