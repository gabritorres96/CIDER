# ORVYN roadmap 0.6 → 1.0

> Status 2026-10-11 (local branch): relative method lists, weak binding, Cocoa umbrella + framework stubs, libc basics, `@synchronized`/associated objects and exception-tolerant linking are implemented and tested on fixtures; **no external binary runs** (cliclick is blocked at binding by 19 imports, see [../compatibility/CLICLICK-5.1.md](../compatibility/CLICLICK-5.1.md)). The version stays 0.5.0 until the acceptance criteria below are met.

Replaces the proposal row for 0.6 in [../ROADMAP.md](../ROADMAP.md) with a plan based on the measurements in this research. **No dates are promised.** Each milestone has an acceptance test that can be run; none is claimed before it passes. The version stays **0.5.0** until a milestone is accepted. Item numbers refer to [TOP-20-COMPATIBILITY-IMPROVEMENTS.md](TOP-20-COMPATIBILITY-IMPROVEMENTS.md).

| Version | Theme | Content | Acceptance |
|---|---|---|---|
| **0.6** Drawing foundations | finish what 0.6.1–0.6.2 started and clear first-error blockers | guest CoreGraphics + `drawRect:` (done for fixtures); items 1, 2, 3, 4, 9, 14 | a clang-built macOS 11+ ObjC fixture (relative method lists, weak refs, `@synchronized`, stack protector) links and runs; `Cocoa` umbrella resolves |
| **0.6.x** Honest stubs | stub-library mechanism (item 5) | named call-time traps; `compatibility` never reports support for stubbed APIs | a fixture calling a stubbed symbol ends with a diagnostic naming it |
| **0.7** First real binary + launcher | items 6, 7, 10, 11 (core), 15 | **cliclick `-V`/`-h` runs unmodified** (stages 0–2); `dlopen` fixture; hash-table dictionary; NSBundle/NSUserDefaults/NSTimer. **Launcher UI** (below) | cliclick prints its version and help and exits with the documented codes; input-device commands fail with a clear message; launcher opens `.app`/`.dmg`, shows analysis and verdict |
| **0.8** Objective-C GUI app | items 12, 13, 16, 17 | images, menus, status bar, dispatch sources, terminators | Flycut or KeepingYouAwake (both MIT) starts, shows its status item and menu, and quits on request |
| **0.9** C++ and text | items 8, 18 (basic) | libc++ subset, font mapping, `NSTextView` plain-text editing | an app that needs libc++ links; a plain-text editing view works with the keyboard |
| **1.0** | reproducible compatibility with a **defined set** of real applications | published compatibility list with versions, hashes, tested behaviors, known failures; security model reviewed per [LIMITATIONS.md](../LIMITATIONS.md) | the list's applications pass their documented scenarios on a clean Windows machine; CI on GitHub green |
| After 1.0 (not committed) | exceptions and Swift | items 19, 20; Swift PoC first (261-import 5.0 runtime) | decided on evidence; SwiftUI remains outside any horizon |

## Launcher UI (0.7)

Goal: a native Windows window to open an `.app` or `.dmg`, see the static analysis (architectures, dependencies, verdict, missing libraries), run it, and read logs. Design inspired by the macOS feel (clean, rounded, light/dark following Windows' `AppsUseLightTheme`), **without Apple logos, icons or fonts** ([THIRD-PARTY-LICENSE-REVIEW.md](THIRD-PARTY-LICENSE-REVIEW.md) §3). The verdict text keeps the project's rule: "analysis compatible" never means "will run". The `launcher/` directory currently holds only a README. Built on the existing CLI/JSON output so there is one source of truth.

## Gates and rules

1. A milestone starts only when the previous acceptance test is reviewed.
2. Every new API: fixture that fails before/passes after, exact-registry update, ASan clean; fuzz where input is untrusted.
3. Copying third-party code only after the licence review is closed ([THIRD-PARTY-LICENSE-REVIEW.md](THIRD-PARTY-LICENSE-REVIEW.md) §4); default is reimplementation ([REUSE-ADAPTATION-MATRIX.md](REUSE-ADAPTATION-MATRIX.md)).
4. The worker is not a sandbox: a real third-party binary runs only after the security note for that stage is reviewed.
5. GitHub Actions has never run; validating CI is part of 0.7, not a precondition for research.

## Risks

* Effort estimates are unmeasured.
* Only one machine, Windows x86_64 and one monitor have been tested.
* The expected output of real binaries cannot be compared without a Mac (PENDING VALIDATION).
