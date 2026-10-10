# Real-world validation (CIDER 0.1.1)

Date: 2026-10-10. Host: Windows 11 Pro 10.0.26200, x86_64. Everything below was actually run; commands and
numbers are reproducible with the scripts named in each section. **Nothing downloaded was executed.**

## 1. Mach-O produced by LLVM (not by CIDER's own fixture builder)

### Toolchain

| Item | Value |
| --- | --- |
| LLVM / clang / lld | 23.1.3, portable `clang+llvm-23.1.3-x86_64-pc-windows-msvc.tar.zst` from the official `llvm/llvm-project` GitHub release, unpacked into `.toolchain/` (git-ignored, no global install) |
| Archive SHA-256 | `7e69dc07a4536b79ff24a651a2611990bf8eaab17ea560f1092707c4f31ba01d`, equal to the digest in the release's Sigstore attestation (`.jsonl`). The attestation signature itself was **not** verified with `cosign`. |
| Cross toolchain | `clang --target=x86_64-apple-macos10.15` / `arm64-apple-macos11` + `ld64.lld` (Mach-O flavour of lld) + `llvm-lipo` |
| libSystem | **hand-written stub** `tests/fixtures/real/sysroot/usr/lib/libSystem.tbd` (declares `_puts _exit _write _getpid dyld_stub_binder`). No Apple SDK, header or `.tbd` is used. Sources are freestanding C that declares its own libc prototypes. |
| Independent reference | `llvm-objdump --macho --private-headers / --syms / --universal-headers` (same LLVM build) |

Reproduce: `tools\llvm-env.cmd tools\gen-real-fixtures.cmd` (rebuilds `tests/fixtures/real/bin` and the
golden objdump snapshots in `tests/fixtures/real/golden`; relative paths only, so no user path ends up in
the binaries or in the ad-hoc signature identifiers).

### Fixtures (committed, ~200 KiB total)

`hello_x86_64`, `hello_arm64`, `hello_universal` (fat x86_64+arm64), `hello_signed_x86_64` (ad-hoc signature),
`hello_chained_x86_64` (`LC_DYLD_CHAINED_FIXUPS`), `libfoo.dylib`, `libweak.dylib`, `app_x86_64` (load + weak
dylib, two `LC_RPATH`), `hello_x86_64.o` (`MH_OBJECT`).

### Results

`tests/regression/test_real_macho.cpp` compares, field by field, what CIDER parses with what llvm-objdump
prints: `ncmds`/`sizeofcmds`, every load command id, all segments (name, vmaddr, vmsize, fileoff, filesize,
protections, section count), all sections (name, segment, addr, size, offset), dylibs (kind, path, versions),
rpaths, dylinker, `LC_MAIN` entryoff/stacksize, UUID, build version (platform/minos/sdk/tool count), symtab,
dysymtab, dyld-info, chained-fixups / exports-trie / function-starts / code-signature ranges, the whole
symbol table (name, value, undefined, debug, external) and the fat header (offset, size, alignment).
**All 9 fixtures agree.** The committed goldens make this check run without LLVM; when llvm-objdump is
available a live test additionally asserts that its current output still equals the goldens.

## 2. Real DMG images (official open-source releases)

Downloaded over HTTPS from the projects' GitHub release pages into `.validation/dmg` (git-ignored) by
`tools/validate-real-dmgs.ps1`:

| Image | Release (published) | Bytes | SHA-256 |
| --- | --- | --- | --- |
| `Rectangle2.0.3.dmg` | rxhanson/Rectangle v2.0.3 (2026-10-05) | 4 730 793 | `a48883b56fbe3fbbd699a05bc8ddbfce65c0c73c66f71eaebbe9f1fdc5a3dcb5` |
| `Stats.dmg` | exelban/stats v3.0.20 (2026-10-04) | 7 644 329 | `ad48e0cd2dd0c9d8873831fa0285be09e19017885a54b57509c4768573f9dfbe` |
| `MonitorControl.4.4.0.dmg` | MonitorControl/MonitorControl v4.4.0 (2026-09-15) | 20 391 423 | `e058da598b92f6ca674f4398c0b6fa49b3b5567ccfddccf6c6a5b23c5c625612` |

(The projects' licences should be checked by whoever redistributes them; CIDER does not redistribute them.)

### UDIF structure (parsed by CIDER, no external tool)

All three: UDIF version 4, flattened, **8 partitions** — `Protective MBR (MBR : 0)`, `GPT Header`, `GPT Partition
Data`, `(Apple_Free : 3)`, **`disk image (Apple_HFS : 4)`**, `(Apple_Free : 5)`, backup GPT table/header. Chunk
types seen: **zlib, raw, ignore/free** only. Sector counts: 71 758 / 88 139 / 185 344 (the partition table
adds up to the trailer's total, asserted by `real_world_dmgs_are_parsed_and_extracted`). Cross-check with
7-Zip's own reading of Rectangle: `unpack-size 36740096` = 71 758 × 512 ✓, `Method = Copy Zero2 ZLIB`
= raw / ignore / zlib ✓.

The filesystem is reported as **"HFS+ (inferred from partition name)"**: the HFS+ partition is compressed, so
CIDER cannot read its signature. 7-Zip names the extracted partition `4.hfs` with type `HFS`, which
independently confirms HFS+/HFSX for these three images (not APFS).

### Extraction through 7-Zip 25.01

| Image | Partitions extracted | Files | Unpacked | `.app` found | Links not recreated |
| --- | --- | --- | --- | --- | --- |
| Rectangle | 1 (HFS) | 149 | 10 176 040 B | `Rectangle.app` (universal x86_64+arm64) | 7 |
| Stats | 1 (HFS) | 114 | 18 634 646 B | `Stats.app` | 37 |
| MonitorControl | 1 (HFS) | 345 | 41 217 020 B | `MonitorControl.app` | 7 |

Independent comparison (`tools/validate-real-dmgs.ps1`):

* **File trees**: CIDER's staged extraction vs a plain `7z x image.dmg` (7-Zip's own nested extraction):
  identical file sets and **identical SHA-256 for every file** (149 / 114 / 345 files, 0 differences).
  Caveat: both paths use 7-Zip's HFS+ decoder, so this validates CIDER's staging, limits and link handling,
  **not** the HFS+ decoder itself. No independent HFS+ implementation was used.
* **Info.plist**: CIDER's bundle metadata equals Python `plistlib` (CFBundleIdentifier, Executable, Version,
  ShortVersionString) for the three apps: `com.knollsoft.Rectangle` 2.0.3, `eu.exelban.Stats` 3.0.20,
  `app.monitorcontrol.MonitorControl` 4.4.0.
* **Mach-O**: every Mach-O file inside the extracted apps was parsed and every slice compared with live
  llvm-objdump (headers, load commands, all symbols): Rectangle **6 files / 12 slices**, Stats **17 / 34**,
  MonitorControl **40 / 47** (45 with symbol tables compared; 2 slices have none or more than 20 000 symbols).
  All match. These include real Swift binaries (names with spaces, hidden/weak symbols, stabs) and
  universal x86_64+arm64 executables and frameworks.

## 3. Incompatibilities found (all fixed unless stated) and where they are tested

| # | Found by | Problem | Fix / regression test |
| --- | --- | --- | --- |
| 1 | libFuzzer on `parse_image` | `LC_SYMTAB` with `strsize == 0` skipped offset validation; `parse_symbols` sliced at the wild offset and threw `std::bad_variant_access` | `macho.cpp` guard; `regression_symtab_zero_strsize_…`, `regression_fuzz_crash_symtab_zero_strsize` (the unminimised crashing input is committed) |
| 2 | real DMGs | hdiutil images contain MBR / GPT / free-space partitions; CIDER extracted them as filesystems → 7-Zip "Unexpected end of data" | skip partition-scheme metadata, silence small non-filesystem partitions; `regression_real_layout_with_gpt_metadata_partitions_…` (builder option `real_layout`) |
| 3 | real DMGs | symlinks (`Applications` alias, framework `Versions/Current`) were created by 7-Zip as reparse points and rejected → extraction aborted | links are **removed, never followed**, reported as one warning; `regression_verify_removes_links_without_touching_their_targets` |
| 4 | MonitorControl | paths > 260 characters: "cannot stat extracted entry" | extended-length (`\\?\`) paths for CIDER's own file operations; `regression_extraction_deeper_than_max_path`, `extended_path_helper` |
| 5 | #4 follow-up | `\\?\` paths are not normalised: `/` appended to them is taken literally | `path_from_utf8` now produces native separators |
| 6 | real Mach-O | `N_UNDF` symbols without `N_EXT` were reported as defined (llvm-objdump says undefined) | `regression_non_external_undefined_symbol_is_undefined` |
| 7 | validation harness | not CIDER bugs, fixed in the *comparer*: stab entries print as `*UND*`, Swift symbol names contain spaces, `w` flag with `.hidden` is not external | — |

### Still open / not compatible

* **Symbolic links in images are not recreated** (Windows cannot portably represent them): `Versions/Current`
  etc. are missing from extracted frameworks. CIDER still finds framework binaries through `Versions/*`
  (all three apps list their embedded frameworks with architectures), but a future loader will need link
  emulation or a path map.
* macOS extended attributes / resource forks come out as NTFS alternate streams (7-Zip behaviour, e.g. 272
  streams for Rectangle) and are ignored by CIDER.
* **APFS**: no APFS image was available; unverified. **HFS+**: verified only as "7-Zip 25.01 reads these three
  images and CIDER's use of it is faithful". Compression other than zlib/raw/ignore (bzip2, LZFSE, LZMA, ADC),
  encrypted images and Apple-Silicon-only DMGs were **not** exercised by real files.
* Case-insensitive collisions inside HFS+X volumes (case-sensitive) are untested.
