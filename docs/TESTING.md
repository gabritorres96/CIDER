# Testing

```bash
ctest --test-dir build -C Debug --output-on-failure
```

No third-party test framework: `tests/unit/test_framework.hpp` is a ~70-line harness
(`CIDER_TEST`, `CHECK*`, `REQUIRE*`, `CHECK_ERR`, `SKIP`). A test executable accepts an optional
substring filter: `build/bin/Debug/test_dmg.exe extract_`. The integration suite needs
`CIDER_EXE` (set automatically by CTest).

## Suites (0.3: 403 tests; 401 run + 2 skipped in the verified Release run; the 0.2 list below plus the 0.3 suites)

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
| `test_cli` | 24 | the real `cider.exe`: all commands, exit codes, JSON validity, DMG end-to-end, real fixtures, `cider run` (text, JSON, exit codes 0/11/12/14, bundle, timeout) |
| `test_memory` | 10 | VirtualAlloc/Protect/Query wrapper: W^X mapping of protections, range/overflow checks, release on destruction, relocation |
| `test_abi` | 14 | **hand-assembled** System V code: argument registers, alignment, red zone, callee-saved register preservation (MASM self-test), thunks with 2/4/6 integer args and doubles, exit, guard page |
| `test_loader` | 23 | layout planning (RWX, overlap, alignment, overflow, entry, limits), validator, mapping (file bytes in memory, zero-fill, per-page W^X), fixups, cleanup after failure, mutated executables, fixture metadata vs llvm-objdump |
| `test_entry` | 9 | LC_MAIN contract (entry == `_main` from llvm-objdump), argv/envp/apple block, stack limits, `args` fixture |
| `test_symbols` | 13 | rebase/bind/lazy-bind opcode decoding (hand-made and malformed streams), decoded fixups vs `llvm-objdump --rebase --bind --lazy-bind` for 17 binaries, resolver with a fake and the real provider |
| `test_runtime` | 11 | every libSystem function called from guest code through the thunks, registry honesty, guest heap lifetime |
| `test_execution` | 21 | **real execution** of the Mach-O fixtures A–H in-process with independently computed expectations, binary patching (authenticity), relocation/slide, repeated runs without leaks |
| `test_linker` | 37 | export trie (vs llvm-objdump, malformed, limits), dependency search, 14 dylib scenarios (one/two/transitive/init/initoff/glob/rpath/loader_path/reexport/weak/cycle/missing/badver/initexit), initializer order, unload, tampered files, Windows-path install names, 600 mutations |
| `test_abi_raw` | 14 | raw thunk with hand-assembled code: 8 integers, mixed int/double, 10 doubles, stack order, AL, rax:rdx / xmm0:xmm1 returns, alignment, callee-saved registers, red zone, exit; `RegsReader`, `VaListReader` |
| `test_darwin_runtime` | 27 | printf formatter (fake readers, host-rendering cross-check, refusals, limits), byte-exact stdout/stderr of Mach-O fixtures, files (read/write/escape/junction/leaks), time, ids, errno |
| `test_chained_fixups` | 14 | decoding vs `llvm-objdump --dyld-info`, execution, PTR_64_OFFSET variant, high8, relocation by slide, malformed/unsupported matrix, 1500 mutations |
| `test_tls` | 11 | TLV execution (exe + dylib), host TEB/gs/TlsAlloc/thread_local untouched, template layout vs llvm-objdump, register preservation, refusals |
| `test_execution_errors` | 18 | load failures (nothing runs), result protocol, worker process: crash, illegal instruction, stack overflow, timeout, memory limit, separate pid, captured output |
| `test_callbacks` | 18 | host→guest callbacks: integer/double/mixed/8-argument/aggregate results, nesting, depth limit, invalid targets, `exit()` in nested callbacks, red zone; `qsort` (ints, strings, records, nested), `bsearch`, `atexit` order/exit/`_exit`, invalid comparator |
| `test_streams` | 6 | `FILE` streams byte-exact (no text translation), `fread/fseek/ftell/feof/ferror/fgets` contract, `--root` confinement, `EACCES` without a root, finite pool + forged pointers, close at exit |
| `test_threads` | 13 | pthread create/join/exit, mutex exactness and the unprotected-race control, per-thread TLS and errno, condition variables, `exit()` from a thread, thread limit, host thread state untouched, no leaks, deterministic scheduling, opaque handles, fault containment |
| `test_objc` | 16 | metadata parsed vs `llvm-objdump --objc-meta-data`, compiled programs (calc → 42, inheritance, run-time classes, categories/`+load`/protocols, ABI return classes, ivar sliding, MRC, ARC/weak/pools), error traps, hostile-metadata matrix |
| `test_foundation` | 7 | compiled Foundation programs: `CIDER result: 42`, NSString/NSNumber/NSArray/NSDictionary checks inside the guest, guest subclasses of NSObject, ten misuse diagnoses, patching the literal changes the output |
| `test_sdk` | 4 | TAPI `.tbd` reader: v4 documents, multi-line lists, target filtering, limits, 20,000 random mutations |
| `test_geometry` | 4 | rectangle algebra, AppKit↔Win32 coordinates, DPI scaling, child frames, autoresizing |
| `test_blocks` | 3 | Blocks ABI: global/stack/heap blocks, `__block`, captured objects, runtime-invoked blocks, misuse |
| `test_dispatch` | 3 | libdispatch: queues, groups, semaphores, once, after, worker→main sync, repeated runs, misuse |
| `test_foundation_extra` | 3 | NSMutableString/NSData/NSDate/NSURL/NSError/NSFileManager (`--root` confinement), without a root |
| `test_corefoundation` | 1 | CF functions, CGRect passing conventions, run loop timers/sources/main-queue blocks |
| `test_objc_dynamic` | 4 | forwarding (resolve, target, invocation), unsupported struct forwarding, NSException, `@try` refusal |
| `test_appkit` | 3 | AppKit classes on the offscreen backend (target/action, responders, autoresize, close/terminate, leak counters), `hello_window` headless |
| `test_gui_win32` | 3 | **interactive**: the real `cider.exe` opens the real window; a controller process clicks with `BM_CLICK` and closes with `WM_CLOSE`; skipped without a desktop (`ctest -L gui`) |

Skipped (printed as `[SKIP]`, exit code 0) when the input is absent: 7-Zip tests without 7-Zip, the symlink
test without symlink privilege, and three that need explicit inputs:

| Variable | Enables |
| --- | --- |
| `CIDER_LLVM_BIN` | live llvm-objdump comparison (`test_real_macho live_*`, `real_world_*`) |
| `CIDER_REAL_APPS_DIR` | compare every Mach-O in an extracted app tree with llvm-objdump |
| `CIDER_REAL_DMG_DIR` | parse + extract genuine `.dmg` files (`test_dmg real_world_*`) |

`tools/validate-real-dmgs.ps1` downloads three official DMGs and sets these up; the test-suite itself never
downloads anything.

## Execution fixtures (0.2)

`tests/fixtures/exec` — 20 C programs (`src/`) compiled by `clang --target=x86_64-apple-macos10.15` and linked by `ld64.lld` against a
hand-written `libSystem.tbd` (`tools/gen-exec-fixtures.cmd`; binaries, `llvm-objdump` snapshots incl. disassembly and
rebase/bind tables are committed). A: `ret42 ret7 ret99`, B: `math`, C: `stack`, D: `calls`, E: `globals`, F: `bss`,
G: `libcall`, H: `hello`, I: `args`, J: `missing` (imports the unimplemented `_fork`), plus `exitcall`, `malloc`, `fmath`,
`crash`, `spin`, `recurse`, `hog`, `pid` for failure/limit tests. All use the LC_MAIN protocol (the real dyld one); none
needs more than the documented subset. `test_execution` asserts values computed in C++ independently of the guest.

## Fixtures — what they are and are not

* **Real (since 0.1.1)**: `tests/fixtures/real` — Mach-O built by LLVM 23.1.3 (`tools/gen-real-fixtures.cmd`), against a
  hand-written libSystem stub, with `llvm-objdump` snapshots as goldens. Committed so CI needs no LLVM.
  They are real linker output (ld64.lld), **not** Apple's ld64/Xcode output.
* **Synthetic**: `tests/fixtures/macho_builder.hpp` and `dmg_builder.hpp` generate Mach-O/FAT/plist/ISO9660+Joliet/UDIF
  in code for malformed-input tests and DMG logic (raw or zlib-"stored" chunks; `real_layout` mimics the
  MBR/GPT/free partitions of hdiutil images). No HFS+/APFS writer exists.
* `tests/fixtures/regression`: inputs that once crashed CIDER (libFuzzer finding).
* `test_abi` guest code is **hand-assembled machine code** (byte arrays in the test) so the ABI bridge is tested
  independently of any compiler.
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

ASan notes for 0.2: the whole suite (all 294 tests, including real guest execution and the worker crash/limit tests) runs clean
under clang-cl ASan. Guest memory is outside ASan's view by construction (guest code is not instrumented, its heap is a private
Win32 heap, not the ASan allocator). Fuzz harness `fuzz_loader` exercises the loader only (mutated binaries are mapped and
released, **never executed**). Results: [PROGRESS.md](PROGRESS.md), [REAL-WORLD.md](REAL-WORLD.md) (0.1.1 campaign).

## Reproducing a failure

1. Run the failing executable directly with the test name as filter.
2. `tools/make_dmg_fixture.exe <dir>` writes sample images and fuzz seeds.
3. Add the minimal reproducer as a regression test before fixing.

## Not done

* MSVC's own `/fsanitize=address` (needs the VS component; clang-cl ASan was used instead).
* GitHub Actions has never run this workflow.
* Long fuzzing campaigns (sessions were minutes, not days).

## 0.3 fixtures

`tests/fixtures/exec` gained 14 programs (`printf_int printf_double printf_str printf_mixed snprintf stdio_out files_read files_write
open_path time_clock abi_ret unsupported ids` + `darwin.h`, a hand-written mini SDK). `tests/fixtures/dylib` (`tools/gen-dylib-fixtures.cmd`) holds
scenario directories `one two trans init initoff glob missing cycle rpath loaderpath reexport weak initexit initcrash badver chained
chainexe tlsexe tlslib` with `llvm-objdump` snapshots (headers, exports trie, symbols, rebase/bind, `--dyld-info`, chained fixups).
Initializer fixtures are compiled with `-O0` because LLVM's GlobalOpt would otherwise evaluate the constructor at compile time.
`test_cli` runs `cider.exe` under a real pseudo console (ConPTY) to guard the 0.2 lost-stdout regression.

New fuzzers: `fuzz_chained` (export trie + chained/classic fixup decoders) and `fuzz_linker` (whole multi-image link, never executes).

## 0.4 notes

* Objective-C and Foundation fixtures are real Mach-O files built by `tools\llvm-env.cmd tools\gen-objc-fixtures.cmd` (clang `-target x86_64-apple-macos10.15` + ld64.lld against hand-written
  `tests/fixtures/objc/include` and `sysroot` stubs; no Apple SDK). Their `llvm-objdump` snapshots are in `tests/fixtures/objc/golden`. The guest programs check themselves and exit with the
  number of failed checks; the host tests assert exit codes, exact NSLog output and `objc_live_objects`.
* `fuzz_objc`: libFuzzer harness that links a mutated Mach-O with the Objective-C runtime hooks and realizes the classes (never runs guest code). Seed corpus: every file under `tests/fixtures/objc/bin`
  and `tests/fixtures/dylib/bin`.
* ASan: `repeated_runs_do_not_leak_memory_or_address_space` allows 200 MiB growth under ASan (its quarantine of freed host blocks, up to 256 MiB, otherwise trips the 24 MiB release limit); a real
  guest leak of 150 × 8 MiB stacks would still fail it.
* Threads are cooperative: the suite cannot, and does not claim to, find data races (no ThreadSanitizer on Windows here).

## 0.5 notes

* **Counts (Release, per executable): 490 run, 487 passed, 0 failed, 3 skipped, 36 suites** (0.4: 462 run, 27 suites).
* Three kinds of GUI tests: unit (no GUI), headless integration (offscreen backend, in-process), interactive (real window). The interactive ones need an input desktop; they are skipped (not failed) in service sessions or when
  `CIDER_NO_GUI_TESTS` is set, and they never block waiting: every wait has a timeout and the program under test is killed in the destructor.
* Fixture generators: `tools\llvm-env.cmd tools\gen-objc-fixtures.cmd` (Objective-C, Foundation, blocks, dispatch, CF, forwarding), `tools\llvm-env.cmd tools\gen-appkit-fixtures.cmd` (AppKit; shares headers and stubs with the Objective-C fixtures).
* New fuzzer `fuzz_tbd` (TAPI reader); `fuzz_objc` now also covers the new classes' installation. The SDK/xip/pkg tools are exercised against the real Apple downloads by hand, not in CTest (the downloads are not versioned).
* `test_execution`'s memory-growth limit is wider under ASan (its allocator quarantine); `test_linker` uses SceneKit (not AppKit) as the example of an unavailable system library.
