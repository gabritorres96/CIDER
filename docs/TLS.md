# Thread-local storage (CIDER 0.3.6)

## Why Windows and Darwin TLS collide

On Windows x64 `gs` points at the TEB; `gs:[0x58]` is `ThreadLocalStoragePointer`, `gs:[0x1480]`… hold TLS slots/FLS. Overwriting
`gs` (or the TEB) to emulate Darwin would corrupt the host's thread state (exceptions, stack limits, CRT). **CIDER never writes
`gs`, the TEB, TlsAlloc slots or host `thread_local` data.**

## What clang actually emits on Darwin x86_64

```
movq  _x@TLVP(%rip), %rdi     # or  leaq _x(%rip), %rdi  for variables of the same image
callq *(%rdi)                 # descriptor->thunk(descriptor)  ->  rax = &x
```

The guest does **not** touch `gs`: only dyld's `tlv_get_addr` does. Mach-O pieces (verified with `llvm-objdump`, fixture `tlsexe`):

| Section | Type | Role |
| --- | --- | --- |
| `__DATA,__thread_vars` | `S_THREAD_LOCAL_VARIABLES` (0x13) | 24-byte descriptors `{thunk, key, offset}`; `thunk` is bound to `__tlv_bootstrap`; `offset` = offset in the template |
| `__DATA,__thread_data` | `S_THREAD_LOCAL_REGULAR` (0x11) | initial values (template, may contain rebased pointers) |
| `__DATA,__thread_bss` | `S_THREAD_LOCAL_ZEROFILL` (0x12) | zero-initialised part of the template |
| `__DATA_CONST,__thread_ptrs` | `S_THREAD_LOCAL_VARIABLE_POINTERS` (0x14) | pointers to *imported* descriptors (cross-image TLS) |
| `__thread_init` | `S_THREAD_LOCAL_INIT_FUNCTION_POINTERS` (0x15) | C++ dynamic TLS initialisers — **refused** |

## Design (implemented)

`__tlv_bootstrap` / `_tlv_get_addr` resolve to a CIDER host function (`src/libsystem/tls.cpp`). On first use per image it
allocates a block in the guest heap (template size + alignment), copies the `__thread_data` bytes, leaves `__thread_bss` zero and
returns `block + descriptor->offset`. There is one guest thread, so one block per image.

Darwin's TLV convention preserves **every register except `rax`** across the call (compilers keep `edx`, `rcx`, xmm values live).
A normal Windows-ABI thunk would clobber them (this was a real bug found while building the fixture: the guest read the wrong
value). `cider_guest_thunk_tlv` therefore saves/restores `rcx rdx rsi rdi r8-r10 xmm0-5` around the host call.

At link time the linker validates the template (size ≤ 16 MiB, alignment ≤ 4096, inside the mapping), that descriptor tables are whole
24-byte records, that every `thunk` equals `__tlv_bootstrap` and every `offset` lies inside the template.

## Limits / refused

Since 0.4.2 every guest thread has its own TLS blocks (created lazily from the template, see [THREADING.md](THREADING.md)); no thread destructors (`_tlv_atexit`); `S_THREAD_LOCAL_INIT_FUNCTION_POINTERS`
(0x15) refused; alignment > 4096 refused; arm64 TLS not applicable. Tests: `tests/runtime/test_tls.cpp` (TEB/`gs`/TlsAlloc/`thread_local`
identical before and after a guest run; register preservation; template layout vs `llvm-objdump`).

## 0.4.2: per-thread blocks

`_tlv_get_addr` now keys the block on the **calling guest thread** (`ThreadState::tls_blocks`, one block per TLS image, created on first use from `__thread_data`/`__thread_bss`
in the thread's own guest memory). Each new thread starts from the template, not from the creator's values (`thread_tls` fixture). Still never written: `gs`, the TEB,
`TlsAlloc`/FLS slots and host `thread_local` data; `test_threads` checks that the host's thread state is identical before and after a multithreaded guest run.
Not implemented: `_tlv_atexit` destructors, `pthread_key_create` destructors at thread exit.
