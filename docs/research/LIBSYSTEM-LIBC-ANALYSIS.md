# libSystem / libc analysis

Goal: decide which Darwin system APIs ORVYN should implement next, how they differ from Windows, and in which order. No extensive implementation is proposed here.
Labels: **VERIFIED** (observed in code, a run or a cited source file), **INFERRED**, **HYPOTHESIS**.

## 1. Sources and method

* Darwin side: `Libc-1725.0.11` (`apple-oss-distributions/Libc`, macOS 26.0 tag, APSL 2.0), `libpthread-539`, `xnu-12377.1.9` (syscall numbers `bsd/sys/syscall.h`), `Libsystem-1356` (umbrella). Reference only, see [APPLE-OPEN-SOURCE-INVENTORY.md](APPLE-OPEN-SOURCE-INVENTORY.md).
* ORVYN side: the host function registry (`orvyn sdk-scan @implemented`, exact list asserted by `tests/runtime/test_runtime.cpp`) — **169 libSystem symbols** (functions and `___std*p`), 82 `libobjc`, 62 CoreFoundation, 125 CoreGraphics (incl. `NS*` geometry), 20 Foundation functions, 49 classes.
* Demand side: the import list of a real application. `utilities/reports/coteditor_imports_by_lib.json` (CotEditor 7.0.9, built from `llvm-objdump --chained-fixups`): **72 libSystem imports, 36 not implemented** (below). One application is a *sample*, not a statistic; Sparkle (its updater) is non-Swift and contributes the C-level imports.

## 2. What real code asks for first (VERIFIED, CotEditor + Sparkle)

Not implemented, from the actual import list:

| Symbol(s) | Why every program has them | Darwin source | ORVYN plan |
|---|---|---|---|
| `___stack_chk_guard`, `___stack_chk_fail` | clang `-fstack-protector` (default in Xcode builds) | `Libc/sys/OpenBSD/stack_protector.c` | trivial: a random guard value in a data symbol; `fail` = trap with a message |
| `___chkstk_darwin` | stack probe for frames > 4 KiB | libSystem (`libsystem_pthread`/Libc asm) | trivial if the guest stack is fully committed (HYPOTHESIS: check `memory::` stack commit policy) |
| `___assert_rtn`, `_abort` | `assert()`, fatal paths | `Libc/gen/FreeBSD/assert.c`; abort raises SIGABRT | print + end the guest (exit 134, like a trap) |
| `___maskrune`, `___tolower`, `___toupper`, `__DefaultRuneLocale` | `isalpha()` & co., `tolower()` expand to these on Darwin (header `_ctype.h`) | `Libc/include/_ctype.h`, `runetype.h` | needs the *data* table `_DefaultRuneLocale` in guest memory with Darwin's layout; medium |
| `_memchr`, `_modf`, `_fma`, `___exp10`, `_clock`, `_sscanf` | plain C | `Libc/string`, `gdtoa`, `stdio` | easy; `sscanf` needs a real scanf engine (currently refused: `sscanf` is in the "honest negatives" of `test_runtime`) |
| `_posix_memalign`, `_malloc_size` | allocation | libmalloc | easy on the guest heap (record sizes) |
| `_os_unfair_lock_lock/unlock`, `_os_log_impl`, `_os_log_type_enabled`, `_os_release`, `_voucher_adopt` | modern Darwin code uses `os_*` instead of pthread mutexes and `NSLog` | libplatform, libsystem_trace | locks: easy on cooperative guest threads; os_log: forward to the ORVYN log (format needs a *different* varargs encoding: os_log buffers are structured, not printf) |
| `_getuid`, `_getpwuid` | identity | Libc/Libinfo | map to a fixed documented uid (e.g. 501) and a profile-derived passwd entry; decision needed, not code |
| `_getxattr`, `_setxattr`, `_removexattr` | file metadata | xnu syscalls | NTFS has alternate data streams (different model): defer |
| `_dlsym`, `__dyld_register_func_for_add_image`, `_getsectiondata`, `__availability_version_check` | dynamic loading, runtime feature checks (`@available`) | dyld (see [DYLD-COMPATIBILITY.md](DYLD-COMPATIBILITY.md) rows 18-19) | loader work first; `_availability_version_check` is a trivial "return macOS version X" decision |
| `_pthread_main_np` | "am I the main thread?" | `libpthread/src/pthread.c:1086` | trivial |

Everything else the application imports from libSystem (36 symbols) *is* implemented.

## 3. Darwin vs Windows: the semantic table

`API Darwin | ORVYN today | Windows equivalent | Differences that matter | Difficulty | Priority`. Difficulty: E easy, M medium, H hard. Priority: 1 first.

| API | ORVYN today | Windows equivalent | Differences that matter | Diff. | Prio |
|---|---|---|---|---|---|
| `open/read/write/close/lseek` | implemented, on a **guest fd table** (not Windows handles), confined by `--root` | `CreateFileW`, `ReadFile`, `WriteFile`, `SetFilePointerEx` | permissions bits and `O_*` values are Darwin's (`kO_CREAT=0x200`...); text/binary mode does not exist on Darwin; `O_CLOEXEC`, `O_NONBLOCK`, `O_APPEND` semantics; path separator; case sensitivity is a *volume* property on macOS (APFS may be case-insensitive, default) and Windows NTFS is case-insensitive by default, so most apps behave the same, but case-sensitive volumes differ | done | — |
| `stat/fstat` (`stat$INODE64`) | implemented | `GetFileInformationByHandleEx` | struct layout is Darwin's 144-byte `stat`; no inode numbers on FAT; times are `timespec`; mode bits synthesized (no POSIX permissions on NTFS) | done (partial) | — |
| `opendir/readdir/closedir`, `getdirentries64` | **absent** | `FindFirstFileExW` | `dirent` layout, `d_type`, ordering (APFS: no guaranteed order, NTFS: alphabetical) | M | 2 |
| `unlink/rename/mkdir/rmdir/chmod/chown/utimes/symlink/readlink/realpath/fcntl/dup/dup2/pipe` | **absent** | `DeleteFileW`, `MoveFileExW`, `CreateDirectoryW`, `SetFileAttributesW`, `CreateSymbolicLinkW` (needs privilege or developer mode), `GetFinalPathNameByHandleW`, `DuplicateHandle`, `CreatePipe` | symlink creation may fail without privilege; rename over an open file fails on Windows; `fcntl(F_GETPATH)` etc. | M | 2 |
| `mmap/munmap/mprotect/madvise/msync` | **absent** for the guest (the loader maps images itself) | `VirtualAlloc`, `MapViewOfFile`, `VirtualProtect` | 64 KiB allocation granularity vs 4/16 KiB pages (fixed-address maps can fail); `MAP_ANON` vs file mappings; `PROT_EXEC` + W^X policy (ORVYN refuses RWX); copy-on-write semantics | M/H | 3 |
| `malloc/calloc/realloc/free` | implemented on a guest heap; `posix_memalign`, `malloc_size`, `reallocf`, `valloc` absent | `HeapAlloc`/own allocator | alignment guarantees (16 bytes on Darwin x86_64); `malloc_zone_*` API used by some frameworks | E | 1 |
| `pthread_*` | 31 functions implemented, **cooperative** threads under a guest lock ([THREADING.md](../THREADING.md)) | Win32 threads, SRW locks, condition variables | the guest never preempts: lock-order assumptions hold, spin-waits on another thread's progress do not; `pthread_rwlock_*`, `pthread_barrier`, `pthread_setname_np`, `pthread_get_stackaddr_np` absent | M | 2 |
| thread-local storage (`__thread`, `pthread_key`) | implemented (`tlv_get_addr`, keys) | `TlsAlloc`/`__declspec(thread)` | Darwin TLV descriptors vs Windows TEB slots: separate mechanisms; ORVYN never touches `%gs` | done | — |
| `signal/sigaction/kill/raise/sigprocmask` | **absent** | SEH + vectored handlers; CRT `signal` only for SIGINT/SIGABRT/SIGFPE/SIGSEGV/SIGILL/SIGTERM | async-signal semantics, signal masks per thread, `SA_SIGINFO` frames with `ucontext_t`, `SIGPIPE` | H | 4 (needed by crash handlers, `kill`-based IPC) |
| `fork/exec*/posix_spawn/waitpid/system` | **absent** (refused by design) | `CreateProcessW` | `fork` has no Windows equivalent; `posix_spawn` can be emulated but would run host executables or other Mach-Os | H | 5 |
| `clock_gettime/gettimeofday/time/nanosleep/usleep/sleep` | implemented | `QueryPerformanceCounter`, `GetSystemTimePreciseAsFileTime`, `Sleep` | which Darwin clocks tick during system sleep differs between `CLOCK_MONOTONIC`, `CLOCK_MONOTONIC_RAW` and `CLOCK_UPTIME_RAW` (PENDING VALIDATION against the man page of the target macOS: not verified here); `mach_absolute_time` + `mach_timebase_info` absent | E (`mach_*`) | 2 |
| `getenv/setenv/unsetenv/environ` | `getenv` implemented over a guest env table | `GetEnvironmentVariableW` | Windows env names are case-insensitive, Darwin's are not: ORVYN's own table keeps Darwin semantics (good); `setenv` absent | E | 2 |
| `getpid/getppid/getuid/geteuid/getgid/getpwuid` | `getpid`, `getppid` | `GetCurrentProcessId`, token SID | no uid/gid model on Windows: **a policy decision** (constant uid vs SID hash); users/groups files absent | E (decision) | 2 |
| `sysctl/sysctlbyname/sysconf/uname/host_info` | **absent** | `GetSystemInfo`, `GlobalMemoryStatusEx`, `RtlGetVersion` | many apps read `hw.ncpu`, `hw.memsize`, `kern.osversion`, `machdep.cpu.brand_string`: values must be *chosen* (what macOS version do we claim?) | M | 2 |
| `setlocale/localeconv/nl_langinfo/iconv`, ctype tables | partial: none of the locale API; `_DefaultRuneLocale` absent | `GetUserDefaultLocaleName`, `MultiByteToWideChar`, ICU | Darwin's `isalpha()` etc. are *table lookups* through `_DefaultRuneLocale` (a data structure the guest reads directly) | M | 1 |
| Unicode: `CFString`, `wchar_t` | UTF-8 storage with UTF-16 views in Foundation | UTF-16 native | `wchar_t` is **4 bytes** on Darwin, 2 on Windows: never pass guest `wchar_t*` to Win32 | done (Foundation) / E (wchar API) | 3 |
| BSD sockets, `getaddrinfo`, `poll/select/kqueue` | **absent** | Winsock2 (`WSAStartup`, `SOCKET`, `WSAPoll`) | `sockaddr` has a length byte on BSD (`sa_len`) and different `AF_*` (`AF_INET6` = 30 vs 23) and `SOL_SOCKET` constants; sockets are not fds on Windows (a guest fd table helps); `kqueue` has no equivalent (IOCP/WSAPoll emulation) | H | 4 |
| `dlopen/dlsym/dladdr`, `_dyld_*` | **absent** | `LoadLibrary` is *not* a substitute (images are Mach-O) | loader feature: see DYLD doc | H | 1 |
| `atexit`, `__cxa_atexit`, `__cxa_finalize` | `atexit` only | CRT | C++ static destruction and `dlclose` hooks need `__cxa_*` | E | 1 |
| `setjmp/longjmp/sigsetjmp`, `getcontext/makecontext` | **absent** | `RtlCaptureContext`, CRT `setjmp` | the jmp_buf layout is Darwin's; `longjmp` across ORVYN host frames (thunks) must unwind the bridge: **dangerous** (see ABI doc, callbacks) | H | 3 |
| stdio (`printf` family, FILE streams) | implemented (full format engine incl. `%@`), `scanf` family absent | CRT | `FILE` is opaque to Darwin guests except through `__stdinp`, `___stdoutp`; `__sFILE` macros like `getc_unlocked` read fields directly in some SDK headers (HYPOTHESIS: check per fixture) | done / M (`scanf`) | 2 |
| `os_log`, `os_signpost`, `os_unfair_lock`, `dispatch_*` | `dispatch_*` yes; `os_*` absent | `OutputDebugString`, ETW, SRW | `os_log` format is *compile-time encoded* (the compiler emits a descriptor and a buffer): needs its own decoder | M | 1 |

## 4. Opportunities to use real Win32 or portable libraries (not implemented here)

* **Directory, rename, mkdir, unlink, realpath, symlink** → a small POSIX-over-Win32 file module on the guest fd table; the pieces already exist in `src/filesystem` (confinement) and `NSFileManager` (root-confined).
* **`scanf`, `strtod/strtol` family, `gdtoa`** → port from `Libc/gdtoa` and `Libc/stdio` (APSL, reference) *or* reimplement; licence decision in [THIRD-PARTY-LICENSE-REVIEW.md](THIRD-PARTY-LICENSE-REVIEW.md). `Libc/gdtoa` is David Gay's code (permissive notice) — to be confirmed per file.
* **ctype / rune tables** → generate the data from the public FreeBSD rune definitions rather than copying Apple's tables (HYPOTHESIS).
* **Locale/Unicode** → Windows ships ICU (`icu.dll`, header `icu.h` in the Windows SDK; the minimum Windows version for each of its exports was not checked here): a candidate backend for collation/normalization/case-mapping needed by `NSString` (HYPOTHESIS; PENDING VALIDATION of availability on the supported Windows versions).
* **Sockets** → Winsock2 with a translation layer for `sockaddr`, constants and fd mapping.

## 5. Order of work suggested by the evidence

1. `__stack_chk_*`, `___chkstk_darwin`, `__assert_rtn`, `abort`, `memchr`, `modf`, `fma`, `posix_memalign`, `malloc_size`, `pthread_main_np`, `__cxa_atexit`, `os_unfair_lock_*` (all easy; they unblock the *first* link error of every real binary).
2. ctype/rune tables; `os_log` family; `sscanf`/`strtod`.
3. `dlopen`/`dlsym`/`_dyld_*` (shared with the loader).
4. Directory and metadata calls; `sysctl`/`sysconf` values; `getuid` policy.
5. `mmap` family; signals; sockets.

Each step must come with a clang-built fixture that fails before and passes after (the existing `tests/fixtures/exec` and `objc` generators are the template) and the exact-registry test in `test_runtime.cpp` must be updated, so no symbol is "claimed" without a test.
