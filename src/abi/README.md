# abi

System V AMD64 <-> Windows x64 bridge: `entry_trampoline.asm` (MASM: guest entry, host epilogue, guest->host thunk), `sysv_bridge.cpp` (per-symbol stubs), `stack_builder.cpp` (guest stack, argv/envp/apple). See docs/ABI.md.
