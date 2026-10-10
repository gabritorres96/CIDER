# Building

## Requirements

* Windows 10/11 x64.
* A C++20 compiler. Developed and tested with MSVC 14.51 (Visual Studio 2026 Community, "Desktop
  development with C++"). Visual Studio 2022 should work; **only MSVC was tested**.
* CMake ≥ 3.21 (tested with 4.2.3).
* Optional: 7-Zip (for `.dmg` extraction; tested with 7-Zip 25.01). Install from <https://www.7-zip.org/>.
* Optional: the "C++ AddressSanitizer" Visual Studio component for `-DCIDER_SANITIZE=ON`.
* Optional, for real-world validation (all kept out of git, no global install):
  portable LLVM (`clang+llvm-<ver>-x86_64-pc-windows-msvc.tar.zst` from the `llvm/llvm-project` releases,
  unpacked into `.toolchain/`; extract with `7z x` then `tar -xf`) — provides clang/ld64.lld/llvm-lipo/llvm-objdump
  and the ASan/fuzzer runtimes. `tools\llvm-env.cmd <command>` runs a command with MSVC + that LLVM on PATH.
* Not required for the normal build/tests: clang, Python, any third-party library (the test harness is in-tree).

## Commands

```bash
cmake -S . -B build -A x64                       # picks the newest installed Visual Studio generator
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Options: `-DCIDER_BUILD_TESTS=OFF`, `-DCIDER_SANITIZE=ON`, `-DCIDER_BUILD_FUZZERS=ON`.
Presets: `cmake --preset default`, `asan`, `fuzz` (see `CMakePresets.json`).

Outputs land in `build/bin/<Config>/`: `cider.exe`, the test executables and `make_dmg_fixture.exe`.

## Troubleshooting

* `LNK1104 ... clang_rt.asan_dynamic_runtime_thunk-x86_64.lib`: the ASan component is not
  installed; run the Visual Studio Installer → Modify → "C++ AddressSanitizer", or build without
  `CIDER_SANITIZE`.
* Backend tests print `[SKIP]`: 7-Zip was not found. Install it or set `CIDER_7Z` to `7z.exe`.
