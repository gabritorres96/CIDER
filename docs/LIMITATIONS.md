# Limitations

## Technical

* **Only controlled fixtures run (0.2).** `orvyn run` executes Mach-O x86_64 programs that import nothing but the 24
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
  are reported as "system, not provided by ORVYN yet".
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

ORVYN does not redistribute or download Apple frameworks. Running macOS software may be subject to
Apple's licence terms (the macOS SLA restricts macOS software to Apple hardware in several
clauses); this project does not give legal advice — check the licence of what you run.

## Execution (0.3)

* Still not a sandbox: guest machine code runs natively with the user's rights; `--root` confines honest programs only.
* One guest thread; no signals, `atexit`, `dlopen`, `fork`, `mmap`, `qsort` (no host→guest callbacks), `FILE` streams beyond stdin/stdout/stderr,
  `readdir`, wide characters, `long double`, locale.
* `printf`: grammar in DARWIN-RUNTIME.md; `%n %a %Lf %ls %'` trap the guest.
* Dylibs: no shared cache, interposing, lazy loading; weak definitions are coalesced since 0.6 (first non-weak export, else the first weak one in load order; see the 0.6 section); lazy binds are applied eagerly; resolver (`STUB_AND_RESOLVER`) exports unsupported.
* Chained fixups: only pointer formats 2 and 6 (no arm64e, 32-bit, kernel-cache, multi-start pages).
* TLS: single thread, no thread destructors, no `__thread_init`.
* Verified on one machine (Windows 11 26200); GitHub Actions never ran. arm64 unsupported.

## 0.4 (Objective-C / Foundation subset)

* Objective-C support covers what clang emits for plain classes, categories, protocols, ARC and MRC on the objc2 ABI. Not supported: `@try/@catch` (see 0.6),
  Swift, property reflection, KVC/KVO. (Blocks, `@synchronized`, relative method lists, message forwarding and `NSInvocation` were added in 0.5/0.6; this line was out of date.)
* Foundation is a **host-written subset** of 9 classes (NSObject, NSString, NSConstantString, NSNumber, NSArray, NSMutableArray, NSDictionary, NSMutableDictionary, NSAutoreleasePool) plus `NSLog` and four helper functions ([FOUNDATION.md](FOUNDATION.md)); `NSLog` omits Apple's timestamp/pid prefix;
  exceptions end the guest; dictionary lookup is linear; no `NSMutableString`, `NSData`, `NSDate`, `NSURL`, `NSFileManager`, `NSBundle`, run loops, formatters or localisation.
* Threads are cooperative, one at a time: no parallelism, spinning threads starve the others, no race detection ([THREADING.md](THREADING.md)).
* Callbacks are entered only at addresses inside a loaded image's code ([CALLBACKS.md](CALLBACKS.md)); no signal handlers.
* The worker is still **not a sandbox**: Objective-C and Foundation code runs with the worker's full rights.
* Only clang-built fixtures were run; no Apple-built binary and no downloaded application.

## 0.5 (AppKit, Blocks, libdispatch, GUI)

* AppKit is a **subset of 9 classes** creating native Windows controls ([APPKIT.md](APPKIT.md)): no menus, no text system, no drawing, no images, no panels, no Auto Layout, no nibs. **No general AppKit/Cocoa compatibility is claimed.**
* **Swift is not supported at all**; every real application tried (CotEditor, Rectangle, Stats, MonitorControl) is a Swift app and is refused. SwiftUI does not exist.
* `@try/@catch` are not supported: since 0.6 programs that contain them LOAD, but nothing is ever caught (see the 0.6 section); uncaught `NSException` throws end the guest.
* Message forwarding does not handle structures by value; blocks are a subset ([BLOCKS.md](BLOCKS.md)); libdispatch runs concurrent queues serially and has no sources ([LIBDISPATCH.md](LIBDISPATCH.md)).
* GUI: only the main guest thread touches windows; the offscreen backend is a test device; the Win32 GUI tests need an interactive desktop; keyboard codes are Windows virtual-key codes; DPI scaling is per monitor but only the primary display is modelled.
* The worker is **still not a sandbox**: guest code, including GUI programs, runs with the user rights. For GUI programs the default 60 s timeout is lifted to 24 h (an explicit `--timeout` still applies).
* Release/ASan/fuzzing evidence is in [PROGRESS.md](PROGRESS.md); GitHub Actions has still never run.

## 0.6.1 - 0.6.2 (CoreGraphics drawing, `-drawRect:`)

* CoreGraphics is a **subset**: no bitmap/PDF contexts, gradients, shadings, patterns, shadows, blend modes, text drawing, color spaces other than DeviceRGB; no way yet for a
  guest to create a `CGImage` ([COREGRAPHICS-RENDERING.md](COREGRAPHICS-RENDERING.md)). A program importing anything else is refused when loaded.
* `CGContextGetCTM` is relative to the view (identity at start), not the full device matrix; stroke width under a non-uniform scale is the average scale.
* `dirtyRect` is always the whole view; `setNeedsDisplayInRect:` is widened. A repaint request from inside the same view's `-drawRect:` is ignored.
* Drawing happens only for views whose class overrides `-drawRect:`; every such view is its own window, so a parent's `-drawRect:` does not paint its subviews, and
  there is no layer backing, `isOpaque`, `bounds` origin, `lockFocus` or `cacheDisplayInRect:`. `NSColor`, `NSBezierPath`, `NSRectFill`, `NSImage`, text drawing do not exist yet.
* Main thread only: the Direct2D factory is single-threaded and every surface belongs to the thread that pumps messages.
* Invalid or dead CoreGraphics handles are refused with a log line, not with the crash Apple's CoreGraphics may produce; a guest relying on that crash is not emulated.
* Verified on one machine (Windows 11, one monitor at the primary DPI), software Direct2D for the headless tests and the default Direct2D target type for windows.
* The worker is still not a sandbox.

## 0.6 (in progress; version number unchanged): modern Objective-C, weak binding, frameworks, Darwin libc

* **Relative ("small") method lists**: read (direct and indirect selectors, categories, class methods, bounds-checked). Tested only on fixtures converted by `tests/fixtures/relmeth.hpp` (clang and ld64.lld cannot emit the format) - **no real ld64-produced binary has been run**. Protocol method lists are not read at all.
* **Weak binding**: `weak_bind` streams are decoded and coalesced across loaded images. Not covered: chained-fixup images (no weak bind stream exists there), flat-namespace interposing, definitions inside the shared cache. Load order approximates dyld's.
* **Cocoa** is only an umbrella: a symbol bound to it is looked up in the AppKit/Foundation subsets (9 AppKit classes, host-written Foundation). **No Cocoa compatibility is claimed.** CoreData is not recognised.
* **Recognised-but-unimplemented frameworks** (ApplicationServices, Carbon, IOKit, CoreServices, Security, ServiceManagement) have a short table of honest stubs (`orvyn frameworks`): linking proceeds, calling one ends the program with its name and public prototype. Data symbols (e.g. `kTISPropertyLocalizedName`) and unlisted functions are refused at load. A stub never returns a value.
* **Exceptions**: programs containing `@try/@catch/@finally` link and run while nothing is thrown (the unwinding entry points are stubs). A throw in such a program stops it with a "no unwinder" message; **nothing is ever caught**. The `_OBJC_EHTYPE_*` descriptors exist only so that linking works.
* **Darwin libc**: `getopt` is BSD getopt (no GNU extras, no `getopt_long`); `malloc_size` reports the requested size, not the allocator's rounded size; `___chkstk_darwin` is a register-preserving `ret` (guest stacks are fully committed) and its name comes from Apple's libSystem, not from the available LLVM 23.1.3 driver, which does not emit it.
* **`@synchronized` / associated objects**: correct for the cooperative guest threads (one runs at a time); no parallelism is claimed. Association policies other than ASSIGN/RETAIN/COPY (+NONATOMIC variants) stop the program.
* **External binaries**: none runs. cliclick 5.1 is blocked by 19 missing imports (see [compatibility/CLICLICK-5.1.md](compatibility/CLICLICK-5.1.md)).

## 0.6.8 additions (jq 1.7.1)

* **Guest environment**: time zone is **UTC** (`localtime_r` == `gmtime_r`, `tm_zone` "UTC"); locale is **"C" only** (`setlocale` refuses anything else); no signals (`raise(SIGABRT)` = `abort()`, other signals stop the program); `isatty` is true only for an uncaptured real console.
* **libm / ctype / time**: implemented with the host's C runtime where the contract matches (checked with mathematically exact values), not bit-compared with Apple's libm. `__DefaultRuneLocale`: layout (offset 60) and the alpha/space bits confirmed in jq's code; the other `_CTYPE_*` bits come from Apple's public header as remembered. `rand()` is the FreeBSD/Darwin Park-Miller generator, not compared with a macOS run. `strftime`/`strptime` cover the common conversions only (unknown conversions are copied literally).
* **stdin**: the guest's stdin is empty unless `orvyn run --stdin` is given.
* **Lazy binding** is still eager: an unresolved import that is never called still fails the load (proposal in the validation report).
* **Flat namespace**: symbols are looked up by name in load order; interposing and `DYLD_FORCE_FLAT_NAMESPACE` do not exist.
* **Weak binding** is not applied to chained-fixup images.
* **Objective-C metadata**: protocol method lists (absolute or relative) are not read; no authentic ld64-produced relative-list binary has been run.
* Only **one** external binary has run (jq). That says nothing about other programs.
