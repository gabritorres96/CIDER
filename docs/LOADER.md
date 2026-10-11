# Loader and orvyn-dyld

**Status (ORVYN 0.2): implemented for controlled x86_64 executables.** Sources: `src/loader`, `src/memory`,
`src/security`, `src/dyld`. Public API: `include/orvyn/loader.hpp`, `memory.hpp`, `fixups.hpp`.
How a whole run fits together: [EXECUTION.md](EXECUTION.md).

## Pipeline (`loader::load_macho`)

Strict separation between stages; nothing before step 5 touches process memory.

1. **Static analysis** — `macho::parse_file` (0.1 parser); picks the x86_64 slice of a FAT file.
2. **Validation** (`security/execution_validator.cpp`) — x86_64, `MH_EXECUTE`, macOS platform, not encrypted, has
   `LC_MAIN` (not `LC_UNIXTHREAD`), no chained fixups, no weak-binding info, no TLS sections, no non-empty
   `__mod_init_func`/`__init_offsets`/terminators, every non-weak dylib is provided (only `libSystem.B.dylib`).
   Issues are classified `Incompatible`, `Unsupported` or `MissingDependency`.
3. **Layout** (`loader/image_layout.cpp`, pure computation) — unmapped: `__PAGEZERO`, `__LINKEDIT`, empty segments.
   Per mapped segment: page-aligned address, size rounded up (overflow-checked), `filesize ≤ vmsize`, W^X-safe
   protection, no overlap after rounding, image span ≤ 1 GiB, base 64 KiB aligned (Windows reservation
   granularity), entry point inside an executable mapped segment, stack size from `LC_MAIN` (default 8 MiB, max 64 MiB).
4. **Fixups and symbols** (`dyld/relocation_engine.cpp`, `dyld/symbol_resolver.cpp`) — decode rebase, bind and
   lazy-bind opcode streams; resolve every bind **before** mapping so unresolved symbols fail with no side effects.
5. **Mapping** (`loader/segment_mapper.cpp`) — `VirtualAlloc(MEM_RESERVE)` of the span at the preferred base
   (`0x100000000`), falling back to any 64 KiB-aligned address (the slide is applied by the rebases); commit every
   segment ReadWrite, copy file bytes, zero the rest.
6. **Fixups applied** — rebases add the slide, binds write System V thunk addresses (+ addend).
7. **Final protections** — `__TEXT` r-x (instruction cache flushed), `__DATA` rw-, `__DATA_CONST` r--. Never RWX.

Any failure after step 5 releases the reservation (RAII `memory::Region`); tests verify the address range is free again.

## Supported fixups (orvyn-dyld, `LC_DYLD_INFO_ONLY`)

| Mechanism | Support |
| --- | --- |
| Rebase: pointer type, all 8 opcodes | yes (verified against `llvm-objdump --rebase` on 20 binaries) |
| Bind: pointer type, ordinals imm/ULEB, special ordinals (-1 main exe, -2 flat), symbol flags (weak import), addends, all `DO_BIND*` forms | yes (verified against `--bind`) |
| Lazy bind stream | decoded and **applied eagerly** (verified against `--lazy-bind`); `dyld_stub_binder` traps |
| Weak imports | bound to 0 with a warning |
| Binding to the image itself (ordinal 0) or to the main executable (-1) | rejected ("not supported yet") |
| Bind types other than pointer, threaded binds, weak-binding streams | rejected |
| Chained fixups (`LC_DYLD_CHAINED_FIXUPS`) | **rejected** with a clear message; link with `-no_fixup_chains` |
| Re-exported libraries, two-level namespace semantics beyond "the ordinal names the library" | not implemented |

Symbol resolution consults a `SymbolProvider`; the only provider is ORVYN's libSystem subset. A symbol of
`libSystem.B.dylib` that ORVYN does not implement is *unresolved*, never mapped to a same-named Windows function.

## Not done (needed for real applications)

Loading dependent dylibs/frameworks (`@rpath`), `__mod_init_func` and `dyld` initialization order, TLS,
`gs`-based thread-local storage (the main open risk, see ABI.md), Objective-C metadata registration, exception
unwind tables, `__PAGEZERO` emulation, ASLR policy beyond "preferred base or relocate", ARM64.
