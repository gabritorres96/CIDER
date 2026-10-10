# Blocks (CIDER 0.5.1)

A subset of Apple's Blocks ABI, implemented in `src/libsystem/blocks.cpp` against what clang emits for `x86_64-apple-macos`. It is **not** a port of a Windows BlocksRuntime and is
not claimed to be a complete Blocks implementation.

## What clang emits (verified with `llvm-objdump -d` on the fixtures)

* A block literal `{ isa, flags, reserved, invoke, descriptor, captures... }`. `isa` is bound to `__NSConcreteStackBlock` (stack blocks) or `__NSConcreteGlobalBlock`
  (blocks without captures, in `__DATA`); heap blocks get `__NSConcreteMallocBlock` from the runtime.
* The descriptor `{ reserved, size, [copy_helper, dispose_helper], [signature] }`; helpers exist when the block captures objects, blocks or `__block` variables
  (`flags & BLOCK_HAS_COPY_DISPOSE`, `1 << 25`).
* `__block` variables live in a `Block_byref { isa, forwarding, flags, size, [keep, destroy], value }`; every access goes through `forwarding`.
* Calls: `__Block_copy`, `__Block_release`, `__Block_object_assign`, `__Block_object_dispose` (Mach-O names; the C names are `_Block_copy`...). Under ARC also `objc_retainBlock`.

## Implementation

| Piece | Behaviour |
|---|---|
| `_Block_copy` | stack block: copy `descriptor->size` bytes to the guest heap, `isa` = `__NSMallocBlock__`, refcount 1 (flags `0xfffe` mask, increments of 2), then call the **copy helper in the guest** (nested callback) so captured objects are retained, captured blocks copied and `__block` variables moved to the heap. Heap block: count + 1. Global block: returned unchanged. NULL: NULL. |
| `_Block_release` | heap block: count - 1; at zero the **dispose helper** runs (guest), the memory is freed and `isa` zeroed. Global/stack: no-op. A pointer that is not a live heap block ends the guest (`_Block_release of 0x... which is not a heap block`). |
| `__Block_object_assign(dest, object, flags)` | `BLOCK_FIELD_IS_BYREF (8)`: copy the `__block` variable to the heap (two references: the block being copied and the frame, as Apple's runtime does) and point `forwarding` at it; `IS_BLOCK (7)`: copy; `IS_OBJECT (3)`: retain; `IS_WEAK (16)`/`BYREF_CALLER (128)`: plain assignment. |
| `__Block_object_dispose` | inverse of the above. |
| `NSBlock`, `__NSStackBlock__`, `__NSGlobalBlock__`, `__NSMallocBlock__` | native Objective-C classes. A block is an object: `[block copy]`, `retain`, `release`, `autorelease`, and `objc_retain/objc_release/objc_retainBlock` on a block go through the Blocks reference counts. Collections retain blocks like any object. |
| Data symbols | `__NSConcrete*Block` resolve to the matching class address (the `isa` value) through the runtime's dynamic symbol provider; they come from libSystem. |
| Runtime-invoked blocks | Foundation calls `invoke` of a block through a nested callback: `-[NSArray enumerateObjectsUsingBlock:]`, `indexOfObjectPassingTest:`, `-[NSDictionary enumerateKeysAndObjectsUsingBlock:]`, `NSNotificationCenter addObserverForName:...usingBlock:`, `dispatch_*`. The invoke pointer must lie in executable guest code (`is_valid_callback`). |

## Tests

`tests/runtime/test_blocks.cpp` + `tests/fixtures/objc/src/blocks_basic.m` (MRC): global block (copy returns itself), stack block with captured value, `Block_copy` of a stack/heap block,
release, `__block` variable shared between frame and heap copies, a counter returned from a function (the byref outlives the frame), captured `NSObject` (retain count 2 after copy, released when
the block dies, dealloc observed), block capturing a block, runtime enumeration with a nested enumeration inside the block and `*stop`, blocks stored in `NSMutableArray`.
`blocks_error.m`: double release, release of a non-block, copy of an unreadable pointer, NULL.

## Not implemented (do not rely on it)

Garbage-collected blocks, `_Block_use_RR2`, `Block_size`, block layout strings, C++ objects with constructors captured by blocks (`BLOCK_HAS_CTOR` is treated as an ordinary helper),
`BLOCK_IS_NOESCAPE` checking, `dispatch_block_t` creation APIs (`dispatch_block_create`...), blocks on threads other than guest threads. A block's `signature` is never interpreted.
