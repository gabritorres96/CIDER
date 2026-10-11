# Darwin runtime: ORVYN's libSystem (0.3)

An **adaptation layer**, not Apple's libc. Each function re-implements the Darwin contract (constants, structure layouts, errno
numbers) on Win32. Anything not listed here is not implemented: a binary importing it is refused at load time
(`Unresolved symbol`, exit 12). Source: `src/libsystem/`.

## Implemented functions

| Area | Symbols |
| --- | --- |
| Output | `puts putchar fputs fputc putc fwrite fflush write` |
| printf family | `printf fprintf sprintf snprintf vprintf vfprintf vsprintf vsnprintf` |
| Files | `open close read write lseek stat fstat access getcwd` (`stat`/`fstat` also as `$INODE64`) |
| Time | `gettimeofday clock_gettime time nanosleep usleep sleep` |
| Process | `getpid getppid getenv exit _exit` |
| Memory | `malloc calloc realloc free memcpy memmove memset memcmp` |
| Strings | `strlen strcmp strncmp strcpy strncpy strcat strchr strrchr strstr strdup atoi strtol` |
| Math | `abs labs sqrt fabs floor ceil pow sin cos tan exp log log10 atan atan2 fmod ldexp frexp` |
| Aggregates | `div ldiv` |
| Other | `___error` (errno), `___stdinp/___stdoutp/___stderrp` (data), `__tlv_bootstrap/_tlv_get_addr`, trapping `dyld_stub_binder` |

**Not** implemented (examples): `fopen/fread/fgets/scanf`, `qsort` (needs guest callbacks), `fork/exec`, signals, threads,
`mmap`, `dlopen`, sockets, `readdir`, `unlink/mkdir`. FILE is **opaque**: only stdin/stdout/stderr, no buffering
(`fflush` is a no-op because every write already reached the sink). Stdio is therefore a subset, not "complete stdio".

## printf

ORVYN never passes a Darwin `va_list` or System V variadic registers to the Windows CRT. `printf_format.cpp` pulls arguments
from an `ArgReader` (registers + stack of a direct call, or a Darwin `va_list` in guest memory) and renders each conversion.
Supported: flags `- + space # 0`; width/precision (number or `*`); lengths `hh h l ll j z t q`;
conversions `d i u o x X c s p f F e E g G %`. `%p` is `0x…` lowercase; `inf`/`nan` are spelled Darwin-style; `%s` of NULL is `(null)`.
**Refused with a runtime trap (guest ends, state `faulted`)**: `%n`, `%a/%A`, `L` (long double), `%ls/%lc/%C/%S`, `%D/%U/%O`,
the `'` flag, positional `%1$d`, absurd widths. Numeric rendering uses the host `snprintf` on one validated number at a time.

## Descriptors, paths, errno

* Guest descriptors are a private table (0/1/2 console, files from 3), **not** Win32 handles or CRT fds. Closed on exit.
* `open` flags are Darwin's (`O_CREAT=0x200`, `O_TRUNC=0x400`, `O_EXCL=0x800`, `O_APPEND=0x8` …); unknown bits → `EINVAL`.
  Directories → `EISDIR` (no `readdir`).
* `struct stat` is the Darwin x86_64 64-bit-inode layout (144 bytes); modes synthesized (`0644/0755`, read-only → `0444/0555`).
* `errno` values are Darwin's (`ENOENT 2, EBADF 9, EEXIST 17, EINVAL 22, EAGAIN 35` …) through `___error()`.
* Clock ids are Darwin's (`CLOCK_MONOTONIC = 6`); `X_OK` on regular files fails (Windows has no execute bit).
* The guest's `/` is `orvyn run --root <dir>` (default: current directory). Paths are normalised lexically (`..` stops at the
  root), Windows syntax (`C:`, `\`, streams, wildcards, `CON`/`NUL`…, trailing `.`/space) is refused, and existing paths are
  re-checked after link resolution (junctions cannot leave the root). Without a root (library use) file access is `EACCES`.
  This confines honest programs; it is **not a sandbox**.

## Limits

Cooperative guest threads only ([THREADING.md](THREADING.md)); no signals; output goes through the worker's stdout/stderr (or the JSON capture).

## 0.4 additions

* **FILE streams**: `fopen`, `fclose`, `fread`, `fwrite`, `fseek`, `fseeko`, `ftell`, `ftello`, `rewind`, `feof`, `ferror`, `clearerr`, `fflush`, `fgets`, `fgetc`/`getc`, `ungetc`, `fputs`, `fputc`,
  `fileno`, `setvbuf`. A guest `FILE*` is an **opaque pointer to a 64-byte read-only slot** of a pool (64 slots; 0-2 are the standard streams), never a copy of Darwin's
  `FILE` struct, and is only compared with the pool range and indexed — a forged, stale or NULL pointer fails with `EBADF`. Streams sit on top of the descriptor table, so `--root`
  confinement, the descriptor limit and close-at-exit are shared with `open`. Files are opened in **binary mode** (bytes written are bytes on disk; no CRLF translation, no Ctrl-Z),
  writes are unbuffered, reads are buffered in 4 KiB chunks that `fseek`/`rewind` discard. Without `--root` `fopen` fails with `EACCES`. Not implemented: `fscanf`, `popen`, `tmpfile`, `freopen`, `fdopen`, wide streams, `fmemopen`.
* `qsort`, `bsearch`, `atexit` (host → guest callbacks, [CALLBACKS.md](CALLBACKS.md)); `pthread_*` ([THREADING.md](THREADING.md)); per-thread `errno`.
* `printf`-family conversions gained `%@` **only** for NSLog / `+stringWithFormat:` (the libSystem `printf` rejects it).
* Objective-C runtime and Foundation subset: [OBJECTIVE-C-RUNTIME.md](OBJECTIVE-C-RUNTIME.md), [FOUNDATION.md](FOUNDATION.md).
