# Mach-O and FAT support

Implemented in `src/macho` (`include/cider/macho.hpp`). Written from the public format description;
no Apple headers are copied.

## Containers

| Format | Support |
| --- | --- |
| Mach-O 64-bit little-endian (`0xfeedfacf`), cputype x86_64 / arm64 / ppc64 | parsed |
| Mach-O 32-bit, big-endian | recognised, reported as `Unsupported` |
| FAT (`0xcafebabe`) and FAT64 (`0xcafebabf`) | parsed; each slice validated independently |
| Java class files (same magic as FAT) | rejected via the arch-count sanity check (1..64) |

A broken slice inside a FAT file does **not** fail the file: it is reported in `Slice::error`.

## Load commands

`LC_SEGMENT_64` (+ sections), `LC_MAIN`, `LC_UNIXTHREAD` (x86_THREAD_STATE64 only),
`LC_LOAD_DYLIB`, `LC_LOAD_WEAK_DYLIB`, `LC_REEXPORT_DYLIB`, `LC_LAZY_LOAD_DYLIB`,
`LC_LOAD_UPWARD_DYLIB`, `LC_ID_DYLIB`, `LC_RPATH`, `LC_LOAD_DYLINKER`, `LC_SYMTAB`, `LC_DYSYMTAB`,
`LC_DYLD_INFO(_ONLY)`, `LC_DYLD_CHAINED_FIXUPS`, `LC_DYLD_EXPORTS_TRIE`, `LC_FUNCTION_STARTS`
(ranges validated, contents **not decoded**), `LC_BUILD_VERSION`, `LC_VERSION_MIN_*`, `LC_UUID`,
`LC_CODE_SIGNATURE`, `LC_ENCRYPTION_INFO_64`. Unknown commands are skipped after the generic
`cmdsize` check.

Not decoded yet: bind/rebase opcode streams, chained-fixup contents, export trie contents,
`__eh_frame`/`__unwind_info`, Objective-C metadata.

## Validation performed

* header ≥ 32 bytes, known 64-bit cputype, filetype 1..12;
* `ncmds`/`sizeofcmds` limits and mutual consistency (64-bit math), commands inside the slice;
* each `cmdsize` ≥ 8, multiple of 8, inside `sizeofcmds`, ≥ the command's minimum;
* segments: `vmaddr+vmsize` and `fileoff+filesize` overflow, file range inside the slice,
  `filesize ≤ vmsize`; sections inside their segment (address and, unless zero-fill, file range);
  no virtual-memory or file overlap between segments;
* strings (install names, rpaths) must be NUL-terminated inside the command;
* symbol/string tables, relocations, dyld info, linkedit data: ranges inside the slice with
  overflow-safe table sizing; `LC_DYSYMTAB` groups inside the symbol table;
* duplicate singleton commands rejected; `LC_MAIN`/`LC_UNIXTHREAD` target must lie in a segment;
* FAT: arch table, slice ranges, zero size, overlap with the table and between slices, alignment
  (warning), duplicates (warning), nested FAT (slice error).

## Code signatures

The embedded SuperBlob is parsed (CodeDirectory identifier, team id, flags: ad-hoc/runtime,
entitlements/requirements presence, CMS presence). **It is not verified**: no hash or certificate
checking. Parse problems are warnings.

## Limits (defaults)

`max_file_size` 512 MiB (callers read whole files), `max_load_commands` 65535,
`max_sizeofcmds` 16 MiB, `max_fat_archs` 64, `max_symbols` 4 Mi.
