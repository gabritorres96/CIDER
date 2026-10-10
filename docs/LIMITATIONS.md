# Limitations

## Technical

* **Only controlled fixtures run (0.2).** `cider run` executes Mach-O x86_64 programs that import nothing but the 24
  libSystem functions listed in EXECUTION.md. No Foundation, AppKit, Objective-C/Swift/C++ runtime, no dyld proper,
  no frameworks, no dependent dylibs, no TLS, no static initializers, no chained fixups, no threads, no signals, no
  printf/stdio/files. Every real application tried is refused.
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

## Execution (0.2)

* **Not a sandbox.** The worker process + Job Object limit crashes, memory, process creation and time; guest machine code
  still runs with the user's full rights (raw NT syscalls, any memory of the worker, files and network through them).
* x86_64 on Windows x64 only; arm64 binaries are refused. Windows 10/11 only; developed and tested on one machine
  (Windows 11 build 26200, MSVC 14.51 and clang-cl 23.1.3).
* ABI bridge limits: up to 6 integer arguments, no variadics, no struct returns, no mixed int/double signatures, no
  callbacks from the host into guest code, one guest at a time (see ABI.md).
* Lazy binding is eager; `__PAGEZERO`/`__LINKEDIT` are not mapped; `envp` is a fixed minimal environment.
* The guest cannot be debugged or traced; a crash only yields the NTSTATUS of the worker.

## Not implemented (by design, for later phases)

Compatibility database and profiles, launcher GUI, virtual filesystem, real sandboxing (AppContainer/low-integrity
process evaluated only on paper), Mach IPC, ARM64.

## Security

No sandbox. No claim of isolation. See SECURITY.md.

## Legal

CIDER does not redistribute or download Apple frameworks. Running macOS software may be subject to
Apple's licence terms (the macOS SLA restricts macOS software to Apple hardware in several
clauses); this project does not give legal advice — check the licence of what you run.

## Execution (0.3)

* Still not a sandbox: guest machine code runs natively with the user's rights; `--root` confines honest programs only.
* One guest thread; no signals, `atexit`, `dlopen`, `fork`, `mmap`, `qsort` (no host→guest callbacks), `FILE` streams beyond stdin/stdout/stderr,
  `readdir`, wide characters, `long double`, locale.
* `printf`: grammar in DARWIN-RUNTIME.md; `%n %a %Lf %ls %'` trap the guest.
* Dylibs: no shared cache, interposing, weak coalescing, lazy loading; lazy binds are applied eagerly; resolver (`STUB_AND_RESOLVER`) exports unsupported.
* Chained fixups: only pointer formats 2 and 6 (no arm64e, 32-bit, kernel-cache, multi-start pages).
* TLS: single thread, no thread destructors, no `__thread_init`.
* Verified on one machine (Windows 11 26200); GitHub Actions never ran. arm64 unsupported.

## 0.4 (Objective-C / Foundation subset)

* Objective-C support covers what clang emits for plain classes, categories, protocols, ARC and MRC on the objc2 ABI. Not supported: blocks, `@try/@catch`, `@synchronized`,
  Swift, relative method lists, message forwarding (`forwardInvocation:`, `resolveInstanceMethod:`), property reflection, KVC/KVO, `NSInvocation`.
* Foundation is a **host-written subset** of 9 classes (NSObject, NSString, NSConstantString, NSNumber, NSArray, NSMutableArray, NSDictionary, NSMutableDictionary, NSAutoreleasePool) plus `NSLog` and four helper functions ([FOUNDATION.md](FOUNDATION.md)); `NSLog` omits Apple's timestamp/pid prefix;
  exceptions end the guest; dictionary lookup is linear; no `NSMutableString`, `NSData`, `NSDate`, `NSURL`, `NSFileManager`, `NSBundle`, run loops, formatters or localisation.
* Threads are cooperative, one at a time: no parallelism, spinning threads starve the others, no race detection ([THREADING.md](THREADING.md)).
* Callbacks are entered only at addresses inside a loaded image's code ([CALLBACKS.md](CALLBACKS.md)); no signal handlers.
* The worker is still **not a sandbox**: Objective-C and Foundation code runs with the worker's full rights.
* Only clang-built fixtures were run; no Apple-built binary and no downloaded application.
