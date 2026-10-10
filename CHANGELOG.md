# Changelog

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
