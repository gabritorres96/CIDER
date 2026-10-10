# ABI notes: System V AMD64 (macOS) vs Windows x64

Research notes for the loader/trampolines (0.2). **No ABI adaptation is implemented in 0.1.**
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
