# Loader and cider-dyld

**Status: NOT IMPLEMENTED in 0.1.** This document is a design note for 0.2. Nothing here exists in
code except the path-level helpers in `cider_dyld` (`@rpath`/`@loader_path`/`@executable_path`
expansion and system-path classification), which only produce candidate file names.

## Plan for 0.2 (controlled fixtures first)

1. Validate the image with `macho::parse_file` (already done by the inspector); refuse anything
   that is not a thin x86_64 `MH_EXECUTE` built for macOS.
2. Reserve address space with `VirtualAlloc(MEM_RESERVE)` at the preferred base, commit and copy
   each segment, then set final page protections with `VirtualProtect`, never leaving a page both
   writable and executable (W^X): map writable, copy, then switch to read/execute.
3. `__PAGEZERO` is not mapped; the Windows low-address reservation already faults null accesses,
   but the 4 GiB hole assumed by macOS programs is not reproduced (see ABI.md).
4. Apply rebase/bind information (classic opcodes first; lazy binding resolved eagerly at start;
   chained fixups later), resolving symbols against CIDER-provided libSystem stubs.
5. Prepare the stack and the Darwin entry convention (`argc/argv/envp/apple[]`), run initialisers
   (`__mod_init_func`), then enter `LC_MAIN`'s entry through an ABI trampoline (System V → Win64
   at the boundary, and back for callbacks).
6. Exit code comes from `exit()` as implemented by CIDER's libSystem, not from the machine code
   returning into an uninitialised frame.

## Acceptance for 0.2 (from the roadmap)

Run a controlled x86_64 Mach-O test program and obtain its real exit code. Until then `cider run`
reports "not available".

## Open problems to investigate before coding

* `%gs`-relative TLS: macOS user code uses `gs` for thread-local storage and `pthread_self`;
  Windows x64 uses `gs` for the TEB. Options: thread-local-variable descriptor rewriting at bind
  time, or running Darwin code with a swapped GS base (requires a Windows driver or
  `WriteGsBase` where available) — not decided.
* Exception unwinding across Darwin frames (DWARF CFI in `__eh_frame`) vs Windows SEH.
* Which `dyld` private APIs a minimal program really touches (`_dyld_*`, `__tlv_bootstrap`).
