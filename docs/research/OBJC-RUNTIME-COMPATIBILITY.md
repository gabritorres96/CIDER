# Objective-C runtime compatibility

Question: how close is ORVYN's own Objective-C runtime to what real Mach-O programs expect, what is missing, and what can be learned from open-source runtimes — **without** swapping the runtime.
Labels: **VERIFIED** (read in a source file or observed in a run), **INFERRED**, **HYPOTHESIS**, **PENDING VALIDATION**.

## 1. Sources

| Source | Version | License |
|---|---|---|
| Apple `objc4` | `objc4-950` `6dee1d7` (macOS 26.0; `main` is `objc4-951.1`) | APSL 2.0 (reference only, see [THIRD-PARTY-LICENSE-REVIEW.md](THIRD-PARTY-LICENSE-REVIEW.md)) |
| GNUstep `libobjc2` | `main` `aabc524` (2026-10-08) | MIT (`COPYING`, "Copyright (c) 2009 David Chisnall") |
| ORVYN | `src/objc/` (1,587 lines), `src/foundation/nsobject.cpp`, `src/foundation/nsinvocation.cpp`, `docs/OBJECTIVE-C-RUNTIME.md` | own |

## 2. API against ABI: the two are different questions

A guest binary needs (a) **exported functions** the compiler and the code call (API), and (b) **data layouts** the compiler writes into the Mach-O and that the runtime must read, and that *the guest also reads inline* (ABI).

### 2.1 Layout (ABI) — why libobjc2 cannot replace the runtime (VERIFIED)

| Structure | Apple (objc4-950) | GNUstep libobjc2 | ORVYN |
|---|---|---|---|
| `objc_class` | `struct objc_class : objc_object { Class superclass; cache_t cache; class_data_bits_t bits; }` (`runtime/objc-runtime-new.h:2635`) | `{ Class isa; Class super_class; const char *name; long version; unsigned long info; long instance_size; ... }` (`class.h:47`) | reads Apple's `class_t` (`isa, superclass, cache, vtable, data→class_ro_t`): `docs/OBJECTIVE-C-RUNTIME.md` §2 |
| `class_ro_t` | `objc-runtime-new.h:1598` | different (`objc_class` v2 / `objc_method_list`, selector tables) | flags@0, instanceStart@4, instanceSize@8, name@24, baseMethods@32, baseProtocols@40, ivars@48 |
| method entries | big (24 bytes) **or small relative** (`method_t::small`, flag `smallMethodListFlag = 0x80000000`, `objc-runtime-new.h:1251`, struct at `:975`) | its own | big only: **small lists refused** (`src/objc/objc_metadata.cpp:30`) |
| `isa` | `isa_t` union with non-pointer isa bits (`objc-private.h:97`) | plain pointer | plain pointer (inline `isa` reads by guest code assume a class pointer; non-pointer isa is *not* emulated) |
| selector refs | `__objc_selrefs` fixed up by dyld notification | typed selectors in a separate table | selrefs rewritten to unique SEL pointers |

Conclusion: a compiled-for-Apple binary has Apple's metadata baked into its `__DATA`; a runtime with a different `objc_class` layout cannot realize it without a translation layer. **Do not replace the runtime with libobjc2.** (`docs/OBJECTIVE-C-RUNTIME.md` §1 reached the same conclusion earlier; this research re-verified it in source.) libobjc2 remains a useful *reference* for algorithms (dtable caches, ARC fast paths, associated objects, exception handling on non-Apple platforms) under MIT.

### 2.2 The first blocker found in this research: relative method lists (VERIFIED by a run)

The fixture `objc_calc.m` was recompiled with the *same* flags except `--target=x86_64-apple-macos13` and linked with ld64.lld. `orvyn run` refuses it before running any guest code:

```
[ORVYN][ERROR] calc: Objective-C metadata: Calculator: relative method lists (macOS 11+ deployment targets) are not supported
```

So **any Objective-C code built for macOS 11 or later (every current Xcode default) is refused today**. This is a small, local, testable fix (parse `method_t::small`: 3 × `int32` relative offsets; `name` is an offset to a *selector reference*, not to a string —
`objc-runtime-new.h:975-999`) and is the cheapest large step toward real binaries. It is listed first in [TOP-20-COMPATIBILITY-IMPROVEMENTS.md](TOP-20-COMPATIBILITY-IMPROVEMENTS.md). The related bits (`bigSignedMethodListFlag`, pointer-authentication) apply to arm64e only.

## 3. API coverage (VERIFIED)

Method: every function-looking `OBJC_EXPORT` declaration in objc4-950's headers (`objc.h`, `runtime.h`, `message.h`, `objc-abi.h`, `objc-exception.h`, `objc-sync.h`, `objc-internal.h`) was extracted (334 names) and compared with the registry (`orvyn sdk-scan @implemented`: 82 `libobjc` functions).

| Header | Declared functions missing in ORVYN | Notes |
|---|---|---|
| public (`objc.h`, `runtime.h`, `message.h`, `objc-exception.h`, `objc-sync.h`) | **95** | below |
| `objc-abi.h` (compiler/ABI entry points) | 75 | e.g. `objc_msgSendSuper2_stret`, `objc_msgSend_fp2ret`, `objc_opt_self`, `_objc_setHook_*`, enumeration mutation |
| `objc-internal.h` (SPI) | 82 | mostly not needed by apps |

Missing public functions that real code uses (from the list; the application link evidence is in the right column):

| Function(s) | Used for | Seen in real imports |
|---|---|---|
| `objc_setAssociatedObject`, `objc_getAssociatedObject`, `objc_removeAssociatedObjects` | associated objects: category "stored properties", Foundation internals, many frameworks | (Foundation/Swift overlays) |
| `objc_sync_enter`, `objc_sync_exit` | `@synchronized` | very common in Objective-C |
| `objc_begin_catch`, `objc_end_catch`, `objc_exception_rethrow`, `objc_setUncaughtExceptionHandler`, `objc_addExceptionHandler`, `objc_terminate` | `@try/@catch/@finally` (needs the C++ ABI unwinder: see section 5) | — |
| `objc_msgSendSuper_stret`, `objc_msgSendSuper2_stret`, `objc_msgSend_fp2ret` | super calls returning structs; `long double`/complex returns | `_objc_msgSendSuper2_stret` in CotEditor's imports (VERIFIED) |
| `class_copyMethodList`, `class_copyIvarList`, `class_copyPropertyList`, `class_copyProtocolList`, `class_getProperty`, `class_addProperty`, `class_addProtocol`, `objc_getClassList`, `objc_copyClassList`, `method_getTypeEncoding` ✓/`method_copyReturnType`/`method_getNumberOfArguments`/`method_getArgumentType`, `method_invoke`, `property_getAttributes` family | reflection, KVC/KVO, `NSCoding`-style code, serialization | — |
| `imp_implementationWithBlock`, `imp_removeBlock`, `imp_getBlock` | blocks as methods (`NSObject` category helpers, test mocks) | — |
| `objc_allocateProtocol`, `objc_registerProtocol`, `protocol_*` (partial) | runtime protocols | — |
| `objc_setHook_getClass`, `_objc_realizeClassFromSwift` | Swift ↔ ObjC lazy classes | `_objc_setHook_getClass` in CotEditor (VERIFIED) |
| `_objc_empty_cache`, `_OBJC_CLASS_$_NSObject`/`_OBJC_METACLASS_$_NSObject` as *data* imports | the compiler references them for subclassing | handled as dynamic symbols (the coverage report lists them because it keys on the tbd) |

Behaviors present and tested (VERIFIED, `test_objc`, `test_objc_dynamic`, fixtures `objc_*`): class/metaclass registration, `+load`, categories, protocols (name-based conformance, requirements not checked), non-fragile ivar sliding, `objc_msgSend`, `_fpret`, `_stret`, `Super`, `Super2`, ARC entry points
(`objc_retain/release/autorelease*`, `storeStrong`, weak refs, `objc_alloc_init`, `objc_opt_*`), autorelease pools with the return-value optimization, `+resolveInstanceMethod:`, forwarding target, `-forwardInvocation:` with `NSMethodSignature`/`NSInvocation`, `@property` accessors (`objc_getProperty`/`setProperty*`), `objc_exception_throw` for **uncaught** exceptions, `NSException`.

## 4. ABI details that already hold (VERIFIED by fixtures) and the traps still ahead

* **Struct returns:** `objc_msgSend_stret` (hidden pointer in rdi, `self` in rsi) is implemented; `CGRect` getters in AppKit use it (`v_frame`, `s_frame`). `objc_msgSendSuper2_stret` is missing (above).
* **Floating point:** `objc_msgSend_fpret` implemented; `fp2ret` (x87 `long double` complex) missing, rare.
* **Variadic:** `NSLog`, `stringWithFormat:` read `%@` and varargs through `abi::ArgReader`; `AL` carries the vector register count (`GuestCallArgs::variadic`).
* **Callbacks host → guest:** nested guest calls from host code (`call_guest_nested`, depth ≤ 64) with the System V frame built by `GuestCallArgs` (ints, doubles, stack). Struct-by-value *arguments* to `NSInvocation` are **not** handled (documented limit).
* **Lifetime:** retain counts live in host tables (`live_`); `objc_live_objects` is the leak oracle used by the tests; weak references are cleared on dealloc.
* **Not emulated and potentially visible to guests:** non-pointer `isa`, tagged pointers (`NSNumber`/`NSString` tagged on Apple; ORVYN returns real objects — guest code that inspects the pointer bits would break), `objc_debug_*` data symbols, class `cache_t` (Apple code that reads `cache` is internal, HYPOTHESIS: rare in apps).

## 5. Exceptions and synchronization

* `@try/@catch` use the C++ Itanium-style unwinder on Darwin: `objc_begin_catch`, `__gxx_personality_v0`/`__objc_personality_v0`, `_Unwind_RaiseException`, `__unwind_info`/`__eh_frame`. ORVYN has none of this, and programs that use it are **refused at load time** (`test_objc_dynamic`, the `objc_try` fixture). Source for the semantics: `objc4-950 runtime/objc-exception.mm:349` (`objc_exception_throw`), `:400` (`objc_begin_catch`). Implementing `@try` means implementing the *unwinder* over `__unwind_info` and DWARF CFI for guest frames — a major piece shared with C++ exceptions and Swift errors that unwind through C++ frames (HYPOTHESIS for the Swift part). Not recommended before the loader has weak symbols and a C++ runtime.
* `@synchronized` → `objc_sync_enter/exit` (`objc-sync.mm:339`): easy on cooperative threads (recursive mutex keyed by object address). Missing today.
* KVO (`addObserver:forKeyPath:`) isa-swizzles a dynamic subclass (`NSKVONotifying_X`) and uses associated objects: needs `object_setClass` (✓ implemented), `class_createInstance`/dynamic class creation (✓), associated objects (✗) and Foundation's KVC (✗).
* Method swizzling (`method_exchangeImplementations` ✓, `class_replaceMethod` ✓): works on ORVYN's method lists; cache invalidation implemented (`flush_caches`).

## 6. Recommendations (feeds the top-20 list)

1. **Small (relative) method lists** — verified blocker; implement first.
2. `objc_sync_enter/exit`, associated objects, `objc_msgSendSuper2_stret`/`_fp2ret`, `class_copy*List`, `objc_getClassList`, `method_*` introspection, `imp_implementationWithBlock` — each is small, local, testable with a clang fixture.
3. Tagged-pointer policy: keep real objects (documented), but verify that `-[NSNumber numberWithInt:]` etc. never leak pointer-bit assumptions into the guest.
4. `@try/@catch`: separate project (unwinder), see sections 5 and the Swift/loader documents.
5. Use `libobjc2` (MIT) and `objc4` as *behavioral references* only; add an ABI regression test per new function (the exact-registry test already forces documentation).
