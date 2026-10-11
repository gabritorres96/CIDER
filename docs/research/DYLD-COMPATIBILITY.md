# dyld compatibility: Apple's dyld vs the ORVYN loader/linker

Purpose: raise the fidelity of the Mach-O loader **without rewriting it**. Method: read Apple's dyld at the tag shipped with macOS 26.0 and compare each behavior with ORVYN's code.
Labels: **VERIFIED** (read in source or observed in a run), **INFERRED**, **HYPOTHESIS**.

## 1. Sources studied

| Item | Value |
|---|---|
| Repository | https://github.com/apple-oss-distributions/dyld (official Apple organisation) |
| Tag / commit | `dyld-1323.3` / `6379117` (2025-10-16) — the version listed in `distribution-macOS` tag `macos-260` (`release.json`: "macOS 26.0"). The `main` branch is newer (`dyld-1378`, `fd8d0c4`, 2026-06-15); not used for citations to avoid mixing versions |
| License | Apple Public Source License 2.0 (`APPLE_LICENSE`, "Version 2.0 - August 6, 2003") — see [THIRD-PARTY-LICENSE-REVIEW.md](THIRD-PARTY-LICENSE-REVIEW.md) |
| Relevant directories | `dyld/` (the loader: `Loader.cpp`, `JustInTimeLoader.cpp`, `PrebuiltLoader.cpp`, `DyldAPIs.cpp`, `DyldRuntimeState.cpp`, `dyldMain.cpp`), `common/` (`MachOFile.cpp`, `MachOLoaded.cpp`, `MachOAnalyzer.cpp`), `libdyld/` (`ThreadLocalVariables.cpp`, `threadLocalHelpers.s`), `include/mach-o/` (`fixup-chains.h`, `dyld_images.h`), `cache_builder/` (shared cache) |
| Does it run on Windows? | No: it is the Darwin process loader (mach_vm, `__DATA_CONST`, shared cache, libSystem helpers). **Reference only** |

ORVYN code studied: `src/loader/*`, `src/linker/linker.cpp` (610 lines), `src/dyld/{chained_fixups,export_trie,install_name,relocation_engine,symbol_resolver}.cpp`, `src/security/execution_validator.cpp`, `src/libsystem/tls.cpp`, `src/macho/*`.

## 2. Difference matrix

State: **P** implemented and tested · **S** implemented partially · **A** absent · **I** incorrect (differs from dyld in a way that can matter) · **N** not applicable to ORVYN.

| # | Feature | Apple dyld (cite) | ORVYN (cite) | State | Notes |
|---|---|---|---|---|---|
| 1 | Mach-O header/FAT parse, x86_64 slice | `common/MachOFile.cpp` | `src/macho/macho.cpp`, `test_macho`, `test_fat`, `fuzz_macho` | P | arm64/arm64e slices are inspected, never executed |
| 2 | Load commands for execution | `Loader::loadDependents` `dyld/Loader.cpp:296`, `JustInTimeLoader.cpp:465` | `src/linker/linker.cpp:174-239` (discovery, rpaths, cycles) | P | `LC_LOAD_DYLIB`, `_WEAK_`, `_REEXPORT_`, `_UPWARD_`, `_LAZY_`, `LC_RPATH`, `LC_ID_DYLIB` are parsed; upward and lazy-load are treated like normal loads |
| 3 | Segment mapping, W^X | `Loader::mapSegments` `dyld/Loader.cpp:1542` (called from `JustInTimeLoader.cpp:1425`) | `src/loader/segment_mapper.cpp`, `image_layout.cpp` (r-x text, rw- data, r-- `__DATA_CONST`, never RWX) | P | `__PAGEZERO` not mapped; 64 KiB-aligned bases (Windows reservation granularity), preferred base `0x100000000` else slide |
| 4 | Rebase opcodes | `Loader::applyFixupsGeneric` `dyld/Loader.cpp:2332` | `src/dyld/relocation_engine.cpp`, verified against `llvm-objdump --rebase` on 20 binaries | P | |
| 5 | Bind / lazy-bind opcodes | same | `relocation_engine.cpp` (lazy binds **applied eagerly**; `dyld_stub_binder` traps) | S | Eager binding fails a program whose lazily-bound symbol is never called but missing; dyld would not fail |
| 6 | Weak-binding stream (`weak_bind`) and weak-definition coalescing | `Loader::forEachBindTarget` `dyld/Loader.cpp`, weak-def map `WeakDefMap` `dyld/DyldRuntimeState.h:352-359,578` | `relocation_engine.cpp:243` returns Unsupported; `execution_validator.cpp:39` | **A** | Every C++ program (inline functions, templates, `operator new`) and every Swift program carries weak definitions: they are refused today |
| 7 | Chained fixups | `include/mach-o/fixup-chains.h`; `MachOFile::withChainStarts` `common/MachOFile.cpp:1513`, `forEachChainedFixupTarget` `:1638` | `src/dyld/chained_fixups.cpp:1-10`: formats `DYLD_CHAINED_PTR_64`, `_64_OFFSET`, imports formats 1-3, 4 KiB/16 KiB pages; refuses arm64e (1,7,9,10,12), 32-bit, kernel-cache and "multiple starts in one page" | S | covers x86_64 userland binaries built with `-fixup_chains`; `test_chained_fixups`, `fuzz_chained` |
| 8 | Two-level namespace | `Loader::resolveSymbol` `Loader.cpp:2787` (ordinal → loader) | `linker.cpp:335-372`: bind per *(ordinal, symbol)*; special ordinals 0 (self), -1 (main), -2 (flat) | P | `MH_FORCE_FLAT` / `DYLD_FORCE_FLAT_NAMESPACE` ignored |
| 9 | Re-exports (`LC_REEXPORT_DYLIB`, export-trie re-export entries) | `Loader.cpp` (`searchNonReExports` handling) | `linker.cpp:537-547` (trie re-export entries and re-exported dylibs) | P | frameworks provided by ORVYN re-export each other |
| 10 | Upward links | `Loader::runInitializersBottomUp` `dyld/Loader.cpp:2565` (upward links are delayed) | cycle detection + "initialization order ignores back edges" `linker.cpp:224-226,570` | S | cycles are legal and reported; upward semantics (delayed initialization) are not modelled |
| 11 | Lazy-load dylibs | dyld loads on first use | loaded eagerly | S | |
| 12 | `@rpath`, `@loader_path`, `@executable_path` | `Loader::forEachPath` `Loader.cpp:713`, `expandAtLoaderPath` `:1207` | `src/dyld/install_name.cpp`, `linker.cpp:174-194` (each rpath expanded relative to the image that owns it, own entries first then the loaders') | P | Windows drive letters, `\`, UNC and control chars refused; `DYLD_INSERT_LIBRARIES` rules (no `@loader_path`) N/A |
| 13 | `DYLD_*` environment, path overrides | `ProcessConfig::PathOverrides` (`DyldProcessConfig.cpp`) | none | A | deliberate: the guest environment is ORVYN's |
| 14 | Dependencies from the system (`/usr/lib`, `/System/Library`) | dyld shared cache | **never loaded**: reported missing; only ORVYN's libSystem and its own framework subsets are *provided* | I | the central problem for real apps, see section 4 |
| 15 | Compatibility/current version check | dyld checks the dependency's compatibility version while loading dependents (`Loader.cpp`; function not individually cited) | refused if the dependency's compatibility version is newer (`linker.cpp`) | P | |
| 16 | Initializers (`__mod_init_func`, `__init_offsets`) | `Loader::findAndRunAllInitializers` `Loader.cpp:2527`; `MachOFile::forEachInitializerPointerSection` `common/MachOFile.cpp:1997` | bottom-up, once each, `init(argc, argv, envp, apple)`; every target must be in an executable segment | P | `test_linker` (37 tests) |
| 17 | Terminators / static destructors | registers `__cxa_atexit` for old `__mod_term_func` `Loader.cpp:2550-2553` | none: `_atexit` exists, `__mod_term_func` and `__cxa_atexit` do not (`orvyn sdk-scan @implemented`) | A | C++ static destructors never run |
| 17b | `__thread_vars` TLV | `libdyld/ThreadLocalVariables.cpp`, `threadLocalHelpers.s:54-68` (`_tlv_get_addr` is `_tlv_bootstrap+8`) | `src/libsystem/tls.cpp:20-61` host `tlv_get_addr`: per-guest-thread lazily created block, never touching `%gs`/the TEB | P | different mechanism, same ABI for the guest; `test_tls` |
| 18 | `dlopen`/`dlsym`/`dlclose`/`dladdr` | `APIs::dlopen` `dyld/DyldAPIs.cpp:1302`, `dlsym` `:1774` | absent | A | `NSBundle`-style plug-ins, many frameworks and every Swift runtime need them |
| 19 | `_dyld_*` image API (`_dyld_image_count`, `_dyld_get_image_header`, `_dyld_register_func_for_add_image`, `_dyld_get_image_vmaddr_slide`) | `DyldAPIs.cpp` | absent (no `_dyld_*` symbol is registered) | A | the ObjC runtime and Swift runtime register for image loading |
| 20 | Interposing (`__DATA,__interpose`) | `RuntimeState::buildInterposingTables` `DyldRuntimeState.cpp:1274` | none | A | rare in applications; common in injected libraries |
| 21 | Shared cache | `cache_builder/`, `SharedCacheRuntime.cpp` | none: ORVYN provides library *content* itself | N | the `.tbd`/dyld-cache inventory only informs symbol coverage |
| 22 | Closures / prebuilt loaders | `PrebuiltLoader.cpp` | none | N | an optimisation |
| 23 | `LC_MAIN` | `dyldMain.cpp` | `execution_validator.cpp:35-37`; `LC_UNIXTHREAD` unsupported | P | |
| 24 | Code signature | `common/CodeSigningTypes.h`, kernel enforcement | parsed for display (`src/macho/codesign.cpp`), never enforced | N | ORVYN is not macOS; entitlement-dependent apps are a separate problem |
| 25 | Objective-C image registration (`__objc_classlist`, categories, selectors) | `map_images` `objc4-950 runtime/objc-runtime-new.mm:3546`, driven by dyld's `_dyld_objc_notify_register` callbacks | `src/objc/objc_metadata.cpp` (registration at load, own runtime) | P | `fuzz_objc` |
| 26 | Swift metadata sections (`__swift5_*`) | `common/SwiftVisitor.cpp` (cache optimiser), runtime scans images | ignored (no Swift runtime) | A | see [SWIFT-RUNTIME-FEASIBILITY.md](SWIFT-RUNTIME-FEASIBILITY.md) |
| 27 | Unwind info (`__unwind_info`, `__eh_frame`, `__gcc_except_tab`) | ``APIs::_dyld_find_unwind_sections` `dyld/DyldAPIs.cpp:2055` | none; no `_Unwind_*` | A | blocks C++ exceptions and Objective-C `@try/@catch` |

## 3. Observed behavior (run, not read)

`orvyn run CotEditor.app` (see [REAL-APP-COMPATIBILITY.md](REAL-APP-COMPATIBILITY.md)): main Mach-O parsed (5 segments, 71 load commands), `Sparkle.framework` located through `@rpath` and parsed, then the link aborts before mapping with
"Missing library" for `/usr/lib/libcompression.dylib`, `IOKit`, `Security`, `ServiceManagement`, `SystemConfiguration`, `WebKit`, `AVFoundation`, `SwiftUI`, `Combine`, `/usr/lib/libc++.1.dylib`, `/usr/lib/swift/libswiftCore.dylib`, ...;
weak libraries absent are only warned (`libswiftCoreAudio`...). Failing before mapping and before any guest code is the designed behavior (`linker.cpp`: "resolve every bind before mapping so unresolved symbols fail with no side effects").

## 4. The five loader problems that most block real applications

Ranked by how many of the four analysed applications each one stops, and how early (VERIFIED against the application reports and the run above unless noted).

1. **System dependencies are never loadable** (row 14). Even Sparkle (non-Swift) asks for `libcompression`, `IOKit`, `Security`, `ServiceManagement`, `libc++`. The linker only knows *provided* libraries (`libSystem`, the framework subsets). Any real app needs a **provided-library policy** that lists which Apple libraries ORVYN claims (and supplies them), plus generated stubs for the rest that fail *at call time with a named symbol* rather than at load time. *Solution direction:* load-time "soft" mode for non-critical weak-ish libraries and a call-time trap table (HYPOTHESIS: needs a decision on honest reporting so no fake compatibility is claimed).
2. **No C++ runtime** (`libc++.1.dylib`, `libc++abi`, `__cxa_*`, `_Unwind_*`, weak definitions row 6, terminators row 17, unwind rows 27). Verified missing in CotEditor/Sparkle's list and in the registry. Without weak-binding support the validator refuses any C++ image at `execution_validator.cpp:39`.
3. **No Swift runtime** (`libswiftCore` + overlays; rows 18, 19, 26). Verified: all four applications link `/usr/lib/swift/libswiftCore.dylib` (CotEditor 26 Swift libraries, Rectangle 19, Stats 20, MonitorControl 22).
4. **No `dlopen`/`dlsym`/`_dyld_*`** (rows 18-19): required by the Swift runtime, `NSBundle`, plug-in loading and by Foundation itself. They need a loader API that can map additional images after start (the linker currently builds the whole image set up front).
5. **Eager lazy-binding and no weak symbols** (rows 5, 6): behavior differs from dyld in ways that make otherwise valid programs fail (a missing symbol in an unused stub, duplicated weak definitions that must be coalesced).

## 5. Priorities for the loader (feeds [TOP-20-COMPATIBILITY-IMPROVEMENTS.md](TOP-20-COMPATIBILITY-IMPROVEMENTS.md))

1. Weak-bind + weak-definition coalescing (rows 6, 17) — small, testable with clang-built C++ fixtures.
2. `dlopen`/`dlsym`/`dladdr` + `_dyld_*` image callbacks on top of a "load more images" API (rows 18, 19).
3. Lazy binding to real stubs (row 5) with `dyld_stub_binder`-compatible resolution on first call.
4. Terminators / `__cxa_atexit` (row 17).
5. A documented provided-library policy and "stub library" mechanism (row 14).
6. Unwind-section lookup (`_dyld_find_unwind_sections`) when exceptions are tackled (row 27).

Not recommended: porting dyld's `Loader`/closure machinery. It assumes Darwin primitives (mach VM, shared cache, libSystem helper vtable); the ORVYN loader is 1,500 lines and tested, and the missing behaviors are small
and local. Keep dyld as the *specification* and its `common/MachOFile.cpp` as the reference for structure decoding.
