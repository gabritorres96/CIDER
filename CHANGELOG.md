# Changelog

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
