# Dynamic linking (ORVYN 0.3)

`orvyn run` loads a Mach-O x86_64 executable **and every Mach-O dylib it needs**. Implementation: `include/orvyn/linker.hpp`,
`src/linker/linker.cpp`, `src/dyld/export_trie.cpp`, `src/dyld/chained_fixups.cpp`, `src/dyld/install_name.cpp`.
This is a small dyld, not Apple's dyld: see "Not implemented".

## Pipeline

1. **Discover** — parse + validate the executable, then each `LC_LOAD_*` dependency (depth-first). Every image file is loaded
   **once** (identity by file, not by name). Cycles are detected.
2. **Map** — every image gets its own W^X mapping (`__TEXT` r-x, data rw-, `__DATA_CONST` r--; never RWX).
3. **Bind** — per image: decode fixups (classic or chained), resolve each import through the dependency's **export trie**
   (following `LC_REEXPORT_DYLIB` and re-export entries), write rebases/binds. A bind is resolved per *(ordinal, symbol)*,
   so two libraries exporting the same name do not collide.
4. **Protect** — final W^X protections.
5. **Initialize** — only now: `__mod_init_func` and `__init_offsets`, dependencies first, executable last, each exactly once,
   called as `init(argc, argv, envp, apple)`. A failed link never runs an initializer.
6. **Run** `main`; **unload** = images are unmapped in reverse load order when the `Program` is destroyed.

## Search rules (`dyld::search_candidates`)

| Install name | Candidates |
| --- | --- |
| `/abs/path` | exactly that path |
| `@executable_path/x` | directory of the main executable |
| `@loader_path/x` | directory of the image that names the dependency |
| `@rpath/x` | one per `LC_RPATH` of the loader, then of the images that loaded it; each entry expanded relative to the image that **owns** it |
| `libSystem.B.dylib` | ORVYN's own libSystem (see DARWIN-RUNTIME.md) |
| other `/usr/lib`, `/System/Library`, relative names | **never loaded**: reported as missing |

Windows DLLs are never substituted. Install names / rpaths containing drive letters, `\`, `:`, UNC (`//`), wildcards or
control characters are refused (`not valid path text`) before any filesystem access. Each candidate must be a regular
file, a valid x86_64 `MH_DYLIB`, and is only parsed as data (never `LoadLibrary`).

Also: compatibility-version check (`LC_LOAD_DYLIB` compat > library current → refused), weak libraries (`LC_LOAD_WEAK_DYLIB`
absent → weak imports bind to NULL), image/depth/file-size limits (`LinkOptions`).

## Cycles

Mach-O allows dependency cycles and dyld loads them. ORVYN **tolerates and reports** them (`dependency cycle` warning, each library
mapped once, the initialization order ignores back edges). `LinkOptions::reject_cycles` / `GuestConfig::reject_cycles` refuse them.

## Initializers

`S_MOD_INIT_FUNC_POINTERS` (pointers, already rebased) and `S_INIT_FUNC_OFFSETS` (32-bit header offsets). Every target must
lie in an executable segment of its own image. An initializer that calls `exit(n)` ends the run with `n` (main never runs);
one that faults kills the worker (exit 14). An image whose state is *Running* is never re-entered. These are **Mach-O**
initializers: Windows CRT constructors are not involved.

## Not implemented

`dlopen`/`dlsym`, `DYLD_*` variables, shared cache, interposing, weak-symbol coalescing across images, `LC_LAZY_LOAD_DYLIB`
laziness (lazy binds are applied eagerly), terminators / `atexit`, upward-link semantics, `STUB_AND_RESOLVER` exports,
weak-bind opcode streams, arm64, frameworks (no Foundation/AppKit/libobjc exist).

## Evidence

`tests/runtime/test_linker.cpp` (37 tests) on the real fixtures of `tests/fixtures/dylib` (clang + ld64.lld, snapshots from
`llvm-objdump`; regenerate with `tools\llvm-env.cmd tools\gen-dylib-fixtures.cmd`). Example, `orvyn run .../one/one --verbose`:

```
[ORVYN][DYLD] Loading main executable
[ORVYN][DYLD] Loading libmath.dylib
[ORVYN][DYLD] Dependencies resolved (2 image(s))
[ORVYN][DYLD][DEBUG] Resolving _orvyn_add for one
[ORVYN][DYLD][DEBUG] Binding _orvyn_add in one -> 0x...
[ORVYN][DYLD] Running initializers (0 in 2 image(s))
[ORVYN][EXECUTION] Guest started
[ORVYN][EXECUTION] guest returned 42
```

The 42 comes from `orvyn_add` inside `libmath.dylib`: patching that function's `lea` in a copy of the dylib changes the result (22).
