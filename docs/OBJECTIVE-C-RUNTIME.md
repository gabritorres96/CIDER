# Objective-C runtime (ORVYN 0.4)

ORVYN executes Objective-C code from Mach-O x86_64 files compiled for Apple's **objc2 ("non-fragile") ABI**. The runtime is written for
ORVYN (`src/objc/`); it is not Apple's libobjc and does not contain any of its code. This document records the research behind that
decision, the data structures that are read, the dispatch design, and what is **not** supported.

Scope: controlled fixtures compiled with clang and linked with ld64.lld. No Apple application has been run. ORVYN makes no claim of
general Objective-C, Cocoa or macOS compatibility.

## 1. Options evaluated

| Option | Verdict |
|---|---|
| Apple libobjc (objc4) | Not available to redistribute with ORVYN (Apple Public Source License); the binary lives only in the macOS dyld shared cache. Not used, not copied. |
| GNUstep libobjc2 | MIT-licensed, but it is **not Apple-ABI-equivalent**: different class/method/ivar metadata (`objc_class` v2 with `dtable`, `objc_method_list`), different selector representation (typed selectors, selector tables in a separate section), different `objc_msgSend` family (`objc_msgSend_slow`, `objc_slot_lookup`), different property and protocol structures and different ARC entry points. Compiler output for the Apple ABI (`__objc_classlist`, `class_ro_t`, `method_list_t` with `entsize`/flags, `__objc_selrefs`) cannot be consumed by it without a translating layer. GNUstep-base (Foundation) is LGPL and depends on that runtime. Not used. |
| Own runtime reading the Apple metadata | **Chosen.** The metadata layout is public through the open-source objc4 headers' documented behaviour and through what `llvm-objdump --objc-meta-data` prints; ORVYN's parser is checked against that tool. |

Only layouts that clang emits for `x86_64-apple-macos` are implemented. Every offset below was verified against fixtures compiled by
the bundled LLVM 23.1.3 and against the `llvm-objdump --objc-meta-data` snapshots in `tests/fixtures/objc/golden`.

## 2. Metadata that is read

Sections of `__DATA` / `__DATA_CONST` / `__TEXT`: `__objc_classlist`, `__objc_nlclslist` (classes with `+load`), `__objc_catlist`,
`__objc_nlcatlist`, `__objc_protolist`, `__objc_selrefs`, `__objc_classrefs`, `__objc_imageinfo`, `__objc_methname`, `__objc_methtype`,
`__objc_classname`.

* `class_t`: `isa`, `superclass`, `cache`, `vtable`, `data` (`class_ro_t*`).
* `class_ro_t` (offsets): flags@0, instanceStart@4, instanceSize@8, name@24, baseMethods@32, baseProtocols@40, ivars@48.
* `method_list_t`: `entsize` + count, entries of 24 bytes (`name`, `types`, `imp`). **Relative method lists (small methods) are refused** with
  a diagnosis; clang emits them only for newer deployment targets (macOS 11+ with `-fobjc-... ` defaults for arm64), not for the
  `macos10.15` target the fixtures use.
* `ivar_list_t`: entries of 32 bytes (`offset` variable pointer, `name`, `type`, `alignment`, `size`).
* `category_t`, `protocol_t` (name, parents), and the protocol handle that `@protocol(X)` loads (`_OBJC_PROTOCOL_$_X`).
* Selector references (`__objc_selrefs`) are rewritten to unique SEL pointers owned by the runtime.

Every pointer is range-checked (`readable`) before it is dereferenced; counts and sizes are bounded; a malformed image produces a
linker error, never a crash (hostile-metadata matrix in `tests/runtime/test_objc.cpp`, plus `fuzz_objc`).

## 3. Classes, metaclasses and layout

* Each class has a metaclass; the root metaclass' superclass is the root class, as in Apple's runtime. `+class` methods are found by
  walking the metaclass chain; the root class' instance methods are the last fallback for class messages.
* Non-fragile instance variables: the compiler emits one 32-bit offset variable per ivar (`OBJC_IVAR_$_Class.ivar`). When a superclass
  from another image is larger than at compile time, the runtime **slides** the subclass' ivars and rewrites the offset variables
  and the instance size (`slide` fixture, dylib built against a newer base class).
* Categories are attached after all classes are realized; `+load` runs superclass first, then class, then categories, once.
* Run-time class construction: `objc_allocateClassPair`, `class_addMethod`, `class_addIvar` (before registration), `objc_registerClassPair`,
  `objc_disposeClassPair`.
* Protocols: conformance is name-based and transitive (`class_conformsToProtocol`, `-conformsToProtocol:`); method requirements are not checked.

## 4. Message dispatch

`objc_msgSend` and its variants are **MASM tail-jump stubs** (`src/objc/objc_msgsend.asm`) installed as ordinary libobjc exports:

1. The stub saves the argument registers, calls the host lookup function (`Runtime::lookup_for_message`) with `(receiver, SEL)` and
   restores the registers.
2. `nil` receiver: the message returns zero in `rax`, `rdx`, `xmm0`, `xmm1` (the `nil_return_imp`).
3. Otherwise it jumps to the IMP with the original registers untouched, so any argument/return class (integer, `double`, `float`,
   structs in registers, varargs, stack arguments) works without per-signature code.
4. `objc_msgSend_stret` (hidden return pointer), `objc_msgSend_fpret`, `objc_msgSendSuper` and `objc_msgSendSuper2` follow the same scheme.
5. Lookup walks the class, then its superclasses (cache per class, flushed when methods are added). A missing method resolves to the
   **forwarding trap IMP**: the guest ends with `-[Class selector]: unrecognized selector sent to instance 0x…` (or `class`).
   `forwardInvocation:`, `resolveInstanceMethod:` and `NSInvocation` are **not** implemented.

Native (host-implemented) methods of the Foundation subset use the same mechanism: each gets a thunk that converts System V to the
Windows ABI (or receives raw `GuestRegs`), so guest code cannot tell native and compiled methods apart. See [FOUNDATION.md](FOUNDATION.md).

## 5. Objects and memory management

* Instances are allocated in guest-visible memory (`class_createInstance`, `+alloc`, `objc_alloc`); the first word is `isa`.
* A live-object table tracks retain count, deallocating state and weak references. Objects inside a loaded image or the runtime arena
  (constant strings, static objects) are **immortal**: retain/release are no-ops.
* Retain/release/autorelease, autorelease pools (`objc_autoreleasePoolPush/Pop`, per guest thread), `-dealloc` (dispatched like any message),
  `.cxx_destruct` for ARC ivars (run from `object_dispose` / `objc_destructInstance`), `objc_storeWeak` family (weak slots zeroed at dealloc),
  `objc_storeStrong`, `objc_retainAutoreleasedReturnValue` & friends, `objc_alloc`, `objc_alloc_init`, `objc_opt_*`.
* An object whose `-dealloc` forgets `[super dealloc]` is freed by the runtime after the method returns. Autorelease without a pool logs a
  warning and leaks the object. Double release, retain of freed memory and invalid pool tokens end the guest with a diagnosis.
* At exit the runtime reports the number of objects still alive (`objc_live_objects` in the result and JSON) — it is a leak counter, not a
  failure.

## 6. Exported libobjc surface

80 symbols (`tests/runtime/test_runtime.cpp` pins the list): the message-send family, class/selector/method/ivar/protocol reflection, run-time
class construction, the memory-management and ARC helpers above, `objc_getProperty`/`objc_setProperty*` (used by compiler-generated accessors),
`objc_enumerationMutation` and `_objc_empty_cache`. Any other `_objc_*` import fails the link with the symbol name (exit code 12).

## 7. Not supported

* **Blocks** (`^{}`), `libdispatch`, `@try/@catch/@throw` and `objc_exception_*` (use `-fno-objc-exceptions`), `@synchronized`.
* Relative (small) method lists, Swift classes, class extensions with property synthesis through metadata introspection (`class_copyPropertyList`
  and friends), `objc_msgSend` message forwarding, `+resolveInstanceMethod:`, KVO/KVC, `NSInvocation`, `objc_copyClassList`.
* Method type encodings are stored, not validated against the call.
* The runtime is single-process and cooperative with ORVYN's guest threads (see [THREADING.md](THREADING.md)); it has no ThreadSanitizer coverage.
* The log line `[ORVYN][OBJC] Message dispatched: …` is printed once per distinct class+selector pair, not on every send.

## 8. Tests

`tests/runtime/test_objc.cpp` (16 tests): parser versus `llvm-objdump`, calc program returning 42, inheritance, run-time classes, categories +
`+load` ordering + protocols, ABI return classes (`double`, `float`, struct in registers, `stret`, varargs, stack arguments, `nil`), ivar
sliding, MRC, ARC with weak references and pools, error traps, hostile metadata. `tests/runtime/test_foundation.cpp` covers the Foundation
subset. Rebuild the fixtures with `tools\llvm-env.cmd tools\gen-objc-fixtures.cmd`.

## 0.5 additions

* **Message forwarding** (0.5.4): `+resolveInstanceMethod:` / `+resolveClassMethod:` (may call `class_addMethod`), `-forwardingTargetForSelector:` (the call is replayed on the new receiver with the original
  registers and stack arguments), `-methodSignatureForSelector:` + `-forwardInvocation:` with `NSMethodSignature` and `NSInvocation` (`getArgument:atIndex:`, `setArgument:atIndex:`, `getReturnValue:`,
  `setReturnValue:`, `invoke`, `invokeWithTarget:`, `target`, `selector`). The "no method" IMP is a raw thunk (`objc_forward_message`) that receives every argument register. Supported types: integers, BOOL, pointers,
  objects, classes, selectors, `float`, `double`, `void`. **Structures/unions/arrays/long double** (by value) are refused with a diagnosis; arguments passed on the stack are supported only on the forwarding-target path.
  If nothing handles the message the old trap remains (`-[Class sel]: unrecognized selector sent to instance 0x...`). Tests: `objc_forward.m`.
* **Exceptions: limited and honest.** `NSException` (create, `raise`, `raise:format:`, `name/reason/userInfo`) and `objc_exception_throw` exist and end the guest with the macOS text for an *uncaught* exception
  (`*** Terminating app due to uncaught exception 'Name', reason: 'Reason'`). `@try/@catch/@finally` are **not supported**: they need the Darwin unwinder (`__Unwind_RaiseException`, `__objc_personality_v0`, DWARF
  call-frame information of every guest and host frame in between) and unwinding through the host frames of the ABI bridge cannot be verified. A binary compiled with Objective-C exceptions imports weak EH-type bindings
  and personality symbols and is **refused at load time** (`orvyn run` exit code 11, "weak binding information is not supported yet") rather than run with a broken catch. Compile guest code with `-fno-objc-exceptions`.
* Blocks are Objective-C objects (`NSBlock`, [BLOCKS.md](BLOCKS.md)); `objc_retainBlock` is exported; the exported libobjc surface is now 82 functions.
* Compiler `@protocol(X)` handles are recognised by `conformsToProtocol:` / `class_conformsToProtocol`.
