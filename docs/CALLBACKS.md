# Host → guest callbacks (CIDER 0.4.0)

Until 0.3 control only went one way: guest code called host (Windows) functions. Since 0.4.0 a host function can call **back into guest machine code**
and return its result, which `qsort`, `bsearch`, `atexit`, the Objective-C runtime (`+load`, `-dealloc`, `-description`, `-isEqual:`…) and the Foundation
subset need.

## Mechanism (`src/abi/callback_bridge.cpp`, `src/abi/entry_trampoline.asm`)

1. `abi::call_guest_nested(target, GuestCallArgs)` is the only way to enter guest code from a host function. It builds a `GuestCall` record (200 bytes, layout
   mirrored in assembly and pinned by `test_abi`) holding the callee-saved registers of the interrupted frame, the System V argument registers
   (6 integer + 8 SSE), stack arguments and the return registers (`rax`, `rdx`, `xmm0`, `xmm1`).
2. The callback runs **on the same guest stack**, below the interrupted frame **and below its 128-byte red zone**, which the System V ABI lets a leaf function use
   without adjusting `rsp` (test: `the_interrupted_frame_red_zone_survives_a_callback_that_uses_a_lot_of_stack`). The stack is re-aligned to 16 bytes at the call.
3. Arguments may be integers, pointers, `double`s, mixed, more than six integers (spilled to the guest stack), and results may be one or two registers
   (aggregates in `rax:rdx` / `xmm0:xmm1`). Hidden-pointer returns are the caller's business (the pointer is just an integer argument).
4. A callee that calls `exit()` (or traps) *unwinds every host frame between it and the entry point*: the outcome carries `exited`/`refused`, the host function
   stops what it was doing and returns, and the exit propagates. `atexit` handlers still run (except after `_exit`).
5. Callbacks can nest guest → host → guest → host → guest up to `kMaxCallbackDepth` = **64**. Beyond that the call is *refused*, the host function receives
   `refused` and the guest is stopped with `callback nesting deeper than 64`; the stack keeps a 64 KiB safety margin before the guard page.

## Validation of callback targets

A function pointer received from the guest is entered only if `is_valid_callback` accepts it: non-null and inside the executable range of a loaded image
(`RuntimeContext::code_ranges`, filled by the linker per `__TEXT` section; the runtime's own native-method thunks are added for Objective-C). Anything else
(stack, heap, data, an address in the middle of nowhere, a host address) is refused **before** any guest code runs, with a diagnosis such as
`qsort: comparator is not executable guest code`. The check is a plausibility filter, not control-flow integrity: a valid-range address that is not a function
start is still entered.

## libSystem users

| Function | Notes |
|---|---|
| `qsort(base, n, size, cmp)` | Stable **merge sort** over an index array, then one permutation copy, so a comparator can never see a half-moved array. Limits: `n` ≤ 2^26 elements and ≤ 256 MiB, else `qsort: array too large`. `cmp` is called as `int cmp(const void*, const void*)` with pointers into the *original* array. A comparator may itself call `qsort`; `exit()` inside the comparator stops the sort and runs the `atexit` handlers. |
| `bsearch(key, base, n, size, cmp)` | Standard binary search; `key` is passed as the first argument. |
| `atexit(fn)` | Up to 256 handlers (`ENOMEM` beyond), run in reverse order when `main` returns or `exit()` is called; `exit()` inside a handler replaces the exit code; `_exit` skips them; an invalid pointer returns `EINVAL`. |
| Objective-C runtime | `+load`, `-dealloc`, `.cxx_destruct`, `-description` from `%@`, `-hash`/`-isEqual:` from collections use the same nested calls (`Runtime::call_imp`, `Runtime::send`). |

## Limits

* One guest at a time per worker process; callbacks share the calling thread's stack (see [THREADING.md](THREADING.md) for other guest threads).
* A callback that never returns blocks the host function that called it; the worker timeout is the only remedy.
* No signal handlers and no `setjmp/longjmp` across a callback (not implemented at all).
* Callbacks into **Windows** code are never possible: only addresses that belong to the guest's Mach-O images or the runtime's own thunks are accepted.

## Tests

`tests/runtime/test_callbacks.cpp` (18 tests): integer, `double`, mixed and eight-argument callbacks, two-register aggregate result, nesting guest→host→guest→host→guest,
the depth limit with the guest surviving the refusal, invalid targets, `exit()` inside nested callbacks, red-zone survival, `qsort` of ints/strings/records and
nested `qsort`, `bsearch`, `atexit` order, `exit()` in a handler, `_exit`, `exit()` in a comparator, and an invalid comparator.
