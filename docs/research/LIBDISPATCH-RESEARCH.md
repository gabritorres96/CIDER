# libdispatch research

Question: what is missing for applications that depend on real concurrency, and should ORVYN extend its own subset, adapt a portable implementation, use parts of the open-source code, or combine them?
Labels: **VERIFIED**, **INFERRED**, **HYPOTHESIS**, **PENDING VALIDATION**.

## 1. Sources

| Source | Version | License | Notes |
|---|---|---|---|
| `apple-oss-distributions/libdispatch` | `libdispatch-1542.0.4`, `2361ffb`, 2025-10-16 (= macOS 26.0) | Apache 2.0 (`LICENSE`) | the Darwin implementation: `src/queue.c` 9,043 lines, `src/source.c` 1,523, `src/semaphore.c` 407, plus `io.c`, `data.c`, `apply.c`, `event/`, `firehose/`, `mach.c` (Mach ports), `eventlink.c`, workqueue integration |
| `swiftlang/swift-corelibs-libdispatch` | `main` `bcae657`, 2026-10-09 | Apache 2.0 | the portable fork used by Swift on Linux/Windows: `src/queue.c` 7,635 lines, `src/event/event_windows.c` **1,005 lines**, `_WIN32` branches in `queue.c` (53 occurrences), `shims/generic_win_stubs.h` |
| ORVYN | `src/dispatch/dispatch.cpp` (583 lines), `docs/LIBDISPATCH.md` | own | 29 functions + 2 data symbols |

## 2. What ORVYN's subset is (VERIFIED, `docs/LIBDISPATCH.md`, registry)

Main/serial/concurrent/global queues, `dispatch_async(_f)`, `sync(_f)`, barriers (as ordinary submissions), `after(_f)`, `once(_f)`, `apply` (inline, in order), groups, semaphores. **Concurrent and global queues run serially**
(one worker each). Guest threads are cooperative under one guest lock; a block that spins without calling libSystem starves everything else; the main queue drains only while the main thread is in `dispatch_main`, `NSApplication run`, `CFRunLoopRun` or `RunInMode`.

### Why there is no real parallelism — and why libdispatch is not the cause (VERIFIED)

`include/orvyn/abi.hpp` documents the host↔guest call bridge as "**Single threaded**: only one guest call may be active at a time"; the assembly bridge keeps `orvyn_current_call` as a global. Guest threads are host threads that take turns
(`src/libsystem/guest_threads.cpp`). So even a perfect libdispatch could only run blocks one at a time. Real concurrency needs a **per-thread bridge state** (`AbiThreadState` already exists as the save/restore unit when a thread yields) *plus* a decision about thread
safety of every host function the guest can call (the Objective-C runtime, Foundation payload store, AppKit all assume one thread). That is a project of its own, larger than libdispatch.

## 3. API gap against the real libdispatch

| Area | Real API | ORVYN | Evidence / who needs it |
|---|---|---|---|
| Queues | `dispatch_queue_create_with_target`, `dispatch_set_target_queue`, `dispatch_queue_set_specific/get_specific`, `dispatch_get_specific`, `dispatch_get_current_queue` (deprecated), `dispatch_queue_get_qos_class`, `dispatch_assert_queue(_not)`, `dispatch_suspend/resume/activate`, `dispatch_queue_attr_make_initially_inactive/with_autorelease_frequency` | `create`, `get_label`, `attr_make_with_qos_class` | Foundation, AppKit and every Swift `DispatchQueue` use target queues, `assert_queue`, `specific` |
| Sources | `dispatch_source_create` (`TIMER`, `READ`, `WRITE`, `SIGNAL`, `PROC`, `VNODE`, `DATA_ADD/OR`, `MACH_*`), `set_event_handler`, `set_cancel_handler`, `resume`, `cancel`, `dispatch_source_set_timer` | **absent** | timers via `DispatchSource.makeTimerSource` are the most common modern timer in apps; `src/source.c` is 1,523 lines |
| Blocks API | `dispatch_block_create(_with_qos_class)`, `dispatch_block_perform`, `dispatch_block_wait/notify/cancel/testcancel` | absent | Swift's `DispatchWorkItem` is built on it |
| Time | `dispatch_walltime`, `dispatch_time` ✓ | `dispatch_time` only | easy |
| Data / IO | `dispatch_data_*`, `dispatch_io_*`, `dispatch_read/write` | absent | `src/data.c` 745, `io.c` 2,839 lines |
| Workloops, event links, mach channels | `dispatch_workloop_*`, `dispatch_mach_*` | absent | rare in apps; Mach-specific |
| `dispatch_apply` | parallel | inline, in order (a valid schedule) | |
| Autorelease | per-block pools | per-block pool ✓ | |
| Main thread integration | `dispatch_main`, `CFRunLoop` main queue source | ✓ via the shared run loop ([EVENT-LOOP.md](../EVENT-LOOP.md)) | |

Observed demand (CotEditor, VERIFIED from its import list): `libswiftDispatch` 13 imports and one libSystem class import `_OBJC_CLASS_$_OS_dispatch_queue`. The other three applications' complete import lists were not re-extracted in this session
(their bundles are no longer on disk), so a frequency ranking across apps is **PENDING**.

## 4. Options

| | A. extend ORVYN's subset | B. adapt `swift-corelibs-libdispatch` wholesale | C. use selected parts of Apple's/Swift's libdispatch source | D. hybrid: keep ORVYN's scheduler and queue model, add features, borrow algorithms |
|---|---|---|---|---|
| What | add sources, suspend/resume, target queues, specific, block API, `dispatch_data` | build the Windows port as a host DLL/static lib and export its symbols to the guest | take `source.c` timer/coalescing logic, `data.c`, `block.cpp` semantics | A, informed by C where it saves work |
| Pros | stays inside the cooperative guest model; each feature testable with a fixture; no new thread model | complete feature set, real concurrency, maintained upstream | proven semantics for the subtle parts (timer leeway, coalescing, cancellation ordering) | lowest risk, incremental |
| Cons / risks | semantics must be rediscovered; effort proportional to features | its workers are **host threads that would call guest blocks in parallel**: impossible with the single-threaded bridge (section 2); dispatch objects have no Objective-C `isa`, but guests treat them as ObjC objects under ARC (`OS_dispatch_queue` class, `objc_retain`) → would need wrapping; layout of `dispatch_object_s` is private but `_dispatch_main_q` and `dispatch_once` predicate are read inline by guests; pulls the Blocks runtime, `libpthread` shims, Windows event machinery (1,005 lines) | licence is clean (Apache 2.0) but code is entangled with `_dispatch_*` internals; extraction is costly | needs discipline to keep the borrowed pieces small and attributed |
| License | own code | Apache 2.0: attribution and NOTICE obligations, modifications marked | Apache 2.0 | Apache 2.0 for borrowed parts |
| ABI compat | full control | internal layouts differ from what the guest's inline code expects; verified only for exported functions | n/a | full control |
| Performance | adequate for UI apps | best | n/a | adequate |
| Maintenance | own | tracks upstream (+ ongoing adaptation) | own after copy | own |
| Difficulty | M per feature | **H** (parallel guest execution first) | M/H | M |

### Recommendation (INFERRED from the evidence above; PENDING a prototype)

**D**: extend the existing scheduler in this order, each with a clang-built fixture and an exact-registry update: (1) `dispatch_source` timers + `suspend/resume/cancel`, (2) target queues, `specific`, `assert_queue`, (3) the block API, (4) `dispatch_data`, (5) `dispatch_io` only when an application needs it.
Use `source.c` and `swift-corelibs-libdispatch/src/event/event_windows.c` as *design references* (how timers map to Windows waitable timers and how the Windows port waits on sockets with the thread pool), not as linked code.
Revisit **B** only together with a thread-safe bridge: parallel guest execution is the prerequisite, and it affects far more than dispatch. Do **not** replace libdispatch during this research phase.

## 5. Test plan for the first step (not implemented)

* Fixture `dispatch_source.m`: a 10 ms repeating timer source on a serial queue fires N times, is suspended, resumed, cancelled; the cancel handler runs once; nothing fires after cancel; leak counter 0.
* Edge cases from the real source: `dispatch_source_set_timer` with `DISPATCH_TIME_FOREVER` interval (one-shot), leeway, `dispatch_resume` on an already-resumed source is a crash on Apple (`over-resume`): ORVYN must trap with a diagnosis, not ignore.
