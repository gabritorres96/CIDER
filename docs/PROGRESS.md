# Progress

**Current phase: CIDER 0.2.0 — first experimental execution runtime. Implemented and verified for controlled fixtures
only. CIDER 0.3 has NOT been started.**
Last verified: 2026-10-10, Windows 11 Pro 10.0.26200, MSVC 14.51 + ml64, clang-cl/LLVM 23.1.3 (portable), CMake 4.2.3, 7-Zip 25.01.

## What now works

`cider run <Mach-O x86_64>` loads the binary, maps it W^X at `0x100000000` (or relocates), applies rebase/bind/lazy-bind fixups,
bridges the System V ABI, calls `main(argc, argv, envp, apple)` on a guest stack, passes the return value to `exit`, and
reports it — all in a separate job-restricted worker process. 24 libSystem functions exist. See [EXECUTION.md](EXECUTION.md).

Real results (Debug build, `cider run tests/fixtures/exec/bin/<name>`; every value is computed independently in
`test_execution` and equals the guest's exit code):

| Fixture | What it exercises | Guest exit code |
| --- | --- | --- |
| `ret42` / `ret7` / `ret99` | A: constant return | 42 / 7 / 99 |
| `math` | B: 64-bit multiply, modulo, xor, unsigned divide | 229 |
| `stack` | C: locals, stack arrays, recursion | 91 |
| `calls` | D: direct calls + indirect call through a rebased data pointer | 67 |
| `globals` | E: initialised data, const data, pointers in data (rebase) | 59 |
| `bss` | F: zero-fill segment starts zero, writable | 232 |
| `libcall` | G: `strlen`, `abs`, `memset`, `memcpy` of CIDER's libSystem | 43 |
| `hello` | H: `puts` + `write` → "Hello from macOS!" / "second line via write" on stdout | 0 |
| `args` (`-- first second`) | I: argc/argv/envp/apple → prints `first` | 31 |
| `missing` | J: imports unimplemented `_fork` | refused before execution: exit 12, `Unresolved symbol: _fork` |
| `exitcall` | `exit(37)` from a nested function; code after it never runs | 37 |
| `malloc`, `fmath` | heap functions; `sqrt/pow/floor` through xmm registers | 181, 46 |
| `crash`, `recurse`, `spin`, `hog` | null write, stack overflow, infinite loop, 1.5 GiB request under a 1 GiB job limit | `crash`: worker dies `0xC0000005`; `recurse`: access violation / stack-overflow status; `spin`: timeout kill; `hog`: `malloc` returns NULL → 3; CIDER survives all |

Real applications: Rectangle, Stats and MonitorControl (extracted from their DMGs in 0.1.1) are **refused** with exit 12
(Foundation, AppKit, libobjc, Swift runtime, embedded frameworks not available). No application runs.

## Verification evidence

| Check | Result |
| --- | --- |
| Debug and Release build (MSVC /W4, MASM) | no errors, no warnings |
| `ctest` Debug and Release | 18/18 suites pass |
| All tests with `CIDER_LLVM_BIN`, `CIDER_REAL_DMG_DIR`, `CIDER_REAL_APPS_DIR` set (Release) | **294 tests, 0 failed, 0 skipped** (the 166 of 0.1.1 + 128 new; without those inputs 291 run + 3 skipped) |
| AddressSanitizer (clang-cl 23.1.3) | all suites clean, including guest execution and worker tests |
| libFuzzer (5 min, coverage-guided) | `fuzz_loader` 1.50 M execs (cov 2189), `fuzz_macho` 1.11 M (2 min), no crash/leak/timeout; earlier 0.1.1 fuzzers unchanged |
| Fixtures vs llvm-objdump | headers, load commands, all symbols (20 binaries) and rebase/bind/lazy-bind tables (17) equal the independent tool |
| GitHub Actions | **NOT validated**: workflow never ran on GitHub |

### Test-suite change that is a behaviour change, not a regression
The 0.1.1 test `run_is_honestly_unavailable` asserted that `cider run` returns 2 "not available". That was the correct
statement for 0.1.1 and is false by design in 0.2, so the test was **replaced** by 10 `cider run` tests (`test_cli`). Every other
0.1.1 test is unchanged and passes.

## Acceptance criteria for 0.2 (from the task)

| Criterion | State |
| --- | --- |
| Loader works with real fixtures; segments mapped; permissions applied | met (`test_loader`: every page checked with VirtualQuery) |
| ABI bridge works; control transfer is real; native instructions executed; verifiable result | met (`test_abi` with hand-assembled code, `test_execution`, patching tests, CLI) |
| Minimal external-symbol implementation; real program output | met (24 functions; `hello`) |
| Errors handled (load, symbols, crash, timeout, limits) | met |
| Previous tests still pass; Debug/Release build; ASan documented | met |
| Runtime simulates nothing | met: results come from guest instructions; see "Evidence" in EXECUTION.md |
| General macOS compatibility | **not claimed and not achieved**: this is "execution of controlled Mach-O fixtures" |

## Problems found while building 0.2 (all fixed, covered by tests)
1. clang-cl ASan + MSVC STL: `stl_asan.lib` and `/MT` conflicts → `_DISABLE_STL_ANNOTATION`, `/MD` (0.1.1 follow-up in CMake).
2. MASM received C++ flags → generator expressions restrict them to CXX.
3. Under ASan a 1.5 GiB guest `malloc` aborted the worker (ASan allocator `out-of-memory`) → guest allocations moved to a
   private Win32 heap (`HeapCreate`): NULL on failure, destroyed with the run (`test_runtime`, `hog` under ASan).
4. Host functions returning `long` truncated `-1` to 32 bits → fixed-width 64-bit types in the libc bridge (`write` returns `-1`, tested).
5. Several test-code bugs found by the sanitizers/CRT checks (dangling iterators from temporaries, narrowing) — in tests only.

## Known gaps
1. CI not run on GitHub. 2. Exactly one machine/OS build (Windows 11 26200). 3. Only x86_64. 4. The ABI bridge
covers ≤ 6 integer args, no variadics/struct returns/mixed int-double. 5. `gs`-relative TLS, exceptions and signals are unsolved
(blockers for real applications). 6. Lazy binding is eager. 7. No dependent dylib loading. 8. Fuzzing sessions are minutes long.
9. The worker is not a sandbox. 10. Not a single real application runs.

## Next phase (recommended, not started)
CIDER 0.3: dependent dylib loading + `__mod_init_func`, more libSystem (printf family via a variadic-aware thunk, stdio/files,
time, pthread basics), chained fixups, TLS design for the `gs` problem; first target: a C program compiled by clang against a
*real* libc-style subset, then an Objective-C "hello" for 0.4.
