# Execution (CIDER 0.2)

> **Status: experimental.** CIDER 0.2 executes **controlled Mach-O x86_64 test programs** built with
> clang + ld64.lld that import only a handful of libSystem functions. It does **not** run macOS
> applications: every real application tried (Rectangle, Stats, MonitorControl from the 0.1.1
> validation) is refused with a clear "library not available" error because it needs Foundation,
> AppKit, libobjc, Swift runtime libraries... None of that exists yet.
> Do not read "it runs the fixtures" as "macOS compatibility".

## What happens when you run `cider run hello`

```
cider run (supervisor)                         worker process (cider.exe __worker, job-restricted)
 1 static analysis: macho::parse_file           ─┐
 2 validation: validate_for_execution            │ refuse early (exit 11 / 12), no process is started
 3 start the worker (Job Object, timeout)  ─────►│
                                                 4 load_macho():
                                                    parse → validate → plan_layout
                                                    decode fixups → resolve symbols       (before any memory is reserved)
                                                    VirtualAlloc(MEM_RESERVE) at 0x100000000 (else anywhere + slide)
                                                    commit RW, copy segments, zero-fill
                                                    apply rebases + binds
                                                    VirtualProtect: __TEXT r-x, __DATA rw-, __DATA_CONST r--
                                                 5 guest stack (8 MiB, guard page) + argv/envp/apple block
                                                 6 cider_enter_guest(): switch to the guest stack and CALL main
                                                 7 main returns / exit() → result file → cleanup (RAII)
 8 read the result file, print "Guest exit code: N"  ◄──
```

All numbered steps log real events as `[CIDER][LOADER|MEMORY|DYLD|ABI|EXECUTION|PROCESS] ...`.

## The entry contract (LC_MAIN)

* `LC_MAIN.entryoff` locates `main` itself (linked without `crt1.o` stubs). CIDER calls it exactly like dyld does:
  `exit(main(argc, argv, envp, apple))`.
* Registers at the call: `rdi=argc rsi=argv rdx=envp rcx=apple`, System V ABI, `rsp` 16-byte aligned before
  the `call`. `argv[0]` is the path as typed; `envp = ["CIDER_RUNTIME=1"]` (the host environment is **not**
  passed); `apple = ["executable_path=<argv[0]>"]`.
* The 32-bit value returned in `eax` (or passed to `exit`) is the guest exit code. `exit_status` in JSON is
  its low 8 bits (POSIX semantics).
* `LC_UNIXTHREAD` binaries (no `LC_MAIN`) are refused.

### Which parts are the real Darwin protocol, which are simplified

| Aspect | Status |
| --- | --- |
| `LC_MAIN` entry, `main(argc, argv, envp, apple)`, return → `exit` | real dyld protocol |
| Image layout (`__TEXT/__DATA_CONST/__DATA`, Mach-O header mapped in `__TEXT`, preferred base `0x100000000`) | real |
| Rebase/bind/lazy-bind opcodes (`LC_DYLD_INFO_ONLY`) | real, decoded and verified against `llvm-objdump` |
| Lazy binding | **simplified**: every lazy pointer is bound eagerly at load; `dyld_stub_binder` is a trap |
| `__PAGEZERO`, `__LINKEDIT` | **not mapped** (the null page region is unmapped, `__LINKEDIT` is not needed at run time) |
| dyld itself, `libSystem` initializers, `__mod_init_func`, TLS, `dyld_*` APIs, `_NSGetArgv` & friends | **absent** (binaries needing them are refused or fail to resolve) |
| Stack | host-allocated, 8 MiB (or `LC_MAIN.stacksize`), guard page below |
| Signals, exceptions/unwinding, threads, `gs`-relative TLS, raw `syscall` | **not supported** |

## ABI bridge

Guest code is compiled for System V AMD64; the host (CIDER) is Windows x64. See [ABI.md](ABI.md) for the
differences. Implemented (`src/abi`):

* **Entry** (`cider_enter_guest`, MASM): saves rbx rbp rdi rsi r12-r15 xmm6-xmm15, switches to the guest stack,
  loads rdi/rsi/rdx/rcx, `call`s the guest, restores everything. Tested by clobbering every one of those
  registers in the guest and checking the host values afterwards (`test_abi`).
* **Calls from the guest into CIDER** (`cider_guest_thunk` + 32-byte per-symbol stubs): rdi,rsi,rdx,rcx →
  rcx,rdx,r8,r9; 5th/6th integer arguments → Windows stack slots; the Windows function runs on the **host**
  stack (not on the guest stack); `double` in xmm0/xmm1 unchanged.
* **Not supported**: more than 6 integer arguments, variadic callees (`printf`: `al` is ignored), struct
  returns, mixed integer/`double` signatures, `long double`, calls from the host back into guest code
  (callbacks), re-entrancy.

## libSystem subset

Implemented (and tested **from guest code**): `puts putchar write exit getpid getenv strlen strcmp strncmp memcpy
memmove memset memcmp abs labs malloc calloc realloc free sqrt fabs floor ceil pow` and a trapping
`dyld_stub_binder`.

* `write` accepts fd 1 and 2 only; output goes to the worker's real stdout/stderr (the supervisor captures it with
  `--json`). `puts`/`write` are unbuffered Win32 `WriteFile` calls.
* `malloc` & co. use a **private Win32 heap** per run: failure returns `NULL`, `free`/`realloc` of foreign
  pointers are refused, and everything is released when the run ends.
* `exit` marks the call as exited; the thunk then returns directly to the host (no `longjmp`).

**Missing** (a binary importing any of them is refused at load time with `Unresolved symbol`): `printf` family,
stdio, files, `mmap`, time, `sin/cos/...` (only the five math functions above), threads, signals, `fork/exec`,
locale, the Objective-C/Swift/C++ runtimes, every framework.

## Memory safety policy

* No page is ever writable and executable: segments are mapped RW, filled, fixed up, then switched with
  `VirtualProtect`; RWX segments are rejected (`test_loader`, `test_memory` check every page with `VirtualQuery`).
* Image span ≤ 1 GiB, stack ≤ 64 MiB, all arithmetic overflow-checked, entry point must lie in an executable
  mapped segment, symbols are resolved **before** anything is mapped, and every failure path releases the
  reservation (RAII; verified by checking the address range is free afterwards).

## Process model and its limits

`cider run` starts the loader/guest in a **separate worker process** (`cider.exe __worker`, internal) assigned to a
Windows Job Object: kill-on-close, one active process, per-process memory limit (default 4 GiB), UI
restrictions, no error-reporting dialog, and a wall-clock timeout (`--timeout`, default 60 s). A crash of the
guest (access violation, illegal instruction, stack overflow...) ends the worker only; CIDER reports
`guest-crashed` with the NTSTATUS.

**This is not a sandbox.** Guest machine code runs natively with the user's full rights: it can issue raw NT
system calls, touch any memory of the worker, read files (through syscalls), open network connections... The
job object limits resources and process creation, nothing more. Never run binaries you do not trust. No
downloaded application was executed during development.

## Running and reproducing

```
cider run tests\fixtures\exec\bin\ret42                    # Guest exit code: 42
cider run tests\fixtures\exec\bin\hello                    # Hello from macOS!
cider run tests\fixtures\exec\bin\args -- first second     # guest sees argc=3
cider run <program> --json                                 # runtime status and guest result as separate objects
cider run <program> --verbose --timeout 10
```

Exit codes of `cider run`: `0` the guest ran to completion (its code is printed / in `guest.exit_code`);
`10` internal error; `11` file incompatible or unsupported; `12` dependency or symbol not available;
`13` load failure; `14` guest failure (crash, runtime trap, timeout); `2` usage error.

Rebuild the fixtures (needs the portable LLVM, see BUILDING.md): `tools\llvm-env.cmd tools\gen-exec-fixtures.cmd`.
Run the tests: `ctest --test-dir build -C Debug --output-on-failure` (suites `test_memory`, `test_abi`,
`test_loader`, `test_entry`, `test_symbols`, `test_runtime`, `test_execution`, `test_execution_errors`).

### Evidence that the instructions come from the Mach-O file

* The fixtures are Mach-O files linked by `ld64.lld`; `llvm-objdump` snapshots in `tests/fixtures/exec/golden`
  (private headers, symbols, disassembly, rebase/bind tables) are compared with CIDER's parse.
* `ret42`'s `main` is `push rbp; mov rbp,rsp; mov eax,42; pop rbp; ret`: the bytes mapped at the entry address equal
  the file bytes (`test_loader`), and the disassembly golden shows `movl $0x2a, %eax`.
* Patching that immediate in a copy of the file changes the guest exit code to 77, -1, 200 or 0 (`test_execution`);
  patching a string in the data changes the printed text. Running ret42's bytes under another name still gives 42.
* Patching `ud2` over `main` makes the worker die with `0xC000001D` (illegal instruction) in the supervised test.

## Executables that can work today

Freestanding or minimal C programs for macOS x86_64, linked with classic dyld info (not chained fixups), non-TLS,
without static initializers, importing only the functions listed above from `/usr/lib/libSystem.B.dylib`.
Nothing else is claimed.

## 0.3 additions

* The loader is now a **multi-image dynamic linker**: [DYNAMIC-LINKING.md](DYNAMIC-LINKING.md). Initializers run before `main`.
* ABI bridge gained raw/TLV thunks: [ABI.md](ABI.md). libSystem gained the printf family, files, time: [DARWIN-RUNTIME.md](DARWIN-RUNTIME.md).
* Chained fixups and TLS: [TLS.md](TLS.md). `cider run --root <dir>` sets the guest's `/`.
* Guest stdout/stderr reach a real console (fix of the 0.2 `CREATE_NO_WINDOW` regression; ConPTY test).
* Evidence that the instructions come from the Mach-O (not a PE): the exit code 42 of `tests/fixtures/dylib/bin/one/one` is computed by
  `lea eax,[rdi+rsi]` inside `libmath.dylib`; replacing those 3 bytes in a copy changes the result to 22 (`test_linker`), and `llvm-objdump -d`
  shows the same instruction. Both files are `Mach-O 64-bit x86_64` (`magic 0xfeedfacf`), neither contains a PE header.

## 0.4 additions

* **Objective-C**: `cider run` links `libobjc.A.dylib` (80 exported functions), parses Apple objc2 metadata, dispatches `objc_msgSend` and logs
  `[CIDER][OBJC] Runtime initialized`, `Class X registered`, `Selector s resolved`, `Message dispatched: -[X s]` — see [OBJECTIVE-C-RUNTIME.md](OBJECTIVE-C-RUNTIME.md).
  `cider run tests\fixtures\objc\bin\objc_calc\objc_calc` prints `Guest exit code: 42` (the 42 is computed by `-[Calculator add:to:]`, compiled Objective-C).
* **Foundation subset**: `cider run tests\fixtures\objc\bin\foundation_hello\foundation_hello` prints `CIDER result: 42` (NSLog, stderr) and `Guest exit code: 42` — [FOUNDATION.md](FOUNDATION.md).
* **Callbacks** (`qsort`, `bsearch`, `atexit`): [CALLBACKS.md](CALLBACKS.md). **Threads** (`pthread_*`, cooperative): [THREADING.md](THREADING.md).
  **FILE streams** (`fopen` … `fclose`, opaque `FILE*`, binary mode, confined to `--root`): [DARWIN-RUNTIME.md](DARWIN-RUNTIME.md).
* `GuestConfig`/`Outcome` gained `objc_live_objects` (objects still alive at exit).

## 0.5 additions

* **AppKit programs open real windows.** `cider run tests\fixtures\appkit\bin\hello_window\hello_window` shows a window "CIDER — Hello from macOS" (two labels, a "Clicks: N" counter, buttons "Click me" and "Close").
  "Click me" runs `-[AppController buttonClicked:]` (compiled into the Mach-O), increments the counter kept in the program, updates the label and logs `Button clicked! count=N` with `NSLog`. "Close" (or the title bar X) closes the window,
  the delegate ends the application, the event loop returns and the process exits with status 0 (`Application terminating` is the last log line). Programs that link AppKit have their default timeout lifted (24 h) unless `--timeout` is given.
* New logs: `[CIDER][OBJC] AppKit: win32 GUI backend ready`, `AppKit: event loop started`, `AppKit: event loop ended`.
* `Outcome` / in-process runs gained `gui_open_windows_at_exit` and `gui_native_objects_after`; `GuestConfig::gui_backend` ("win32" default, "offscreen" for tests) and `gui_script`.
* Blocks, libdispatch, CoreFoundation, CoreGraphics geometry, forwarding: [BLOCKS.md](BLOCKS.md), [LIBDISPATCH.md](LIBDISPATCH.md), [FOUNDATION.md](FOUNDATION.md), [COREGRAPHICS.md](COREGRAPHICS.md), [OBJECTIVE-C-RUNTIME.md](OBJECTIVE-C-RUNTIME.md).
* `cider sdk-scan <dir|file.tbd> [--json]` compares Apple text stubs with what CIDER implements ([APPLE-SDK-RESEARCH.md](APPLE-SDK-RESEARCH.md)).
