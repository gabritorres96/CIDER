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
| `cider_dyld` | `src/dyld` | path-level part of "cider-dyld"; loads nothing |
| `cider_bundle` | `src/bundle` | `.app` analysis |
| `cider_analysis` | `src/analysis` | dependency closure, compatibility verdict |
| `cider_dmg` | `src/dmg` | "cider-dmg": UDIF + 7-Zip |
| `cider` | `src/cli` | `cider.exe` |

Reserved, **empty** directories (README only): `loader`, `memory`, `abi`, `darwin`, `libsystem`,
`objc`, `foundation`, `appkit`, `graphics`, `process`, `security`. Interfaces are declared only
when a test program needs them (no unimplemented declarations).

## Design rules

* **Parsers are independent of executors.** `macho::parse_file` returns plain data.
* **Errors are values** (`Result<T>`/`Status`), carrying a machine-readable `ErrorCode`.
* **Untrusted input.** All reads go through `ByteView`; arithmetic on file-controlled values uses
  `checked_add/checked_mul`; every table is range-checked against its container.
* **Honest verdicts.** `CompatStatus::Executable` and friends exist in the enum but nothing in
  0.1 can produce them; `CompatReport::execution_confirmed` is always `false`.
* **Strictness policy.** Structural violations (bad offsets, overlaps, truncation, malformed
  commands) fail the parse; oddities that do not endanger parsing (e.g. a broken code-signature
  blob, missing entry point in an executable) become `warnings`.
* **Optional external tool.** The only external process is 7-Zip, only for `extract`/`inspect` of
  disk images, located by absolute path.
