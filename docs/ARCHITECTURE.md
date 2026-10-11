# Architecture

ORVYN is a set of small static libraries with one-way dependencies. Analysis modules never depend
on loader/runtime modules (which do not exist yet).

```
orvyn (CLI)
  └─ orvyn_analysis ── dependency closure + compatibility policy
       ├─ orvyn_bundle ── .app analysis (Info.plist, frameworks)
       │    ├─ orvyn_plist ── XML + binary property lists
       │    └─ orvyn_macho ── Mach-O / FAT parser, code-signature blob parser
       ├─ orvyn_dyld ── install-name classification and @rpath/@loader_path/@executable_path expansion
       └─ orvyn_core ── ByteView, Result/Error, JSON writer, logger, util, process runner
  └─ orvyn_dmg ── UDIF parser + optional 7-Zip backend (depends on orvyn_core, orvyn_plist)
```

| Library | Directory | Purpose |
| --- | --- | --- |
| `orvyn_core` | `src/core`, `src/logging` | bounds-checked `ByteView`, `Result<T>`, `JsonWriter`, logger, UTF-8/path helpers, `run_process` |
| `orvyn_macho` | `src/macho` | parser, validation, names, code-signature SuperBlob |
| `orvyn_plist` | `src/corefoundation` | plist reader (named after the Apple framework it mirrors) |
| `orvyn_dyld` | `src/dyld` | "orvyn-dyld": install-name expansion, rebase/bind/lazy-bind decoding, symbol resolution (pure, loads nothing) |
| `orvyn_bundle` | `src/bundle` | `.app` analysis |
| `orvyn_analysis` | `src/analysis` | dependency closure, compatibility verdict |
| `orvyn_dmg` | `src/dmg` | "orvyn-dmg": UDIF + 7-Zip |
| `orvyn_runtime` | `src/memory security loader abi libsystem process` | ORVYN 0.2 execution runtime (Windows x64 only) |
| `orvyn` | `src/cli` | `orvyn.exe` |

ORVYN 0.2 adds `orvyn_runtime` (Windows x64 only; `memory`, `security`, `loader`, `abi` incl. MASM, `libsystem`,
`process`) on top of `orvyn_dyld`'s fixup decoder:

```
orvyn (CLI: run + internal __worker)
  └─ orvyn_runtime ── guest process (in-process run, worker, supervisor), loader, memory, ABI bridge, libSystem subset, validator
       └─ orvyn_dyld (fixups: rebase/bind/lazy-bind decoding, symbol resolution) ── orvyn_macho ── orvyn_core
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
  `false`. Whether a program runs is only what `orvyn run` observes.
* **Strictness policy.** Structural violations (bad offsets, overlaps, truncation, malformed
  commands) fail the parse; oddities that do not endanger parsing (e.g. a broken code-signature
  blob, missing entry point in an executable) become `warnings`.
* **Execution is isolated from analysis.** Analysis commands never map or run anything; `orvyn run` is the only
  command that executes guest code, and it does so in a separate worker process.
* **Optional external tool.** 7-Zip, only for `extract`/`inspect` of
  disk images, located by absolute path.
