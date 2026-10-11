# Swift runtime feasibility for ORVYN

Question: what would ORVYN need to execute a **Swift Mach-O x86_64 binary** (guest code), and can open-source Swift runtime components be adapted? This is a feasibility study: **no Swift support was implemented and ORVYN does not support Swift.**
Labels: **VERIFIED** (read in a source file, measured on a real binary, or observed in a run), **INFERRED**, **HYPOTHESIS**, **PENDING VALIDATION**.

## 1. Four situations that must not be confused

| # | Situation | Relevant to ORVYN? |
|---|---|---|
| 1 | **Swift compiled for Windows (PE)**: `swiftc` for `x86_64-unknown-windows-msvc`, runtime = `swiftCore.dll` | **No.** Running a PE Swift program is not running a Mach-O. It does not count as Swift support |
| 2 | **Swift compiled for macOS (Mach-O)**: `swiftc -target x86_64-apple-macos…`, calls `libswiftCore.dylib` by the Darwin ABI | **Yes, the target.** Nothing about Windows PE applies |
| 3 | **The Swift runtime built for Windows** (`stdlib/public/runtime` with `ImageInspectionCOFF.cpp`, `SwiftRT-COFF.cpp`) | **Source of ideas and code**: same C++ runtime sources, different image-inspection and platform layers |
| 4 | **The runtime a Darwin binary expects**: the Swift 5 ABI entry points (`swift_retain`, `swift_getGenericMetadata`, …) **plus the Swift standard library compiled to Mach-O**, **plus Objective-C interop**, in `/usr/lib/swift/libswiftCore.dylib` (inside the macOS shared cache on a Mac) | **Yes**: this is the contract. Situation 3 satisfies only the C++ half of it |

## 2. What a Mach-O Swift binary imports (VERIFIED, four real applications)

| Application | Imported symbols | Swift libraries linked | libswiftCore imports |
|---|---|---|---|
| CotEditor 7.0.9 | 4,371 | 26 | 1,316 (1,186 Swift-mangled `$s…` + 130 C-level) |
| Rectangle | 1,208 | 19 | 342 (+ SwiftUI 344, Combine 13) |
| MonitorControl | 1,434 | 22 | 1,086 |
| Stats | 623 | 20 | 386 |

The **130 C-level runtime entry points** CotEditor imports (extracted from its import list; the other apps use subsets) are: `swift_retain/release(_n)`, `swift_allocObject`, `swift_initStackObject`, `swift_deallocObject`, `swift_allocBox`, `swift_bridgeObjectRetain/Release`, `swift_unknownObject*` (ObjC interop), `swift_weak*`, `swift_unowned*`, `swift_getGenericMetadata`, `swift_getTypeByMangledNameInContext(2)`, `swift_getWitnessTable`, `swift_conformsToProtocol(2)`, `swift_getAssociatedTypeWitness`, `swift_getAssociatedConformanceWitness`, `swift_dynamicCast`, `swift_dynamicCastClass(Unconditional)`, `swift_dynamicCastObjC*`, `swift_initClassMetadata2`, `swift_allocateGenericClassMetadata`, `swift_initStructMetadata`, `swift_initEnumMetadata*`, `swift_getTupleTypeMetadata*`, `swift_getEnumTagSinglePayloadGeneric`, `swift_storeEnum…`, `swift_beginAccess/endAccess` (exclusivity), `swift_once`, `swift_allocError`, `swift_willThrow`, `swift_errorRetain/Release`, `swift_getKeyPath`, `swift_slowAlloc/Dealloc`, `swift_isUniquelyReferenced*`, `swift_stdlib_*`, plus data: `__swiftEmptyArrayStorage`, `__swiftEmptyDictionarySingleton`, `_swift_isaMask`, `__swiftImmortalRefCount`, `_OBJC_CLASS_$__TtCs12_SwiftObject`. A *minimal* program (arithmetic, String, Array, a function) needs a much smaller subset; the exact subset for the PoC must be measured on the PoC binary (PENDING).

Where the Foundation weight lies: **93 %** of CotEditor's Foundation imports (698/749) are Swift-mangled *overlay* symbols (`$s10Foundation10CocoaErrorV…`, `Data`, `Date`, `URL`, `Codable`), i.e. **compiled Swift code inside Apple's Foundation**; an Objective-C-only host Foundation cannot provide them (see [FOUNDATION-COREFOUNDATION.md](FOUNDATION-COREFOUNDATION.md)). Same for AppKit (112 of 310 mangled) and SwiftUI (1,267 of 1,267). So the realistic **first** Swift target is a program that uses only `libswiftCore` (no Foundation/AppKit/SwiftUI).

## 3. Measured evidence: what Apple's own `libswiftCore.dylib` needs (VERIFIED)

The user's own Xcode (`utilities/Xcode_27.2_beta_2.xip`, inventory in `utilities/reports/xcode_files.txt`) ships **real Mach-O Swift runtime dylibs** for back-deployment: `Toolchains/XcodeDefault.xctoolchain/usr/lib/swift-5.0/macosx/libswiftCore.dylib` (6,456,208 bytes) and `usr/lib/swift-5.5/macosx/libswift_Concurrency.dylib` (1,124,496 bytes). They were extracted read-only into a scratch directory (not tracked, not redistributed) with `tools/xip_inventory.py` and analysed with `orvyn inspect/dependencies` and `llvm-objdump`:

| Fact | Value |
|---|---|
| Format | Mach-O 64-bit **x86_64** dylib, `@rpath/libswiftCore.dylib`, min macOS 10.9, SDK 10.14 (the **Swift 5.0 runtime**: old) |
| Flags | `NOUNDEFS DYLDLINK TWOLEVEL BINDS_TO_WEAK NO_REEXPORTED_DYLIBS APP_EXTENSION_SAFE` |
| Segments/sections | `__swift5_typeref/capture/reflstr/fieldmd/builtin/protos/assocty/proto/types` (the metadata the runtime scans), `__gcc_except_tab`, `__unwind_info`, `__eh_frame`, `__mod_init_func` (5 initializers), classic dyld info (no chained fixups), `__objc_*` sections (it defines `_SwiftObject`) |
| Dependencies | `Foundation`, `CoreFoundation`, `/usr/lib/libicucore.A.dylib`, `/usr/lib/libc++.1.dylib`, `libSystem`, `libobjc` |
| Imports | **261 distinct**: libSystem 137, libobjc 47, CoreFoundation 28, libc++ 20, libicucore 20, Foundation 9 (+12 weak-bind records) |
| ORVYN today provides | libSystem 54 of 137 (**83 missing**: float/long-double math, `pthread_rwlock_*`, `malloc_zone_*`, `dlsym`, `dladdr`, `__dyld_register_func_for_add_image`, `_Unwind_Resume`, `___divti3`, `__stack_chk_*`, `_strtod_l`...), libobjc 34 of 47 (**13 missing**: `objc_setHook_getClass`, `_objc_realizeClassFromSwift`, `class_copyIvarList`, `objc_constructInstance`, `_objc_rootAutorelease`...), CoreFoundation 7 of 28 (`CFString*Bytes/Characters/Hash/Compare…`), **libc++ 0 of 20** (`operator new/delete`, `std::string` members, `__gxx_personality_v0`, `__cxa_demangle`), **libicucore 0 of 20** (`ubrk_*`, `unorm2_*`, `u_strTo*`, `utext_*`), classes `NSSet`, `NSEnumerator`, `NSIndexSet`, `NSProcessInfo` absent |
| Coverage of a modern app | CotEditor's 130 C-level runtime imports: **118 are exported by this 5.0 runtime**, 12 are not (`swift_getTypeByMangledNameInContext2`, `swift_conformsToProtocol2`, `swift_coroFrameAlloc`, `swift_willThrowTypedImpl`, `swift_runtimeSupportsNoncopyableTypes`...); of its 1,186 Swift-mangled stdlib imports, **1,111 exist** in the 5.0 stdlib |

Reading (INFERRED): an **old runtime covers ~91 % of the C entry points and ~94 % of the stdlib names** a modern app imports, but modern apps still need the newer pieces, and **loading Apple's runtime requires a C++ runtime, an ICU, weak-binding and unwinding in ORVYN's loader plus ~100 more libSystem/libobjc/CF symbols**. This is the measurable price of path A below.

## 4. Swift ABI facts that affect ORVYN's bridge (VERIFIED in the Swift repo)

* **Calling convention.** `swiftcc` on x86-64 keeps `r13` = context (`self`), `r12` = error, `r14` = async context (`docs/ABI/CallingConventionSummary.rst:48-53`), otherwise System V. 176 runtime declarations are annotated `SWIFT_CC(swift)` (`include/swift/Runtime/*.h`, e.g. `HeapObject.h:107,115,1156`). ORVYN's raw thunk captures only `rdi..r9`, `xmm0-7`, `rax` and the stack pointer (`abi::GuestRegs`): **`r12–r14` are not captured**. Any host function the guest calls with `swiftcc` (and any guest `swiftcc` function the host calls: witness methods, async resume, `@convention(thick)` closures) needs a new thunk variant and a new `GuestCallArgs` mode.
* **Metadata discovery.** The runtime finds types/protocol conformances by scanning image sections (`__swift5_types`, `__swift5_protos`, `__swift5_proto`, …). On Darwin this is driven by `_dyld_register_func_for_add_image` and `_dyld_get_image_*`/`dyld_priv.h` section queries: `stdlib/public/runtime/ImageInspectionMachO.cpp:15-48,144,221,245-248` (294 lines). ORVYN has **no `_dyld_*` image API** (registry), so the loader must grow one before any Swift runtime can find a conformance.
* **Objective-C interop** (`SwiftObject.mm`, `ErrorObject.mm`, `ReflectionMirrorObjC.mm`, `ObjCRuntimeGetImageNameFromClass.mm`; references `objc_retain`, `objc_debug_isa_class_mask`, `objc_absolute_packed_isa_class_mask` (`SwiftObject.mm:72,179,238`)): Swift classes on Darwin are ObjC classes (`_SwiftObject`), swift objects have ObjC-compatible `isa`; `swift_unknownObject*` call `objc_retain`. ORVYN's runtime has plain-pointer `isa` and no `objc_debug_isa_class_mask`: Swift's non-pointer-isa checks need a defined value (e.g. 0xFFFFFFFFFFFFF8 mask semantics) (HYPOTHESIS).
* **Exclusivity enforcement, TLS and access sets** (`Exclusivity.cpp`, `SwiftTLSContext.cpp`): need per-thread storage: ORVYN's TLS works (`tlv_get_addr`), `pthread_key_*` exists.
* **Concurrency** (`swift_task_*`, executors, `libswift_Concurrency.dylib` + `libdispatch` integration; `Concurrency.h:53,66` swiftcc/async cc) requires the `swiftasync` convention (`r14`) and dispatch queues/threads: far downstream.
* **Error handling**: Swift errors are *values* returned in `r12`, not exceptions; but the runtime also touches `__gxx_personality_v0` for C++ interop.

## 5. The central question: adapt the open-source runtime to serve a Mach-O Swift binary?

Three paths, from the evidence:

| Path | What it means | Requirements | Verdict |
|---|---|---|---|
| **A. Run Apple's runtime dylib as guest code** | load a `libswiftCore.dylib` (Mach-O) with ORVYN's loader; satisfy its 261 imports | section 3: weak binding, `dlsym/dladdr/_dyld_*`, `_Unwind_*`, a C++ runtime (libc++/libc++abi), ICU (system or bundled), +83 libSystem, +13 libobjc, +21 CoreFoundation, a few Foundation classes; plus the missing Objective-C features (`objc_setHook_*`) | **Technically the most faithful, and measurable**; every item is a normal ORVYN improvement that also helps non-Swift C++ programs. **Legal blocker for redistribution**: Apple's binaries cannot be shipped with ORVYN; the user would supply them from their own Xcode/macOS (PENDING legal review). Gives the *Swift 5.0* runtime: enough for 5.0-era binaries, not for modern apps |
| **B. Host-implemented runtime (adapted open-source C++), stdlib from a Mach-O** | compile `stdlib/public/runtime` for Windows as ORVYN host code, export the `swift_*` entry points via System V thunks; load the *Swift stdlib code* from a Mach-O | the runtime's `ImageInspection` layer must be rewritten for ORVYN's image list (not COFF, not Mach-O+dyld); `swiftcc` thunks (r12–r14); metadata layout must match Apple's *exactly* (the stdlib Mach-O was compiled against Apple's layout); the stdlib Mach-O still has to come from somewhere: it **is not separable from the runtime on Darwin** (they live in one dylib, `libswiftCore.dylib` exports both `swift_retain` and `$sSS…`) | **Hard to split**: the host would shadow `swift_*` symbols while the stdlib image also defines them. Possible if the loader lets the host override selected exports (a form of interposing). HYPOTHESIS; high risk of subtle ABI divergence (struct layouts of `HeapObject`, refcount encoding `swift::InlineRefCounts`, enum layouts) |
| **C. Build the open-source stdlib for `x86_64-apple-macos` ourselves** | Swift sources (Apache-2.0 + RLE) compiled with a toolchain targeting macOS, producing our own redistributable Mach-O `libswiftCore.dylib` | a Swift compiler that emits Darwin objects from Windows (whether the Windows toolchain can do this was **not verified**; it would need a macOS SDK for system modules, which Apple licenses for development and which is **not redistributable**; whether using it off Apple hardware is allowed needs legal review), `ld64`-compatible linking (`ld64.lld` is already used by ORVYN's fixtures), and an ObjC-interop-enabled stdlib build | **Only way to get a redistributable runtime**, but with ObjC interop ON the stdlib build depends on Darwin SDK headers; with it OFF the ABI differs from Apple's (no bridging, different class layout): **not binary-compatible with Apple-built apps** (they call `swift_unknownObject*` etc.). HYPOTHESIS; not tested |

**Answer to the central question:** *partly*. The **C++ runtime sources** are adaptable to Windows (they already build for PE), but a Darwin Swift binary is only satisfied by *Darwin-ABI* runtime **and** stdlib; the cleanest, most testable path is **A** (guest-loaded `libswiftCore.dylib`), which turns the Swift problem into a list of ordinary ORVYN improvements. Path B is a possible later optimisation, path C a distribution question.

## 6. Version, conflicts and risks

* **Versions:** apps built with Swift 5.9/6.x import symbols the 5.0 runtime lacks (12 runtime entry points, 75 stdlib names for CotEditor). A modern runtime exists only inside macOS (shared cache) or in newer Xcode toolchains' back-deploy folders (`swift-5.5/macosx/libswift_Concurrency.dylib` is there; `libswiftCore` for newer versions is not shipped, VERIFIED by the inventory: only `swift-5.0` and `swift-5.5` back-deploy directories).
* **Runtime conflicts:** two Swift runtimes in one process (host Windows `swiftCore.dll` for tooling vs guest Darwin runtime) must never share symbols: the host would not link a Swift runtime at all.
* **ObjC interop is mandatory** for Apple-built binaries: `swift_unknownObjectRetain` → `objc_retain`, `NSObject` subclassing from Swift, `@objc` methods (`$s…` plus `objc_msgSend`). ORVYN's Objective-C runtime must support: dynamic class creation by the Swift runtime (`objc_allocateClassPair`, `class_addMethod` ✓), `objc_setHook_getClass` (✗), non-pointer `isa` handling (✗), associated objects (✗), `objc_constructInstance` (✗). See [OBJC-RUNTIME-COMPATIBILITY.md](OBJC-RUNTIME-COMPATIBILITY.md).
* **Loader prerequisites** (also valuable alone): weak-bind + weak-def coalescing, `dlopen/dlsym/dladdr`, `_dyld_*`, terminators, `__unwind_info`/`__eh_frame` lookup, relative method lists. See [DYLD-COMPATIBILITY.md](DYLD-COMPATIBILITY.md).
* **Dependencies of the runtime:** libc++ (Apple's `libc++.1.dylib` is also a system library ORVYN does not provide: need an own build from LLVM's libc++/libc++abi for `x86_64-apple-macos` — LLVM license Apache-2.0 + LLVM exception, built with the bundled LLVM 23: a **redistributable** C++ runtime is feasible and independent of Swift), and ICU (`libicucore`: Windows' system ICU or a bundled ICU under the Unicode license — PENDING).

## 7. Proof-of-concept design (not implemented)

**Goal:** demonstrate real execution of Swift guest code, verifiable by exit code and output. A PE Swift program or any host-side Swift is **not** a valid PoC.

Program `swift_poc.swift` (compiled for `x86_64-apple-macos10.14`, Swift 5.0-compatible features only, no Foundation):

```swift
@_silgen_name("exit") func exit(_ code: Int32) -> Never   // C exit() without importing the Darwin overlay
@inline(never) func fib(_ n: Int) -> Int { n < 2 ? n : fib(n - 1) + fib(n - 2) }   // own function, arithmetic
func main() -> Int32 {
    let s = "ORVYN" + "-swift"                 // String creation
    var a = [3, 1, 2]; a.append(fib(10)); a.sort()   // Array
    let ok = s.count == 11 && a == [1, 2, 3, 55]
    print(s, a)                                // via libSystem write
    return ok ? 42 : 1                         // verifiable return value
}
exit(main())
```

Steps: (1) obtain a Swift toolchain able to emit macOS objects (Windows Swift toolchain with `-target x86_64-apple-macos10.14 -sdk <the user's MacOSX.sdk>`, or Xcode's `swiftc` on a Mac), link with `ld64.lld` against `libswiftCore.tbd` from the SDK; (2) analyse the binary with `orvyn inspect` to list the *actual* imports (expected: ≈ 20-40 `swift_*` entry points, all in `libswiftCore`, plus `libobjc`, `libSystem`); (3) satisfy the loader prerequisites of path A in the order of section 6, each with its own fixture; (4) provide the Swift 5.0 `libswiftCore.dylib` from the user's Xcode as a *local, user-supplied* guest library; (5) run `orvyn run swift_poc` and require exit code 42 and the printed line. **Acceptance:** exit code 42, output `ORVYN-swift [1, 2, 3, 55]`, `objc_live_objects` and `gui_native_objects` zero, no host-side Swift involved. **Failure is informative:** the first missing symbol names the next task.

## 8. Conclusion (justified)

**Viable, but with considerable technical investment** — not "incremental adaptation" and not "not viable".

* Evidence for *viable*: the Swift runtime is open (Apache-2.0 + Runtime Library Exception), its Darwin dependencies are a finite, **measured** list (261 imports for the 5.0 runtime; 130 C entry points for a modern app), real Mach-O runtime binaries exist on the user's machine, and ORVYN already has the harder structural pieces (a Mach-O loader, an Apple-ABI Objective-C runtime, TLS, host↔guest calls with System V structs).
* Evidence for *considerable investment*: ORVYN lacks every prerequisite the runtime needs outside Swift itself — weak binding, a C++ runtime, unwinding, `_dyld_*`, `dlsym`, ICU, ~100 libSystem/libobjc/CF symbols, `swiftcc` thunks — and applications additionally need the Foundation/AppKit/SwiftUI **Swift overlays**, which are compiled Swift code inside Apple frameworks that ORVYN cannot provide (93 % of CotEditor's Foundation imports). SwiftUI (1,267 symbols in CotEditor, 344 in Rectangle) is outside any realistic horizon.
* **Do not claim Swift compatibility on the basis of symbol analysis.** The first honest milestone is the PoC above; the first *application* milestone is an Objective-C app (see [REAL-APP-COMPATIBILITY.md](REAL-APP-COMPATIBILITY.md)).

Sources: `swiftlang/swift` `main` `c2b0f68` (2026-10-10; Apache-2.0 + Runtime Library Exception, `LICENSE.txt:205`): `stdlib/public/runtime/ImageInspectionMachO.cpp`, `ImageInspectionCOFF.cpp`, `SwiftObject.mm`, `docs/ABI/CallingConventionSummary.rst`, `include/swift/Runtime/*.h`; Apple's `dyld-1323.3` (see [DYLD-COMPATIBILITY.md](DYLD-COMPATIBILITY.md)); real binaries as stated in section 3.
