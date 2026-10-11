# cliclick 5.1 — static pre-analysis

Date 2026-10-11. **Static analysis only: cliclick was not run by ORVYN** (the analysis host was Linux; ORVYN's runtime builds only on Windows x64). Tool: `llvm-objdump` on the unmodified original.

## Provenance

| Item | Value |
|---|---|
| Source | `https://github.com/BlueM/cliclick/releases/download/5.1/cliclick.zip` (release asset of the upstream project) |
| `cliclick.zip` SHA-256 | `710910fd2dade6a3b5889594bfc019e7d4c2beee09fd777503e71ea82acc91bc` (55,835 bytes) |
| `cliclick` SHA-256 (FAT) | `7c76e378713182f615330d451fa643da65f29dfc62091680886ff60a3f542912` (235,312 bytes) |
| Licence | BSD-3-Clause (upstream `LICENSE`, read in the earlier phase). The binary is **not** committed here; fetch it from the URL above |
| Slices | FAT: x86_64 (offset 16384, size 104,304), arm64 (offset 131072, size 104,240) |

## Findings (VERIFIED with llvm-objdump, x86_64 slice)

* **Dylibs:** Foundation, ApplicationServices, Carbon, IOKit, **Cocoa**, libSystem, AppKit, CoreFoundation, CoreGraphics, CoreServices, libobjc. No Swift, no libc++.
* **Fixups:** classic opcodes (bind + lazy bind), `__DATA_CONST`; **no weak binds** (weak-bind table empty) and **no chained fixups**.
* **Objective-C metadata:** method lists have `entsize 24` (three 8-byte pointers): **absolute, not relative**. Class flags `0x10` / `0x11 RO_META`, `__objc_imageinfo` = `0x40` (no small-method-list flag).
* **Initializers:** no `__mod_init_func` section.
* **Unwind:** `__unwind_info` present.
* **Data imports (bind):** `_NSZeroPoint`, `_kTISPropertyLocalizedName`, `_kTISPropertyUnicodeKeyLayoutData`, `__NSConcreteGlobalBlock`, `___stack_chk_guard`, `___stderrp`, `_optarg`, `_optind`, `dyld_stub_binder`, `_NSStringPboardType`, `___CFConstantStringClassReference` (many `__cfstring`), and **`_OBJC_EHTYPE_$_NSException`**.
* **Classes used:** NSAutoreleasePool, NSCharacterSet, NSFileHandle, NSFileManager, NSMutableString, NSNumber, NSPredicate, NSProcessInfo, NSString, NSBitmapImageRep, NSEvent, NSPasteboard, NSWorkspace, NSArray, NSDictionary, NSException, NSMutableArray, NSMutableDictionary, NSURL.
* **Lazy-bound functions:** `AXIsProcessTrusted`, `CFDataGetBytePtr`, `CFRelease`, `CGEventCreate*`, `CGEventGetLocation/Post/SetIntegerValueField/SetType`, `CGImageRelease`, `CGWindowListCreateImage`, `LMGetKbdType`, `NSClassFromString`, `TISCopyCurrent*`, `TISGetInputSourceProperty`, `UCKeyTranslate`, **`_Unwind_Resume`**, `__stack_chk_fail`, `atoi`, `fprintf`, `fwrite`, `getopt`, `nanosleep`, `usleep`, `printf`, `puts`, `objc_alloc`, `objc_alloc_init`, `objc_autorelease`, **`objc_begin_catch`, `objc_end_catch`**, `objc_enumerationMutation`, `objc_msgSendSuper2`, `objc_opt_class`, `objc_sync_enter/exit`.

## What this means for ORVYN

| Planned blocker | Does cliclick need it? |
|---|---|
| Relative method lists | **No** (absolute lists) |
| Weak binding | **No** (empty weak-bind table) |
| `Cocoa` umbrella + stub libraries (ApplicationServices, Carbon, IOKit, CoreServices) | **Yes** (all named in `LC_LOAD_DYLIB`) |
| `@synchronized` (`objc_sync_*`) | **Yes** |
| libc: `getopt` + `optarg/optind`, `nanosleep`, `usleep`, stack protector | **Yes** |
| `NSPredicate`, `NSFileHandle`, `NSCharacterSet`, `NSProcessInfo`, `NSClassFromString`, `objc_enumerationMutation` | **Yes** (not in ORVYN's class list) |
| **`@try/@catch` (`objc_begin_catch/end_catch`, `_Unwind_Resume`, `OBJC_EHTYPE_$_NSException`)** | **Yes — and ORVYN refuses images that use it at load time** (see [OBJC-RUNTIME-COMPATIBILITY.md](../research/OBJC-RUNTIME-COMPATIBILITY.md) §5) |

**Correction to earlier research:** [REAL-APP-COMPATIBILITY.md](../research/REAL-APP-COMPATIBILITY.md) presented cliclick as a small first target with 38 imports and did not list exception handling. This analysis shows it depends on the unwinder (Top-20 item 19, XL). Until that is resolved, cliclick is **not** a short-path first target.

Unknown (needs a run): whether `-V`/`-h` reach code that throws; the refusal at load happens regardless.

## Options

1. Implement the minimum exception support (`objc_begin_catch/end_catch`, a personality for guest frames) — large, shared with C++ and Swift.
2. Allow images that reference exception symbols to load, with a **call-time trap** only when an exception is actually thrown or caught. Honest, but the guest's `@catch` blocks would never run; must be reported as unsupported.
3. Pick another small Objective-C binary without `@try` (run the same checks first: `llvm-objdump --macho --lazy-bind | grep -E 'begin_catch|Unwind'`).

Decision pending the owner.

## ORVYN 0.6 staged run (2026-10-11, Windows; binary downloaded with the owner's approval, SHA-256 verified)

`cliclick.zip` SHA-256 and the FAT binary SHA-256 match the values above. Everything below is **static** (`orvyn inspect`, `orvyn frameworks`); cliclick was **not executed**.

| Stage | Result |
|---|---|
| A. Analysis | **passed**: FAT (x86_64 + arm64) parsed, x86_64 slice selected |
| B. Loader / dependencies | **passed**: all 11 libraries are recognised (Foundation, AppKit, CoreFoundation, CoreGraphics, libSystem, libobjc = implemented subsets; Cocoa = umbrella; ApplicationServices, Carbon, IOKit, CoreServices = recognised, not implemented) |
| C. Binding | **failed**: 76 imports - 47 implemented, 10 stubs (`AXIsProcessTrusted`, `LMGetKbdType`, `TISCopyCurrent*`, `TISGetInputSourceProperty`, `UCKeyTranslate`, `__Unwind_Resume`, `objc_begin_catch`, `objc_end_catch`, `__objc_personality_v0`), **19 missing** |
| D-F. Runtime, CLI, verification | **not reached** (the link is refused; there is no stdout/stderr/exit code) |

The 19 blocking imports: classes `NSCharacterSet`, `NSFileHandle`, `NSPredicate`, `NSProcessInfo`, `NSBitmapImageRep`, `NSPasteboard`, `NSWorkspace`; data `kTISPropertyLocalizedName`, `kTISPropertyUnicodeKeyLayoutData`, `NSStringPboardType`; functions `CFDataGetBytePtr`, `CGEventCreate`, `CGEventCreateKeyboardEvent`, `CGEventCreateMouseEvent`, `CGEventGetLocation`, `CGEventPost`, `CGEventSetIntegerValueField`, `CGEventSetType`, `CGWindowListCreateImage`. Exceptions are no longer a loading blocker (0.6), but a throw would still stop the program.

Not done on purpose: stubbing the `CGEvent*` family or declaring empty placeholder classes would make the link succeed while hiding that cliclick's whole purpose (synthesising input) does not exist on ORVYN; injecting real Windows input from a macOS binary is a safety decision for the owner.

### Second external binary
Selection criteria: no Swift, no libc++, no exceptions, few dependencies, clear licence, original upstream binary, x86_64, reproducible. A web search found **no candidate** meeting all of them (results were frameworks, Swift rewrites or GPL tools). No further download was made. Next step: find an upstream release asset or build a small Objective-C CLI with the owner's toolchain, then run `orvyn frameworks <binary>` first - it says in seconds whether the link can succeed.

### Re-check at 0.6.8 (2026-10-11)
After the libSystem additions for jq, `orvyn frameworks` on the same binary still reports 76 imports: 47 implemented, 10 stubs, **19 missing** (no regression, no change). First blocker in `orvyn run`: `_OBJC_CLASS_$_NSCharacterSet`. Not run. `CGEvent*` is not implemented and no real input injection exists.
