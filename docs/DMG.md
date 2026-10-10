# Disk images (DMG) — `cider-dmg`

Source: `src/dmg`, API in `include/cider/dmg.hpp`. Commands: `cider inspect <x.dmg>`,
`cider dmg-info`, `cider extract`.

**Support is deliberately narrow and verified only as stated below. CIDER does not claim universal
DMG, HFS+ or APFS support.**

## Detection (content-based, never by extension)

| Kind | How it is recognised | Supported |
| --- | --- | --- |
| UDIF | 512-byte `koly` trailer at the end of the file | structure parsed by CIDER |
| Encrypted | `encrcdsa` / `cdsaencr` header | detected, **not supported** (clear error) |
| Sparse image | `sprs` header | detected, **not supported** |
| Raw image | HFS+/HFSX (`H+`/`HX` @1024), APFS (`NXSB` @32), ISO 9660/Joliet/UDF, NTFS, FAT, GPT, APM at the start | detected; extraction through 7-Zip |
| Sparse bundles (directories) | — | not handled |

## UDIF parsing (pure C++, no external tool)

`koly`: version (4 expected, other = warning), header size (must be 512), flags, data fork,
XML plist range, sector count, segment count (>1 = unsupported). The embedded XML plist is parsed
with the CIDER plist reader (size-limited) and each `blkx` entry's `mish` block table is
validated: signature, chunk table size, every chunk's sector range inside its partition and data
range inside the data fork and file. Compression is reported per chunk type:
zero-fill, raw, ignore, ADC (`0x80000004`), zlib (`0x80000005`), bzip2 (`0x80000006`),
LZFSE (`0x80000007`), LZMA (`0x80000008`).

**CIDER does not decompress anything itself.** The filesystem is reported as:

* *verified from data* — only when the partition's first chunk is stored raw and a signature was
  read from it (e.g. ISO 9660 + Joliet in the test images);
* *inferred from partition name* — e.g. `Apple_HFS` → HFS+, `Apple_APFS` → APFS. This is a hint,
  not a check.

## Extraction through 7-Zip (optional backend)

`locate_7zip`: `--7z <abs path>` → `$CIDER_7Z` → absolute `PATH` entries (`7z.exe`, `7zz.exe`) →
`Program Files\7-Zip`. The candidate is executed once with no arguments and must print
"7-Zip"; its version is parsed. `cider dmg-info` also asks 7-Zip to list the image to confirm it
can open it.

Pipeline (`extract_image`):

1. `7z l -slt` (auto-detect) — every listed path is vetted (also nested filesystem entries).
2. UDIF: `7z l/x -tdmg` unpacks one raw file per partition into a private temporary directory
   (`-tdmg` stops 7-Zip from expanding nested filesystems on its own, so sizes are known first).
   Non-UDIF raw images are their own single partition.
3. Each partition is listed again, validated, and extracted into `<output>/<partition>/`.
4. The result tree is re-verified; `.app` directories are located (`find_apps`).

Safety measures:

* **Path traversal**: absolute paths, drive letters, `..` (also with trailing dots/spaces),
  `:` (alternate streams) and reserved device names (`CON`, `NUL`, `COM1`…) in any listing abort
  the extraction before anything is written. Afterwards every extracted entry must resolve inside
  the output directory; symlinks/reparse points are deleted (the link, never its target) and reported.
* **Limits**: `--max-size` (default 8 GiB total unpacked) and 200 000 files, checked on listings
  before extraction and on the written tree after it.
* **Temporary files**: a private `cider-<random>` directory (in `--temp-dir`/`%TEMP%`) is removed
  before returning, on success and failure. `inspect` extracts into such a directory and removes
  it after analysing the apps. For `extract`, a failed run removes what it wrote into `--output`
  (a directory that did not exist is deleted again).
* **Output directory** must not exist or be empty.
* **Process**: 7-Zip runs without stdin (no password prompts), merged output capped at 64 MiB,
  15-minute timeout, killed if CIDER exits.
* **Nothing extracted is ever executed or loaded.**

## What was verified

| Case | Verified with |
| --- | --- |
| UDIF with raw chunks wrapping ISO 9660 + Joliet (synthetic) | 7-Zip 25.01, automated |
| UDIF with zlib chunks (stored deflate blocks, synthetic) | 7-Zip 25.01, automated |
| hdiutil-like layout (MBR + GPT + free partitions around the filesystem, synthetic) | 7-Zip 25.01, automated |
| Headerless ISO image | 7-Zip 25.01, automated |
| Malformed `koly`/XML/`mish`, encrypted & sparse headers | CIDER only, automated (+ libFuzzer) |
| **Genuine hdiutil images**: Rectangle 2.0.3, Stats 3.0.20, MonitorControl 4.4.0 (UDIF v4, 8 partitions, zlib/raw/ignore chunks, `Apple_HFS` partition) | 7-Zip 25.01; UDIF parsed by CIDER, extracted, trees identical to a direct `7z x`, metadata equal to plistlib/llvm-objdump — see [REAL-WORLD.md](REAL-WORLD.md) |
| HFS+ | only *through 7-Zip's decoder* on the three images above; no independent HFS+ implementation was used |
| APFS | **not verified** (no APFS image tested; CIDER only detects/infers it, 7-Zip's APFS support is version dependent) |
| bzip2 / LZFSE / LZMA / ADC chunks, encrypted images with a real password | **not verified**; reported, extraction depends on 7-Zip |
| Multi-segment images, sparse bundles, images > a few tens of MB | not tested / unsupported |

## Known limitations

* No password support; encrypted images are refused.
* Symbolic links in the image (the `/Applications` alias, framework `Versions/Current`, ...) are **removed and
  reported, never recreated or followed**; embedded frameworks are still found through `Versions/*`.
* Partition-scheme metadata (MBR/GPT/APM) and small non-filesystem partitions are skipped silently.
* macOS extended attributes / resource forks become NTFS alternate streams (7-Zip behaviour) and are ignored.
* Paths longer than MAX_PATH are handled with extended-length (`\\?\` prefix) paths for CIDER's own file operations;
  7-Zip uses its own long-path support. Tools that are not long-path aware cannot open the extracted tree.
* Extraction depends on a third-party tool and its version; its parsers are not sandboxed.
* Extracting writes the whole image to disk (twice transiently for UDIF: partition + files).
* The 7-Zip console output language follows the system; CIDER only relies on the machine-readable
  `-slt` keys and exit codes.
