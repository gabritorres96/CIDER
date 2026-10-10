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

## Not implemented

The local compatibility database and per-application profiles (`compatibility/profiles`) are
**not implemented**: there are no runtime options to put in a profile yet. They are added together
with the first option they control.

## Tested applications

None. No real macOS application has been run or even analysed by the automated tests; fixtures are
synthetic (see TESTING.md). No compatibility with any real application is claimed.
