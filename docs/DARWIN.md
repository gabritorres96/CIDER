# Darwin layer and libSystem

**Status (0.3):** a subset of libSystem exists in `src/libsystem` (see [DARWIN-RUNTIME.md](DARWIN-RUNTIME.md) for the exact function
list, printf grammar, descriptor table, path rules and errno mapping). `src/darwin` and `src/filesystem` remain placeholders.

## Principle

Each exported function needs: declaration, implementation, test, documentation and a compatibility status. A same-named
Windows/UCRT function is **not** assumed to have the same semantics (`open` flags, `struct stat`, `errno` values, clock ids,
`va_list` all differ; 0.3 implements the Darwin versions explicitly).

## Still missing (future)

`fopen`-style FILE streams, `readdir`/`mkdir`/`unlink`, `mmap`/`mprotect`, threads and pthread synchronisation, signals, Mach IPC,
`dlopen`, `atexit`, locale, wide characters, sockets. Mach IPC is not emulated up front; a port abstraction is added only when a test
program needs it. No security guarantee is claimed: see SECURITY.md (the worker is not a sandbox).
