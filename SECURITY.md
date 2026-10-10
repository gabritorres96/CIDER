# Security policy

## Threat model

CIDER treats every macOS executable, bundle and disk image as **untrusted**.

| Property | 0.1 status |
| --- | --- |
| Static analysis never executes input code | yes — only `cider run` executes guest code, in a separate worker process |
| Bounds-checked, overflow-safe parsing (Mach-O, FAT, plist, UDIF) | yes, covered by malformed-input tests and deterministic mutation fuzzing |
| Resource limits (file size, load commands, symbols, plist depth/nodes, UDIF XML/chunks) | yes (see `Limits` structs) |
| DMG extraction path-traversal protection | yes: every listed path is validated before extraction, the extracted tree is re-verified afterwards (no links, nothing outside the output directory) |
| DMG extraction size / file-count limits | yes, checked on listings *before* extraction and on the result afterwards |
| External process hardening (7-Zip) | absolute path only, no stdin, output cap, timeout, killed with CIDER (Windows Job Object) |
| Process isolation for `cider run` (0.2) | worker process + Job Object (kill-on-close, 1 process, memory cap, UI limits, timeout): contains **crashes and runaway resource use** of the guest |
| Sandbox | **no** — guest machine code runs natively with the user's rights (raw NT syscalls, files, network). Never run untrusted binaries; none of the downloaded applications was ever executed |
| Cryptographic verification of code signatures | **no** — signatures are parsed, never verified |

Known limits: 7-Zip is a third-party parser of complex formats and is *not* sandboxed by CIDER
beyond the measures above; extract untrusted images on a machine you are prepared to treat
accordingly. Memory-safety tooling: since 0.1.1 the whole test-suite runs clean under AddressSanitizer (clang-cl)
and the parsers were fuzzed with libFuzzer (one crash found and fixed: `LC_SYMTAB` with `strsize == 0`);
see docs/TESTING.md and docs/REAL-WORLD.md. Fuzzing sessions were minutes long, not a long campaign.

## Reporting a vulnerability

Please report privately (do not open a public issue) to the repository owner with: affected
version/commit, a minimal reproducer (a generated fixture is ideal) and the observed impact.
You will get an acknowledgement and a fix or mitigation plan as soon as possible.
