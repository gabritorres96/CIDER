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
