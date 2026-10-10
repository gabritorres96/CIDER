# Darwin layer (`cider-darwin`) and libSystem

**Status: mostly NOT IMPLEMENTED.** CIDER 0.2 provides only the 24-function libSystem subset in `src/libsystem` (see
[EXECUTION.md](EXECUTION.md)). `src/darwin` and `src/filesystem` are README placeholders. The rest of this file is
the design intent for 0.3.

## Principle

Each exported function needs: declaration, implementation, test, documentation and a
compatibility status. A same-named Windows/UCRT function is **not** assumed to have the same
semantics (e.g. `open` flags, `stat` layout, `errno` values, `mmap` granularity: 4 KiB on macOS
x86_64 vs 64 KiB allocation granularity on Windows).

## Priority areas (0.3)

Processes (pid, argv/envp, exit codes, threads), memory (`mmap`/`munmap`/`mprotect` over
`VirtualAlloc`/`VirtualProtect`), files (`open/close/read/write/stat/lseek/mkdir/unlink` with a
virtual root: `/Applications`, `/Users`, `/tmp`, `/Library`, `/System/Library` mapped under
`%LOCALAPPDATA%\CIDER`), time (`clock_gettime`, `mach_absolute_time`), synchronisation (pthread
mutex/cond/semaphores over SRWLOCK/CONDITION_VARIABLE), limited documented signal emulation.

Mach IPC is **not** emulated up front; a port/message abstraction is added only when a test
program needs it.

## Virtual filesystem requirements (design)

Path normalisation, no escapes from the virtual root, case-sensitivity policy, symlink handling,
configurable shared directories, optional per-application isolation. No security guarantees are
claimed until a real sandbox (Job Objects / AppContainer) is implemented and verified.
