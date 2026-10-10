# Progress

**Current phase: CIDER 0.1.1 (Real World Validation) — complete. CIDER 0.2 has NOT been started.**
Last verified: 2026-10-10, Windows 11 Pro 10.0.26200, MSVC 14.51, CMake 4.2.3, 7-Zip 25.01, LLVM 23.1.3 (portable).

## State of the code

Everything from 0.1 (inspector, CLI, Mach-O/FAT/plist/.app, DMG via optional 7-Zip) plus the fixes listed in
[CHANGELOG.md](../CHANGELOG.md). Still a **static inspector: it cannot run macOS programs**.

## Verification evidence (this machine)

| Check | Result |
| --- | --- |
| Debug and Release build (MSVC /W4) | no errors, no warnings |
| `ctest` Debug and Release, no optional env | 10/10 suites pass; 164 tests run, 3 skipped (the optional real-world / live-objdump tests) |
| Same with `CIDER_LLVM_BIN`, `CIDER_REAL_DMG_DIR`, `CIDER_REAL_APPS_DIR` set | **166 tests, 0 failed, 0 skipped** (the 144 tests of 0.1 all still pass, +22 new) |
| AddressSanitizer (clang-cl 23.1.3, `CIDER_SANITIZE`) | all 166 tests clean, including extraction of the 3 real DMGs |
| libFuzzer (`fuzz_macho/plist/udif`, coverage-guided, 5 min each) | 1.77 M / 2.40 M / 0.21 M execs, no crash, no leak, no timeout |
| Real Mach-O vs llvm-objdump | 9 LLVM-built fixtures + 63 files / 93 slices from 3 real apps: all agree |
| Real DMGs | 3 genuine UDIF/HFS+ images analysed + extracted; trees identical to direct 7-Zip extraction; plist metadata equals Python plistlib |
| GitHub Actions | **NOT validated**: the workflow was reviewed and extended but has never run on GitHub |

Details and file hashes: [REAL-WORLD.md](REAL-WORLD.md).

## Acceptance criteria for 0.1.1

1. Real Mach-O executables analysed correctly — **met** (LLVM-built + 63 real files).
2. At least one authentic DMG tested — **met** (3 official releases).
3. Errors found have regression tests — **met** (7 findings, see REAL-WORLD.md §3).
4. Debug and Release build without errors — **met**.
5. All automated tests pass — **met** (see table).
6. Limitations documented — **met** (LIMITATIONS.md, REAL-WORLD.md §3 "Still open").

## Known gaps

1. CI not run on GitHub (see above). The experimental ASan job is unvalidated.
2. **APFS** DMGs untested; HFS+ is only validated *through 7-Zip* (no independent HFS+ decoder was available);
   bzip2/LZFSE/LZMA/ADC chunks and encrypted images were never seen in a real file.
3. The three real DMGs are Intel+Apple-Silicon universal apps from small GUI projects; no Apple-made DMG,
   no installer package, no kernel extension, no app using Objective-C metadata checks was analysed beyond
   the load-command level.
4. Symbolic links inside images are removed (not recreated).
5. Sigstore signature of the LLVM archive was not verified (digest only).
6. Bind/rebase opcodes, chained-fixup contents, export tries, unwind info, Objective-C/Swift metadata are not
   decoded (needed by 0.2's loader).
7. 4–5 minute fuzzing sessions are a smoke test, not a long campaign.

## Next tasks (not started)

1. Review this release. 2. Decode bind/rebase opcodes and chained fixups. 3. CIDER 0.2 (memory mapper, minimal
loader, SysV→Win64 trampolines, first libSystem functions) only after that review.
