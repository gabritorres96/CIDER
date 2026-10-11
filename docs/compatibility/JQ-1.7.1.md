# jq 1.7.1 (macOS x86_64) - first unmodified external binary run by ORVYN

Date 2026-10-11, Windows 11 Pro 10.0.26200. **The binary is the original upstream release asset, byte for byte; ORVYN did not modify it, and it is not part of this repository.**

## Provenance

| Item | Value |
|---|---|
| Project | jq (https://github.com/jqlang/jq) |
| Version | 1.7.1 |
| URL | `https://github.com/jqlang/jq/releases/download/jq-1.7.1/jq-macos-amd64` (+ `sha256sum.txt` from the same release) |
| Licence | MIT (jq); the release binary statically contains oniguruma (BSD-2) and decNumber (ICU licence). It is **not** committed here |
| Size | 851,328 bytes |
| SHA-256 | `4155822bbf5ea90f5c79cf254665975eb4274d426d0709770c21774de5407443` (matches the release's `sha256sum.txt`) |
| Architecture | Mach-O 64-bit x86_64 (thin), minimum macOS 13.0, SDK 13.3, `LC_MAIN` |
| Dependencies | only `/usr/lib/libSystem.B.dylib` |
| Imports | 133 symbols (function and data) of libSystem; no Objective-C, no Swift, no libc++ |
| Exceptions / weak binds / chained fixups | none |

## What it needed (the real first blockers)

`orvyn frameworks <jq>` listed **58 imports ORVYN did not have** (`__DefaultRuneLocale`, `____chkstk_darwin`, `_environ`, about 30 libm functions, `strptime/strftime/timegm/gmtime_r/localtime_r`, `setlocale`, `realpath$DARWIN_EXTSN`, `fopen$DARWIN_EXTSN`, `fdopen$DARWIN_EXTSN`, `memmem`, `strspn`, `___bzero`, `___memcpy_chk`, `_memset_pattern16`, `_pathconf`, `_isatty`, `_raise`, `_rand`, `_strerror`, `_perror`, `_dirname`, ...). They were implemented in `src/libsystem/darwin_ext.cpp` and are described in [../LIMITATIONS.md](../LIMITATIONS.md). Two of them corrected earlier assumptions:

* the Mach-O name of the stack probe is `____chkstk_darwin` (four underscores); ORVYN had registered `___chkstk_darwin` from memory (wrong);
* the inlined `isalpha()/isspace()` read `__DefaultRuneLocale.__runetype[c]` at **offset 60** with the `_CTYPE_A = 0x100` / `_CTYPE_S = 0x4000` flags (read from jq's disassembly). The other `_CTYPE_*` bits are from Apple's public `runetype.h` as remembered, not confirmed by this binary.

## Stages (all REAL runs through `orvyn run` / `guest::run_in_process`)

| Stage | Result |
|---|---|
| A Parse | VERIFIED |
| B Load (mapping, W^X) | VERIFIED |
| C Link (133 imports) | VERIFIED: 133 implemented, 0 stubs, 0 missing |
| D Initialize | VERIFIED (no initializers) |
| E Execute | VERIFIED: guest code runs, including jq's own bytecode compiler for its builtins and the oniguruma regex engine |
| F Exit | VERIFIED: exit codes 0, 2, 3 and 5 as jq defines them |
| G Verify | PARTIAL: stdout/stderr/exit status compared with **documented jq behaviour** (manual examples, arithmetic), not with a recorded macOS run (no Mac available) |

Reproduce: `set ORVYN_EXTERNAL_JQ=<path to jq-macos-amd64>` and run `build\bin\Release\test_external.exe` (the test checks the SHA-256 first and skips any other file). Evidence from `orvyn run <jq> -- --version`: stdout `jq-1.7.1`, exit code 0.

Examples verified (expected value from jq's documentation or mathematics): `--version`, `-n '1+2'`, `map(.*2)`, `fromdate`/`todate`/`gmtime`/`strftime`/`strptime` (the manual's 1425599507 = 2015-03-05T23:51:47Z), `sqrt/pow/cbrt/tgamma/atan`, regex `test/match/gsub/scan`, `@csv/@sh/@base64`, number formatting (`1/3`, `0.1+0.2`), `sort_by`, `reduce`, `limit`, file input through `--root`, `-S`, missing file (exit 2), division by zero (exit 5), syntax error (exit 3).

## Not covered / known differences

* Not verified against a macOS run: the exact text of every message, `localtime` (the guest's time zone is UTC), `rand()` sequence, `$ENV` contents, `strerror` strings outside the table, behaviour with a terminal on stdin/stdout, `--seq`, `--stream`, `input_filename` with several files, big inputs, performance.
* stdin handling was not exercised by the tests.
* This shows that **this one binary** runs. It is not a statement about jq on other macOS versions or about other programs.
