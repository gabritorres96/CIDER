# Limitations

## Technical

* **Nothing can be run.** No loader, dyld, libSystem, Objective-C runtime, Foundation or AppKit.
  `cider run` exits with code 2.
* Static analysis only: bind/rebase opcodes, chained fixups, export tries, unwind info and
  Objective-C metadata are not decoded; `LC_FUNCTION_STARTS` etc. are range-checked only.
* Targets x86_64 macOS binaries on Windows x86_64. arm64 slices are parsed and reported but judged
  "not compatible"; no ARM64 execution or translation exists. 32-bit and big-endian Mach-O are
  rejected.
* Code signatures are parsed, never verified. Entitlements are not decoded.
* System libraries are never resolved against anything: `/usr/lib` and `/System/Library` entries
  are reported as "system, not provided by CIDER yet".
* Whole executables are read into memory (limit 512 MiB). Disk images are streamed by 7-Zip.
* Dependency analysis follows the architecture of the main executable (x86_64 slice if present).
* `.app` support covers macOS bundles (`Contents/`); iOS-style flat bundles are rejected.
  Symlinks inside images are not recreated on Windows, so framework binaries are located through
  `Versions/*` instead of `Versions/Current`.
* Plist: OpenStep/ASCII format unsupported; integers above int64 and 128-bit binary integers are
  rejected.

## Validation scope (0.1.1)

Real-world validation covers 3 small open-source apps (universal x86_64+arm64, Swift/AppKit) and 9 LLVM-built
fixtures. It does not cover Apple-made software, Objective-C/Swift metadata, installers (`.pkg`), kernel
extensions or very large apps. See [REAL-WORLD.md](REAL-WORLD.md).

## Disk images

* Encrypted, sparse, sparse-bundle and multi-segment images are unsupported.
* Extraction requires an external 7-Zip. HFS+ was validated only through 7-Zip on three images; APFS and
  non-zlib compressions are **unverified** (see DMG.md). No universal DMG/HFS+/APFS support is claimed.
* Symbolic links in images are dropped (not recreated).
* Windows file-system restrictions apply to extracted names (case-insensitivity, reserved names,
  no symlinks/permissions/extended attributes/resource forks).

## Not implemented (by design, for later phases)

Compatibility database and profiles, launcher GUI, virtual filesystem, sandboxing (Job
Objects/AppContainer evaluated only on paper), Mach IPC, ARM64.

## Security

No sandbox. No claim of isolation. See SECURITY.md.

## Legal

CIDER does not redistribute or download Apple frameworks. Running macOS software may be subject to
Apple's licence terms (the macOS SLA restricts macOS software to Apple hardware in several
clauses); this project does not give legal advice — check the licence of what you run.
