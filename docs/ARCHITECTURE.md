# Architecture

CIDER is a set of small static libraries with one-way dependencies. Analysis modules never depend
on loader/runtime modules (which do not exist yet).

```
cider (CLI)
  └─ cider_analysis ── dependency closure + compatibility policy
       ├─ cider_bundle ── .app analysis (Info.plist, frameworks)
       │    ├─ cider_plist ── XML + binary property lists
       │    └─ cider_macho ── Mach-O / FAT parser, code-signature blob parser
       ├─ cider_dyld ── install-name classification and @rpath/@loader_path/@executable_path expansion
       └─ cider_core ── ByteView, Result/Error, JSON writer, logger, util, process runner
  └─ cider_dmg ── UDIF parser + optional 7-Zip backend (depends on cider_core, cider_plist)
```

| Library | Directory | Purpose |
| --- | --- | --- |
| `cider_core` | `src/core`, `src/logging` | bounds-checked `ByteView`, `Result<T>`, `JsonWriter`, logger, UTF-8/path helpers, `run_process` |
| `cider_macho` | `src/macho` | parser, validation, names, code-signature SuperBlob |
| `cider_plist` | `src/corefoundation` | plist reader (named after the Apple framework it mirrors) |
| `cider_dyld` | `src/dyld` | "cider-dyld": install-name expansion, rebase/bind/lazy-bind decoding, symbol resolution (pure, loads nothing) |
| `cider_bundle` | `src/bundle` | `.app` analysis |
| `cider_analysis` | `src/analysis` | dependency closure, compatibility verdict |
| `cider_dmg` | `src/dmg` | "cider-dmg": UDIF + 7-Zip |
| `cider_runtime` | `src/memory security loader abi libsystem process` | CIDER 0.2 execution runtime (Windows x64 only) |
| `cider` | `src/cli` | `cider.exe` |

CIDER 0.2 adds `cider_runtime` (Windows x64 only; `memory`, `security`, `loader`, `abi` incl. MASM, `libsystem`,
`process`) on top of `cider_dyld`'s fixup decoder:

```
cider (CLI: run + internal __worker)
  └─ cider_runtime ── guest process (in-process run, worker, supervisor), loader, memory, ABI bridge, libSystem subset, validator
       └─ cider_dyld (fixups: rebase/bind/lazy-bind decoding, symbol resolution) ── cider_macho ── cider_core
```

Separation of stages: static analysis (macho) → validation (security) → layout (loader) → symbols (dyld) →
mapping (memory/loader) → execution context (abi) → transfer of control (abi MASM) → result (process).

Reserved, **empty** directories (README only): `darwin`, `objc`, `foundation`, `appkit`, `graphics`.
Interfaces are declared only when a test program needs them (no unimplemented declarations).

## Design rules

* **Parsers are independent of executors.** `macho::parse_file` returns plain data.
* **Errors are values** (`Result<T>`/`Status`), carrying a machine-readable `ErrorCode`.
* **Untrusted input.** All reads go through `ByteView`; arithmetic on file-controlled values uses
  `checked_add/checked_mul`; every table is range-checked against its container.
* **Honest verdicts.** The static `compatibility` command never claims execution: `CompatStatus::Executable` and
  friends exist in the enum but static analysis cannot produce them and `CompatReport::execution_confirmed` is always
  `false`. Whether a program runs is only what `cider run` observes.
* **Strictness policy.** Structural violations (bad offsets, overlaps, truncation, malformed
  commands) fail the parse; oddities that do not endanger parsing (e.g. a broken code-signature
  blob, missing entry point in an executable) become `warnings`.
* **Execution is isolated from analysis.** Analysis commands never map or run anything; `cider run` is the only
  command that executes guest code, and it does so in a separate worker process.
* **Optional external tool.** 7-Zip, only for `extract`/`inspect` of
  disk images, located by absolute path.
