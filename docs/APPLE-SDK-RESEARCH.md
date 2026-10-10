# Apple SDK research (CIDER 0.5)

Technical notes derived from the Apple developer downloads the user placed in `utilities/` (git-ignored, never versioned). **This document contains no Apple code,
headers or binaries**: only file names, sizes, hashes, directory structure, symbol *counts* measured by CIDER's own tools, and CIDER's conclusions. Detailed listings
that name many proprietary symbols stay in `utilities/reports/` (also ignored).

Method (2026-10-11): nothing from Apple was executed or installed; no protection was bypassed. Containers were opened read-only with CIDER's UDIF reader, 7-Zip (listing and
selective extraction) and two small Python tools written for this (`tools/pkg_payload.py`: pbzx/cpio payloads; `tools/xip_inventory.py`: streaming xar+pbzx inventory of an
`.xip`, extracting nothing). Extractions live in `utilities/extracted/` (ignored).

## 1. Inventory

SHA-256 values are in `utilities/reports/inventory.tsv`. "Real format" is what the bytes are, not the extension.

| File | Size | Real format | What is inside | Inspected | Use for CIDER |
|---|---:|---|---|---|---|
| `Xcode_27.2_beta_2.xip` | 2.04 GB | xar archive (`xar!`), one member `Content` = pbzx (xz chunks) of a cpio | `Xcode-beta.app`: 167,218 entries inventoried. Contains `MacOSX.platform` with `MacOSX27.2.sdk`, the toolchain, 8 more platforms, LLDB, Instruments | **Inventory only** (streamed, 0 files extracted): no Xcode.app is needed because the identical SDK is in the Command Line Tools | Confirms the SDK layout, shows which tools exist (`clang`, `ld`, `dyld_info`, `llvm-readtapi`, `otool`, `nm`, `lipo`, `codesign_allocate`, `metal`...). None runs on Windows |
| `Command_Line_Tools_27.2_beta.dmg` | 525 MB | UDIF, zlib, HFS+ | `Command Line Tools.pkg` (xar, 539 MB) with `CLTools_macOSNMOS_SDK.pkg` (pbzx payload, 66 MB) holding `MacOSX27.2.sdk` | **Yes**: SDK payload extracted selectively (`.tbd`, headers of libobjc/dispatch/Block/os, AppKit, Foundation, CoreFoundation, CoreGraphics, QuartzCore): 4,397 files, ~420 MB in `utilities/extracted/sdk` | **Main source** of the SDK matrix below |
| `CotEditor_7.0.9.dmg` | 25.6 MB | UDIF, LZFSE, **APFS** (7-Zip reads it) | `CotEditor.app` 7.0.9 (build 843), universal x86_64 + arm64, Sparkle.framework | **Yes**, fully extracted | Real application used as a compatibility reference: [COTEDITOR-COMPATIBILITY.md](COTEDITOR-COMPATIBILITY.md) |
| `x86_64.zip` | 54 MB | ZIP | `Intel/ba-serve`, `Intel/ba-package` | Yes: **Linux ELF x86-64 static executables** (Background Assets tools), not Mach-O | None (not macOS binaries) |
| `aarch64.zip` | 52 MB | ZIP | `Silicon/ba-serve`, `Silicon/ba-package` (same tools, arm64) | Listing only | None |
| `Additional_Tools_for_Xcode_27.2_beta_2.dmg` | 65 MB | UDIF, HFS | 957 files: `Hardware`, `Audio` tool folders | Listing only | Low |
| `Game_Porting_Toolkit_4.0_beta_2.dmg` | 104 MB | UDIF, HFS | `gptk-sample`, `metal-cpp` | Listing only | Low: Metal/Direct3D translation, unrelated to AppKit |
| `Evaluation_environment_for_Windows_games_4.0_beta_2.dmg` | 26 MB | UDIF, HFS | `redist` | Listing only | None |
| `FxPlug_SDK_4.3.5.dmg` | 0.9 MB | UDIF | `Examples` | Listing only | None |
| `Font_Tools_27_beta.dmg` | 9.9 MB | UDIF | `macOS Font Tools/Documentation` | Listing only | None |
| `Icon_Composer_1.6_Release_Candidate_2.dmg` | 17 MB | UDIF, HFS | `Icon Composer.app` | Listing only | None |
| `Reality_Composer_Pro_3.dmg` | 300 MB | UDIF, HFS | `RealityComposerPro.app` | Listing only | None |
| `Workflow_Extensions_SDK_1.0.3.dmg` | 0.5 MB | UDIF | `WorkflowExtensionsSDK.pkg` | Listing only | None |
| `ASAF_AAX_v2.1.14.dmg` | 1.2 GB | UDIF, nested disk image | Pro Tools presets and session templates | Listing only | None |
| `AccessoryNotifications_SDK_26.3.0.zip` | 1.3 MB | ZIP | framework + documentation archive (947 entries) | Listing only | None |
| `AppleLogLUTsv1.0.zip` | 2.4 MB | ZIP | colour LUTs | Not opened | None |

Why not everything was opened: the instructions asked for selective inspection; the low-priority items have no AppKit, Foundation or runtime content, so extracting
gigabytes would have produced nothing usable. No file was declined for safety reasons; `.xip` was **not** treated as a ZIP (it is xar + pbzx, handled by the streaming tool).

## 2. What the SDK stubs say (`cider sdk-scan`)

`cider sdk-scan <dir | file.tbd> [--json]` reads TAPI text stubs (`cider::sdk::parse_tbd`, fuzzed by `fuzz_tbd`) and compares them with what CIDER implements.
Run on the extracted `MacOSX27.2.sdk`: **3,702 `.tbd` files parsed, 0 failed, 3,710 distinct libraries** (x86_64-macos entries only).

| Framework / library | Exported symbols | Exported ObjC classes | Symbols CIDER implements | Classes CIDER implements |
|---|---:|---:|---:|---:|
| libobjc (`/usr/lib/libobjc.A.dylib`) | 415 | 3 | 82 | 1 (NSObject, exported by libobjc on Apple systems) |
| libSystem (with its re-exported libraries) | 9,959 | 27 | 125 | 6 (block classes and NSBlock) |
| libdispatch (the `dispatch_*` part of libSystem) | 270 | 0 | 29 | 0 |
| Foundation | 45,097 | 442 | 20 | 20 |
| CoreFoundation | 3,435 | 44 | 62 | 0 |
| CoreGraphics | 6,252 | 8 | 24 | 0 |
| AppKit | 86,530 | 1,413 | 0 (AppKit exports only data and classes in the subset) | 9 |

Read the table with care: an exported symbol is not a working API. The five levels CIDER distinguishes are *exported* (the SDK lists it), *linkable* (CIDER
resolves the symbol), *implemented* (a host function or class exists), *tested* (a compiled fixture calls it) and *behaviourally compatible* (nobody has proven that;
CIDER does not claim it for any API). The "implements" columns count the second and third levels only. The counts are small relative to Apple's surface
(for example 20 of 45,097 Foundation functions, 20 of 442 Foundation classes) on purpose: CIDER implements what fixtures need.

Observations that matter for the design:
* `libSystem.tbd` re-exports libsystem_c, libsystem_kernel, libdispatch, libsystem_blocks... as inlined documents; the analyzer follows re-exports.
* The Objective-C classes of AppKit are exported with `objc-classes:`; instance methods are **not** in a `.tbd`. Selector coverage therefore has to come from the headers or
  from the binaries that call them, not from the stubs.
* The SDK ships `.tbd` files for ~3,700 libraries; every Apple framework a real app links is described there, which makes missing-library reports exact.
* Several dozen stubs exist twice (macOS and `System/iOSSupport` / `macabi`); the analyzer prefers the `x86_64-macos` description.

## 3. Tools that exist in the Xcode toolchain (not usable on Windows, useful as references)

From the xip inventory: `clang`, `clang++`, `ld`, `ar`, `as`, `lipo`, `otool` / `otool-classic` / `llvm-otool`, `nm`, `dyld_info`, `readtapi` / `llvm-readtapi`, `install_name_tool`, `dsymutil`,
`codesign_allocate`, `metal`, `mig`, `segedit`, `llvm-objdump`. CIDER's own tests already use the portable LLVM (clang, ld64.lld, llvm-objdump) for the same purposes
(`llvm-readtapi` could validate the hand-written stubs; not done).

## 4. Findings for CIDER

1. **Link surface**: AppKit exposes 1,413 classes and 86,530 symbols; the AppKit subset (9 classes) covers a tiny fraction. Growth should be driven by real fixtures, not by the stubs.
2. **Swift is the wall** for real applications (see COTEDITOR-COMPATIBILITY.md): every application tried links `libswiftCore` with 342-1,316 imports.
3. The SDK confirms ABI details used by CIDER: `__NSConcreteStackBlock` & friends come from libSystem, `___NSArray0__` / `___NSDictionary0__` and the `NSLocalizedDescriptionKey` family
   from Foundation, `_NSApp` from AppKit, `__dispatch_main_q` from libSystem.
4. Headers: `CGGeometry.h` marks `CGRectMake` as inline but exports `CGRectGetMinX` & co. as real functions: CIDER exports the latter (CGRect passed on the stack, hidden-pointer results).

## 5. Legal and technical limits

* Apple's SDKs and developer tools are covered by Apple's licences. They are used here only to *read* public symbol/layout information on the user's machine; nothing is copied into the repository,
  `utilities/` is ignored by Git, and the hand-written headers/stubs in `tests/fixtures/objc` are written by CIDER and declare only what the fixtures use.
* The `.tbd` files and headers were extracted for analysis only; they are not redistributed.
* The Xcode `.xip` was inventoried, not installed. No Apple tool was executed.
* The beta status of these downloads means names and contents may change; the counts above are those of the files received on 2026-10-10.
