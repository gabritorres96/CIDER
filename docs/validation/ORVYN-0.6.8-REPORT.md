# ORVYN 0.6.8 validation report (internal milestone, not a release)

Labels: VERIFICADO / PARCIAL / PENDIENTE / BLOQUEADO / NO VERIFICABLE. No compatibility percentages are used anywhere. Version stays **0.5.0**.

## Environment

Windows 11 Pro 10.0.26200 x64 · MSVC 14.51.36231 (Visual Studio 18 Community) · CMake 4.2.3 · LLVM/clang/clang-cl/ld64.lld 23.1.3 (portable, `.toolchain`) · libFuzzer built from the official llvm-project `compiler-rt/lib/fuzzer` sources (sparse checkout in `.toolchain/lf`) · branch `claude/orvyn-objc-darwin-3d9584`, base `2184d2f`, nothing pushed, `main` untouched.

## A. Audit and regressions

| Item | Result |
|---|---|
| Git audit (status, branch, diff, `git diff --check`, authors) | VERIFICADO: clean tree at start; every commit authored and committed by `gabritorres96 <338864508+gabritorres96@users.noreply.github.com>`; no co-author trailers |
| Release, `ctest -j1` and `-j4`, `-j8` | VERIFICADO: all suites pass (41 suites after this phase) |
| Debug | VERIFICADO: 40/40 suites at the start of the phase (re-run at the end, see the table at the bottom) |
| Fresh build (new directory `build-fresh`, no previous artifacts) | VERIFICADO: configure + build rc 0 in 301 s; `ctest -j1` 40/40 pass in 113 s (before the jq work) |
| AddressSanitizer (clang-cl, all suites) | VERIFICADO: all suites pass, including the jq tests |
| `test_threads` intermittency | **VERIFICADO as a TEST defect, fixed** (below) |

### test_threads
Reproduced with the ASan build, 16 copies in parallel: 2 failures in 160 runs, both `without_the_mutex_updates_are_lost_which_proves_real_interleaving` (`counter == 4000`, expected `< 4000`). Cause: the fixture let each guest thread start counting as soon as it existed; under host load a thread that is slow to *start* found the others finished and no update was lost. No scheduler race was found (the mutex test, the TLS test and the exit/timeout tests never failed). Fix: a start barrier in `thread_race.c` (all four threads exist before any counts). After the fix: 0 failures in 240 parallel runs; `ctest --repeat until-fail:30` passes. No sleep or timeout was added. Residual risk: the scheduler itself was reviewed by reading only, not formally verified.

## B. ABI audit

### Relative method lists (PARCIAL)
Reviewed `small_method_list`: entry size 12 enforced (flags bits excluded), 4-byte alignment, count bound, every field offset computed in 64-bit and required to stay inside the image, indirect selectors required to point into a `__objc_selrefs` slot of the same image, strings read with the bounded checked reader. Tests: both selector modes, categories, class methods, 800 random corruptions (ASan clean), 40,195 structure-aware fuzz executions. **Not done:** (1) no authentic ld64-produced x86_64 binary was available: the only sample found (`trash` v1.0.0) is arm64-only and defines no classes; (2) **protocol method lists are not read at all, absolute or relative** (the runtime only needs protocol names for `conformsToProtocol`); (3) the imp pointer is not checked to be executable. **No claim of complete modern Objective-C metadata support is made.**

### Weak binding (VERIFICADO for the supported formats)
Reviewed ordering (strong export beats weak, else first image in load order), sites of symbols nobody defines (kept, never nulled), re-exports and resolver exports skipped. New tests: chained-fixup images get no coalescing and a mixed chained + `weak_bind` image is refused; `-flat_namespace` binds resolve by name across images; a missing flat-namespace symbol fails the link (`(flat lookup)`). Not implemented: interposing, `DYLD_FORCE_FLAT_NAMESPACE`, chained-format weak coalescing.

### Lazy binding (PARCIAL, documented proposal)
Lazy binds are applied eagerly, so an unresolved import that is never called still fails the load. Changing this affects ABI/state/concurrency (first-call resolution under the cooperative lock, trampoline state). Proposal for a later phase: bind lazy imports to per-symbol trap thunks (same mechanism as the honest stubs) **only for symbols the provider knows by name and prototype**; unknown names must still fail at load. Not done here.

### libc (PARCIAL)
Names checked against real compiler output and jq's import table; the audit found one real bug: the stack probe is `____chkstk_darwin` (four underscores), not `___chkstk_darwin`; fixed, with an LLVM-emitted call fixture (`chkstk_probe.ll`, the compiler produces `mov eax,size; call ____chkstk_darwin; sub rax,rsp`) in addition to the hand-written register-preservation test. `malloc_size`: Darwin returns the allocator's rounded usable size (>= requested); ORVYN returns the requested size from `HeapSize`, which satisfies "at least what was asked" and is 0 for non-malloc pointers; programs that depend on the exact rounding are not covered. `getopt`: BSD algorithm, globals in guest memory, reset per process; not thread-safe by design (guest threads are cooperative). `rand()` state is per guest process (bug fixed). Guest time zone is UTC and locale is "C" only (documented).

### Synchronization (PARCIAL)
`objc_sync_*`: recursion, independent objects, ownership errors, three contending guest threads (150 exact updates). Associated objects: ASSIGN / RETAIN / COPY, replacement, removal, atomic getter retain+autorelease, release on dealloc, undefined policy stops the program. Host tables key on guest addresses and never dereference them. All of this is demonstrated **only with cooperative guest threads**; no claim about real parallelism.

### Stub signatures (VERIFICADO)
Every remaining stub prototype was checked against the macOS 11.3 SDK headers (read from a public mirror, nothing copied into the repo). `IOMainPort` could not be verified (macOS 12 API) and was **removed**; it now fails at load. `orvyn frameworks` labels: partially implemented (subset) / umbrella / stubs only, and states that a framework's status does not guarantee its symbols. A symbol is a stub only if listed with its prototype; data symbols and unlisted functions are refused.

### Exceptions (PARCIAL)
Images that import the unwinding entry points load; a program that never throws runs normally; a throw in such a program stops with a "no unwinder" message; calling `objc_begin_catch`, `objc_end_catch`, `_Unwind_Resume`, `objc_exception_rethrow`, the personality routine or throwing nil directly stops the program and never returns (new fixture `objc_exc_direct`). Nothing is caught, nothing is propagated. No unwinder was implemented.

## C. Fuzzing (VERIFICADO, short sessions)
`clang-cl` + ASan + libFuzzer (`build-fuzz`), seeded with the in-repo linked fixtures (not the external binaries). No crash, timeout, OOM or leak in any run.

| Target | Runs | Time |
|---|---|---|
| `fuzz_macho` | 1,213,796 | 121 s |
| `fuzz_loader` | 757,434 | 121 s |
| `fuzz_linker` | 325,722 | 121 s |
| `fuzz_chained` | 821,297 | 121 s |
| `fuzz_objc` | 8,171 | 122 s |
| `fuzz_cg` | 864,703 | 121 s |
| `fuzz_objc_relative` (new, structure-aware) | 8,771 + 40,195 | 122 s + 601 s |

`fuzz_objc_relative` starts from valid linked programs (absolute, converted direct, converted indirect, categories) and mutates only Objective-C data. It is slow (about 70 executions/s because each input builds a runtime). Sessions of minutes are not a security proof. Command: `build-fuzz\bin\fuzz_objc_relative.exe <corpus> -max_total_time=600 -timeout=20 -rss_limit_mb=4096 -max_len=600`. The corpus lives in the scratch directory, not in the repo.

## D. First external binary (see [../compatibility/JQ-1.7.1.md](../compatibility/JQ-1.7.1.md))
jq 1.7.1 `jq-macos-amd64`, SHA-256 `4155822b…7443`, unmodified: stages A-F VERIFICADO, G PARCIAL (compared with documented jq behaviour, not with a macOS run). Exit codes 0/2/3/5 as jq defines them. Required 58 new libSystem symbols and an `orvyn run --stdin` option. Second binary (`trash` v1.0.0): rejected (arm64 only). cliclick 5.1: still BLOQUEADO at binding (19 imports, first one `NSCharacterSet`); no regression. No `CGEvent` support, no input injection.

## Launcher contract
`orvyn run --json` gained `stubbed_imports`; `orvyn frameworks [--json]` is new; `inspect`, `dependencies`, `compatibility`, `doctor` are unchanged. A static `frameworks` result says "would link", never "runs". The launcher was not modified.

## Open items
PENDIENTE: authentic x86_64 relative-method-list sample; protocol method lists; lazy-binding model; formal review of the cooperative scheduler; longer fuzzing; comparing jq output with a real macOS run (NO VERIFICABLE without a Mac); `_CTYPE_*` bits other than A/S and `rand()` bit-exactness against macOS.

## Final run (head of the branch, after all changes)

| Check | Result |
|---|---|
| Debug, `ctest -j4` | VERIFICADO: 41/41 suites |
| Release, `ctest -j1` | VERIFICADO: 41/41 suites (117 s); per-executable totals: 551 tests run, 0 failed, 3 skipped (dmg 1, real_macho 2) with `ORVYN_EXTERNAL_JQ` and `ORVYN_EXE` set; baseline at the start of 0.6: 509 run, 3 skipped |
| Fresh build in a new directory (`build-fresh`) | VERIFICADO: build rc 0 in 342 s, `ctest -j4` 41/41 |
| ASan (clang-cl), `ctest -j2` | VERIFICADO: 41/41, including the jq tests |
