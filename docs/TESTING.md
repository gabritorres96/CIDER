# Testing

```bash
ctest --test-dir build -C Debug --output-on-failure
```

No third-party test framework: `tests/unit/test_framework.hpp` is a ~70-line harness
(`CIDER_TEST`, `CHECK*`, `REQUIRE*`, `CHECK_ERR`, `SKIP`). A test executable accepts an optional
substring filter: `build/bin/Debug/test_dmg.exe extract_`. The integration suite needs
`CIDER_EXE` (set automatically by CTest).

## Suites (166 tests with all optional inputs; 164 run + 3 skipped without them)

| Executable | Tests | Covers |
| --- | --- | --- |
| `test_util` | 9 | checked arithmetic, `ByteView`, strings, UTF-8 sanitising, JSON writer, logger, file limits |
| `test_macho` | 20 | headers, segments/sections, entry points, dylibs, rpaths, symbols, code signature, encryption flag |
| `test_malformed` | 33 | rejection matrix: truncation, offsets, overflow, overlaps, unterminated strings, limits; fuzz regression |
| `test_fat` | 10 | FAT/FAT64, slice isolation, overlap/out-of-bounds, Java class look-alike |
| `test_plist` | 11 | XML + binary plists, entities, limits, corruption, cycles |
| `test_bundle` | 15 | `.app` metadata, traversal rejection, frameworks, dyld path expansion, dependency closure, compatibility policy |
| `test_mutation_fuzz` | 4 | 20 000 random mutations each of Mach-O/FAT and plists, garbage input, regression |
| `test_dmg` | 36 | UDIF structure and corruption, encrypted/sparse/raw detection, path/listing validation, process runner, 7-Zip extraction (limits, cleanup, traversal, long paths, links, hdiutil-like layout), optional real DMGs |
| `test_real_macho` | 13 | LLVM-built fixtures vs llvm-objdump goldens, live llvm-objdump, optional real-world apps |
| `test_cli` | 15 | the real `cider.exe`: all commands, exit codes, JSON validity, DMG end-to-end, real fixtures |

Skipped (printed as `[SKIP]`, exit code 0) when the input is absent: 7-Zip tests without 7-Zip, the symlink
test without symlink privilege, and three that need explicit inputs:

| Variable | Enables |
| --- | --- |
| `CIDER_LLVM_BIN` | live llvm-objdump comparison (`test_real_macho live_*`, `real_world_*`) |
| `CIDER_REAL_APPS_DIR` | compare every Mach-O in an extracted app tree with llvm-objdump |
| `CIDER_REAL_DMG_DIR` | parse + extract genuine `.dmg` files (`test_dmg real_world_*`) |

`tools/validate-real-dmgs.ps1` downloads three official DMGs and sets these up; the test-suite itself never
downloads anything.

## Fixtures — what they are and are not

* **Real (since 0.1.1)**: `tests/fixtures/real` — Mach-O built by LLVM 23.1.3 (`tools/gen-real-fixtures.cmd`), against a
  hand-written libSystem stub, with `llvm-objdump` snapshots as goldens. Committed so CI needs no LLVM.
  They are real linker output (ld64.lld), **not** Apple's ld64/Xcode output.
* **Synthetic**: `tests/fixtures/macho_builder.hpp` and `dmg_builder.hpp` generate Mach-O/FAT/plist/ISO9660+Joliet/UDIF
  in code for malformed-input tests and DMG logic (raw or zlib-"stored" chunks; `real_layout` mimics the
  MBR/GPT/free partitions of hdiutil images). No HFS+/APFS writer exists.
* `tests/fixtures/regression`: inputs that once crashed CIDER (libFuzzer finding).
* Embedded "code" is never executed.

## Sanitizers and fuzzing (clang-cl + portable LLVM)

```bat
tools\llvm-env.cmd cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER_TYPE=LLD -DCIDER_SANITIZE=ON
tools\llvm-env.cmd cmake --build build-asan
:: run with clang_rt.asan_dynamic-x86_64.dll on PATH (<LLVM>\lib\clang\23\lib\windows)
```

ASan notes: clang-cl ASan needs the release DLL CRT (`/MD`, set by CMake) and `_DISABLE_STL_ANNOTATION`
(the MSVC STL's `stl_asan.lib` ships with the VS ASan component, which is not installed here). The
prebuilt `clang_rt.fuzzer` is `/MT` and cannot link with `/MD` ASan code, so libFuzzer is built from its
sources: `-DCIDER_BUILD_FUZZERS=ON -DCIDER_LIBFUZZER_SRC=<llvm-project>/compiler-rt/lib/fuzzer`
(see `tests/fuzz/CMakeLists.txt`). With fuzzers enabled the library code is also coverage-instrumented
(without it libFuzzer is blind). Run, e.g.:

```bat
build-fuzz\bin\fuzz_macho.exe corpus_dir -max_total_time=300 -max_len=65536
```

Results of the 0.1.1 campaign: [REAL-WORLD.md](REAL-WORLD.md) / [PROGRESS.md](PROGRESS.md).

## Reproducing a failure

1. Run the failing executable directly with the test name as filter.
2. `tools/make_dmg_fixture.exe <dir>` writes sample images and fuzz seeds.
3. Add the minimal reproducer as a regression test before fixing.

## Not done

* MSVC's own `/fsanitize=address` (needs the VS component; clang-cl ASan was used instead).
* GitHub Actions has never run this workflow.
* Long fuzzing campaigns (sessions were minutes, not days).
