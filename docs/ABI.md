# ABI notes: System V AMD64 (macOS) vs Windows x64

Reference table of the ABI differences; the implemented part is described at the end of this file (CIDER 0.2).
Running x86_64 instructions natively does not make a macOS binary compatible: the ABI, the
libraries and the system calls all differ.

| Topic | macOS x86_64 (System V AMD64) | Windows x64 |
| --- | --- | --- |
| Integer/pointer args | `rdi, rsi, rdx, rcx, r8, r9` | `rcx, rdx, r8, r9` |
| FP args | `xmm0–xmm7` (independent of integer slots) | `xmm0–xmm3`, positional (shares slots with integer args) |
| Extra args | stack | stack, plus 32-byte caller-allocated shadow space |
| Return | `rax` (`rdx` second), `xmm0` (`xmm1`) | `rax`, `xmm0` |
| Callee-saved | `rbx, rbp, r12–r15` | `rbx, rbp, rdi, rsi, r12–r15, xmm6–xmm15` |
| Red zone | 128 bytes below `rsp` usable by leaf functions | none |
| Stack alignment at call | 16 bytes | 16 bytes |
| Struct return | ≤16 bytes classified into INTEGER/SSE eightbytes in registers; larger via hidden pointer in `rdi` | sizes 1/2/4/8 in `rax`, otherwise hidden pointer in `rcx` |
| Variadic calls | `al` = number of vector registers used | FP args also duplicated in integer registers |
| `long`, `wchar_t` | 64-bit, 32-bit (LP64) | 32-bit, 16-bit (LLP64) |
| TLS | `gs`-based; TLV descriptors via `__thread_vars` / `tlv_get_addr` | `gs` = TEB; implicit TLS directory |
| Exceptions | DWARF CFI (`__eh_frame`, `__unwind_info`) | table-based SEH (`.pdata`/`.xdata`) |
| Syscalls | `syscall` with class bits in `rax` (BSD class `0x2000000`); programs normally go through libSystem | NT system calls are private and unstable |
| Address space | `__PAGEZERO` makes the lower 4 GiB invalid; images at `0x100000000` | low 64 KiB reserved; lower 4 GiB usable by other allocations |

Consequences for CIDER (to design and test, not yet built):

* every call from Darwin code into CIDER-provided functions needs a System V → Win64 trampoline
  (move argument registers, create shadow space, preserve `rsi/rdi/xmm6–15`, align the stack);
  callbacks from Windows code into Darwin code need the reverse;
* variadic functions (`printf` family) need `al`-aware thunks;
* the `gs` conflict is the biggest open risk (see LOADER.md);
* ARM64 (future): code translation is a separate problem from API translation; options to evaluate:
  QEMU user-mode, LLVM-based dynamic binary translation. Windows ARM64 host + macOS ARM64 guest is
  the cheaper scenario (native execution; ABIs still differ: Apple deviates from AAPCS64 in
  variadic and `x18` handling).

## What CIDER 0.2 implements (src/abi, tests in test_abi / test_execution)

| Concern | Implementation | Verified by |
| --- | --- | --- |
| Entering System V code from Windows x64 | `cider_enter_guest` (MASM): saves rbx rbp rdi rsi r12-r15 xmm6-xmm15, switches rsp to the guest stack, loads rdi/rsi/rdx/rcx, `cld`, `call` | guest clobbers all of those registers; host values compared afterwards |
| Argument registers | System V integer arguments: rdi, rsi, rdx, rcx (entry) and rdi..r9 (thunks) | machine code with `imul` per register |
| Stack alignment | initial rsp 16-byte aligned before the `call`, so the callee sees rsp % 16 == 8 | guest returns `rsp & 15` == 8 |
| Red zone | guest runs on a host-allocated stack with room below rsp | guest leaf code writes 128 bytes below rsp |
| Calls guest → host | per-symbol stub (`mov r10, fn; jmp thunk`) → `cider_guest_thunk`: rdi,rsi,rdx,rcx → rcx,rdx,r8,r9; r8,r9 → Windows stack slots 5/6; 32-byte shadow space; runs on the **host** stack | 2-, 4- and 6-argument host functions called from guest code |
| Return values | `eax`/`rax` and `xmm0` pass through untouched | int, 64-bit, pointer and `double` results |
| `double` arguments | xmm0/xmm1 identical on both ABIs when they are the first/second *positional* argument | `sqrt`, `pow`, `floor`, ... called from a real Mach-O (fixture `fmath`) |
| `exit()` | host function sets `GuestCall::exited`; the thunk returns to the host epilogue instead of the guest | code after `exit` never runs |
| Unwinding | none: an exception inside guest code terminates the worker | `crash`, `recurse`, `ud2` tests |

## What CIDER 0.3 adds (tests: `test_abi_raw`, `test_darwin_runtime`, `test_tls`)

Three thunk kinds (`abi::ThunkSpec`), chosen per libSystem symbol:

| Kind | Entry | Use |
| --- | --- | --- |
| positional (`cider_guest_thunk`) | rdi,rsi,rdx,rcx → rcx,rdx,r8,r9; r8/r9 → stack slots | plain integer / all-double signatures (0.2 behaviour) |
| raw (`cider_guest_thunk_raw`) | host function receives `GuestRegs*`: rdi..r9, AL (rax), xmm0-7 (low qword), address of the stack-argument area; results go in `ret_rax/ret_rdx/ret_xmm0/ret_xmm1` | mixed int/double, >6 integer args, stack args, **variadics**, aggregate returns |
| TLV (`cider_guest_thunk_tlv`) | Darwin TLV convention: descriptor in rdi, result rax, **every other register preserved** | `tlv_get_addr` only |

* `RegsReader` walks the psABI: INTEGER class rdi..r9 then 8-byte stack slots, SSE class xmm0-7 then stack slots; both classes share the
  stack slots in call order (tested with 8 ints + 10 doubles). `VaListReader` reads a Darwin `va_list` in guest memory
  (`gp_offset`, `fp_offset`, `overflow_arg_area`, `reg_save_area`) and writes the cursor back like `va_arg`.
* AL (number of vector registers of a variadic call) reaches the callee through `GuestRegs::rax`.
* Aggregates: ≤16 bytes INTEGER → rax(:rdx), SSE → xmm0(:xmm1) (`div`, `ldiv`, tested with hand-assembled code and real clang code).
  Hidden-pointer (MEMORY class) returns work for raw functions that read `rdi` themselves; no libSystem function needs it yet.
* A positional thunk misplaces `f(int, double)`: the double is in xmm0 on System V, xmm1 on Windows (a test documents this).
* Guest stack pointer is never written by host code: host functions run on the host stack, so the 128-byte red zone below the guest
  `rsp` survives calls (tested). Stack arguments start 16-byte aligned like at a real call site (tested).
* callee-saved registers (rbx rbp r12-r15) survive raw thunks even when the host function is noisy (tested).

Still **not** handled: `long double` / x87 arguments, structures passed by value on the stack or in mixed classes as *arguments* to
host functions (only returns), callbacks host → guest (so no `qsort`/`atexit`), re-entrancy (one active guest call), SEH/DWARF
unwinding across the boundary, raw `syscall` instructions (Windows would interpret the number as an NT service). `gs`-relative TLS by
the guest itself is still unsupported; the standard Mach-O TLV protocol does not need it (see [TLS.md](TLS.md)).
