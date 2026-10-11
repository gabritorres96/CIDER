# Compatibility model

## Status values

| Status | Meaning in 0.1 |
| --- | --- |
| not analyzed | nothing was examined |
| analysis compatible | the x86_64 slice parsed, is for macOS, not encrypted; bundled dependencies resolve. **This does not mean the program runs.** |
| pending dependencies | at least one required (non-weak) dependency could not be resolved inside the bundle |
| partial execution / executable / executable with issues | reserved: only a runtime can produce them. **Never emitted by 0.1** |
| not compatible | no x86_64 slice, slice invalid, non-macOS platform, encrypted image, iOS-only bundle |

`CompatReport::execution_confirmed` is always `false` in 0.1 and `cider compatibility` prints
"Execution support not yet confirmed" every time.

## Dependency classes

`system` (`/usr/lib`, `/System/Library`: provided by macOS, **not provided by CIDER yet**),
`bundled` (resolved to a file inside the bundle), `missing`, `external` (absolute path outside
system directories), `outside-bundle` (resolved but escaping the bundle). Weak dependencies that are
missing do not downgrade the verdict. The closure follows bundled libraries (max 512 images,
cycle-safe) using the loader's rpaths plus its ancestors'.

## What `cider run` adds (0.2)

`cider run` is the only place where execution is observed. Its result is a fact about *that run of that binary*, not a
compatibility rating: `compatibility` still reports "execution not confirmed" for every file. Binaries known to run today:
only the controlled fixtures in `tests/fixtures/exec` (see EXECUTION.md). Every real application tried is refused at
validation with a precise missing-library message.

## Not implemented

The local compatibility database and per-application profiles (`compatibility/profiles`) are
**not implemented**: there are no runtime options to put in a profile yet. They are added together
with the first option they control.

## Tested applications

Real applications **executed**: none. In 0.1.1 three real apps (Rectangle, Stats, MonitorControl) were analysed statically
and extracted from their DMGs; in 0.2 `cider run` refuses them (exit 12: Foundation/AppKit/libobjc/Swift runtime/embedded
frameworks are not available). Programs executed successfully: only the LLVM-built fixtures listed in EXECUTION.md.
No compatibility with any real application is claimed.

## What `cider run` adds (0.3)

Dependent Mach-O dylibs are searched and loaded (DYNAMIC-LINKING.md), so a binary whose dependencies are *shipped next to it* as real
x86_64 Mach-O dylibs and that only imports the functions listed in DARWIN-RUNTIME.md can run. Anything importing Foundation, AppKit,
libobjc, libc++/libc++abi, the Swift runtime or other system libraries is refused (`Missing library`, exit 12). No compatibility with
general macOS applications is claimed.

## What `cider run` adds (0.4)

Programs that import `libobjc.A.dylib` and, optionally, the **Foundation subset** listed in [FOUNDATION.md](FOUNDATION.md) can run, plus `FILE` streams,
`qsort`/`bsearch`/`atexit` and pthreads (cooperative). Compatibility is defined by the symbol and selector tables, not by an application list: a program that
imports any other Foundation, CoreFoundation, AppKit, libc++, Swift or system symbol is refused (exit 12) with the symbol name, and a message that the subset
does not implement ends the guest with `unrecognized selector`. The only Objective-C programs known to run are the clang-built fixtures in `tests/fixtures/objc`.
**No compatibility with Cocoa, with real macOS applications or with Apple's Foundation as a whole is claimed**; Rectangle, Stats and MonitorControl remain refused.

## What `cider run` adds (0.5)

Programs that call **Blocks, libdispatch, CoreFoundation basics, CoreGraphics geometry, message forwarding and the AppKit subset** can run: [BLOCKS.md](BLOCKS.md), [LIBDISPATCH.md](LIBDISPATCH.md),
[FOUNDATION.md](FOUNDATION.md), [COREGRAPHICS.md](COREGRAPHICS.md), [APPKIT.md](APPKIT.md). A program that links AppKit opens **real Windows windows**. The known working programs are the clang-built fixtures in
`tests/fixtures/objc` and `tests/fixtures/appkit`. `@try/@catch` programs are refused (no unwinder); uncaught `NSException`s end the guest with the macOS message.

### Real applications re-analysed in 0.5 (static analysis only, nothing executed)

`tools/app_coverage.py` reads the x86_64 slice of each application with `llvm-objdump` and compares its imports with what CIDER implements (a host function or class exists; not verified behaviour).

| Application | Imported symbols | CIDER implements | Swift runtime libraries linked | Verdict |
|---|---:|---:|---:|---|
| CotEditor 7.0.9 | 4,371 | 86 | 26 | refused (exit 12): missing libraries incl. the whole Swift runtime and SwiftUI; [COTEDITOR-COMPATIBILITY.md](COTEDITOR-COMPATIBILITY.md) |
| Rectangle 2.0.3 | 1,208 | 94 | 19 | refused: Swift (342 libswiftCore imports) + SwiftUI (344) |
| Stats 3.0.20 | 623 | 25 | 20 | refused: Swift (386 libswiftCore imports) |
| MonitorControl 4.4.0 | 1,434 | 26 | 22 | refused: Swift (1,086 libswiftCore imports) |

All four are Swift applications. The Swift runtime, not AppKit, is the first blocker; **none of them runs and none is claimed to**.

## What 0.6.1 - 0.6.2 add (drawing)

A controlled Objective-C Mach-O program can now draw with CoreGraphics from `-drawRect:` into a real window. Evidence is limited to the fixtures `cg_draw_test` (scene and
Redraw button), `cg_abi_test` (struct passing), `cg_lifecycle` (window cycles, close during drawing, stale contexts) and `cg_unimplemented` (refusal). **No real application
draws through CIDER yet**: CotEditor, Rectangle, Stats and MonitorControl remain Swift applications and are refused at load; nothing in this phase changes that
([COTEDITOR-COMPATIBILITY.md](COTEDITOR-COMPATIBILITY.md)). See [COREGRAPHICS-RENDERING.md](COREGRAPHICS-RENDERING.md) and [APPKIT-DRAWING.md](APPKIT-DRAWING.md).
