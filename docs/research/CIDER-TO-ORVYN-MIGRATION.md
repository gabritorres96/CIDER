# CIDER → ORVYN migration

**Official name: ORVYN. Former name: CIDER ("Cider Is Darwin Executable Runtime").** ORVYN is the same project, same repository, same history, same version number
(**0.5.0**: renaming is not a release). No expansion of the new name has been defined; the descriptive tagline stays "Darwin Executable Runtime".

Status of this document: everything below was checked in the repository on 2026-10-11 (VERIFIED unless marked otherwise).

## 1. State before the migration (audit)

* Branch `main`, last commit before the rename `aa8f0bf` (docs for 0.6.1-0.6.2); remote `origin https://github.com/gabritorres96/CIDER.git` (not touched; never pushed).
* 1090 tracked files; 673 mentioned the old name (1,517 occurrences of `cider`, case-insensitive; `git grep -c`), in C++ (namespace `cider`, `include/cider/`), CMake (`cider` target,
  `cider_*` libraries, `CIDER_*` options and macros), tests (`CIDER_TEST`, `CIDER_EXE` ...), fixtures (Mach-O strings and symbols such as `cider_add`), docs, CI, the Win32 window
  classes `CIDERWindow` / `CIDERView`, the log prefix `[CIDER]`, the asm labels `cider_enter_guest`, `cider_guest_thunk`.
* Baseline (Debug and Release, this machine): 39 suites, 509 tests run, 506 passed, 0 failed, 3 skipped; fresh Release build and AddressSanitizer: 39/39.

## 2. What was changed (commit `refactor: rename CIDER project identity to ORVYN`)

| Old | New | Notes |
|---|---|---|
| product name `CIDER`, `Cider` | `ORVYN`, `Orvyn` | README, docs, banner, `--version`, `doctor`, error messages |
| executable `cider.exe` | `orvyn.exe` | CMake target `orvyn`; manifest `src/cli/orvyn.manifest` |
| namespace `cider` | `orvyn` | all of `src`, `include`, `tests` |
| `include/cider/*.hpp` | `include/orvyn/*.hpp` | `git mv`: history is kept |
| `cider_*` (CMake targets, functions, asm labels, C identifiers) | `orvyn_*` | including `orvyn_enter_guest`, `orvyn_guest_thunk` |
| `CIDER_*` (CMake options, macros, environment variables of the tools) | `ORVYN_*` | `ORVYN_TEST`, `ORVYN_BUILD_TESTS`, `ORVYN_SANITIZE`, `ORVYN_EXE` ... |
| `cmake/CiderCompileOptions.cmake`, `assets/cider-icon.svg` | `OrvynCompileOptions.cmake`, `orvyn-icon.svg` | `git mv` |
| Win32 classes `CIDERWindow`, `CIDERView` | `ORVYNWindow`, `ORVYNView` | the test harness finds windows by class name and was renamed with them |
| guest-visible fixture text/symbols (`CIDER — Hello from macOS`, `_cider_add`) | `ORVYN ...`, `_orvyn_add` | the fixtures are *our* test programs, not Apple code: they were regenerated with `tools\llvm-env.cmd tools\gen-*-fixtures.cmd` (deterministic: before the rename the generators reproduced every binary byte for byte) |
| log prefix `[CIDER]` | `[ORVYN]` | |

The replacement was **not blind**: it ran from a script (`git ls-files`, UTF-8 text only, NUL-containing files skipped, line endings preserved, `CHANGELOG.md` excluded) and each class of
exception below was reviewed first. The 640 changed files compile and pass the whole suite (see section 4).

## 3. Remaining references to the old name

### 3.1 Historical and legitimate

| Where | Why it stays |
|---|---|
| `CHANGELOG.md` entries up to 0.5.0 | history: they describe releases made under the old name; a new top entry records the rename |
| `README.md` title (`formerly CIDER, "Cider Is Darwin Executable Runtime"`) | states the former name once, on purpose |
| git history, commit messages, tags | never rewritten |
| the git remote `origin` (`.../CIDER.git`) | not modified: that is a decision for the repository owner (see 5) |
| `utilities/` | untracked, ignored, not touched |
| this document | describes the migration |

### 3.2 Temporary compatibility aliases

| Alias | Behavior | Why | Removal criterion |
|---|---|---|---|
| `CIDER_LOG_FILE` → `ORVYN_LOG_FILE`, `CIDER_TEMP_DIR` → `ORVYN_TEMP_DIR`, `CIDER_7Z` → `ORVYN_7Z` | `orvyn::getenv_compat(new, legacy)`: the new name wins; the old one is used only when the new is unset/empty | scripts and shells of existing users set these variables | remove in the first release after 0.6, announcing it in the CHANGELOG |

`tests/unit/test_util.cpp::getenv_compat_prefers_the_new_name_and_falls_back_to_the_legacy_one` covers it. No alias exists for C++ namespaces, headers or CMake options: they are internal
to this repository (no installed headers, no external consumer is known) and a stale name fails at compile time, which is the safe failure.

### 3.3 Pending

| Item | Detail |
|---|---|
| Physical folder `C:\Users\gabri\Desktop\mtd\CIDER` | not renamed in this run (rule of the task). 340 golden snapshot files under `tests/fixtures/*/golden/` have the absolute path of the repository as their first line (`llvm-objdump` prints the file path). They are *not* compared by the tests (verified: the whole suite passes), but the generators will write the new folder name the next time they run. |
| GitHub repository name/URL | the remote still points at `.../CIDER.git`; renaming the repository, the remote and CI badges is a separate, outward-facing step |
| Editor / IDE state outside the repository | `.vscode`/Visual Studio settings with absolute paths, if any, are not tracked here |
| Existing build directories | `build*`, `build-asan`, `build-fuzz` are not tracked; their CMake caches contain the old `CIDER_*` option names. They were reconfigured (see 4); any other checkout must delete its build directory |

## 4. Validation (2026-10-11, this machine)

* Debug and Release build with the Visual Studio generator after deleting and regenerating the build directory; `orvyn.exe --version` prints `orvyn 0.5.0` (the version did not change).
* `ctest -C Debug` and `ctest -C Release`: 39/39 suites pass. The GUI tests drive `ORVYNWindow` and ran against the new `orvyn.exe` on the interactive desktop.
* AddressSanitizer and fuzz trees were reconfigured with the new options (`-DORVYN_SANITIZE=ON ...`) — results in `ORVYN-CURRENT-STATE.md`.
* Residual search: `git grep -i cider` outside `CHANGELOG.md` and the golden snapshots finds only the lines listed in 3.1 and 3.2.

## 5. Procedure to rename the repository folder safely (not executed)

1. Close editors, terminals and any running `orvyn.exe` / test processes whose working directory is the folder; commit or stash local work (`git status` must be clean).
2. From the parent directory: `Rename-Item C:\Users\gabri\Desktop\mtd\CIDER ORVYN` (PowerShell). Git does not store the folder name; `.git/config` has no absolute paths for this repository (check with `git config --list --show-origin`).
3. Delete the three build directories (`build`, `build-asan`, `build-fuzz`): CMake caches embed absolute paths and refuse to run from another location. Reconfigure:
   `cmake -S . -B build`; for ASan `tools\llvm-env.cmd cmake -S . -B build-asan -DORVYN_SANITIZE=ON -DORVYN_CLANG_RT_DIR=<LLVM>/lib/clang/23/lib/windows`; for fuzzers add `-DORVYN_BUILD_FUZZERS=ON -DORVYN_LIBFUZZER_SRC=<dir>`.
4. Update, outside the repository: editor workspace files, terminal profiles, scheduled tasks and shortcuts that contain the old path; Claude Code's per-project state lives under `C:\Users\gabri\.claude\projects\<mangled path>` and starts fresh for the new path.
5. Optionally regenerate the fixtures so the golden snapshot headers show the new path: `tools\llvm-env.cmd tools\gen-objc-fixtures.cmd` (and `dylib`, `exec`, `real`, `appkit`); review `git diff --stat` — only the golden headers should change.
6. Run the full suite. Then, as a separate decision, rename the GitHub repository and update the remote (`git remote set-url origin ...`); GitHub redirects the old URL, but CI badges and links in documents still say CIDER until edited.

## 6. Risks considered

* **A blind replacement could damage data.** Mitigated by the script's limits, by reviewing the identifier classes, by compiling and testing everything afterwards and by the fact that every fixture binary was regenerated, not edited. One test depended on a *partial* word (`SIDER result`, a patched literal) and was fixed by hand: that kind of case is invisible to a case-based search and is why the full suite was the acceptance criterion.
* **Persistent data.** No file format, registry key, named pipe or user-data directory used the old name (checked: `getenv`, `LOCALAPPDATA`, registry, named objects in `src`). The planned 0.6 per-application data directory has not been created yet and will use ORVYN names from the start.
* **Third-party names.** Nothing from Apple or other projects is renamed: only our own identifiers.
