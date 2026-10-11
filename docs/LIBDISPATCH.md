# libdispatch subset (ORVYN 0.5.2)

Implemented in `src/dispatch/dispatch.cpp`. A controlled subset of Grand Central Dispatch, **not** libdispatch: no sources, I/O channels, data objects, workloops, QoS overrides,
target queues, suspend/resume.

## Why the cooperative scheduler can host it

Guest threads run one at a time under a fair guest lock ([THREADING.md](THREADING.md)); they switch only at blocking points. Queue state is therefore touched only while holding the lock and
needs no mutex, and nothing in the queue code can race. What had to be added to the scheduler: `GuestThreads::create_native` (a thread whose body is host code that may call guest code through
`Runtime::call_imp` on **its own guest stack**) and `Runtime::call_imp` choosing that stack for non-main threads. Existing thread tests are unchanged.

## Model

| Queue | Executed by | Notes |
|---|---|---|
| main (`_dispatch_main_q`, `dispatch_get_main_queue()`) | **the main guest thread only**, from `dispatch_main()` or from the run loop ([EVENT-LOOP.md](EVENT-LOOP.md)) | blocks never run on another thread, so GUI callbacks stay on the thread that owns the windows |
| serial (`dispatch_queue_create(label, NULL)`) | one worker thread per queue, created on the first asynchronous submission | FIFO, one block at a time |
| concurrent (`DISPATCH_QUEUE_CONCURRENT`) and global (`dispatch_get_global_queue`, four classes) | one worker each; blocks run **serially** (a valid schedule) | barriers are ordinary submissions; `dispatch_apply` iterates inline in order |

* `dispatch_async(_f)`: the block is copied (`_Block_copy` semantics), the queue is retained while an item is pending, the worker runs the block inside an autorelease pool.
* `dispatch_sync(_f)`: the block runs **on the calling thread** once the queue is idle and everything submitted earlier has finished (libdispatch does the same). Onto the **main** queue
  from a worker it waits for the main thread to drain it. Self-deadlocks libdispatch also has are **trapped with a diagnosis**: `dispatch_sync` onto the main queue from the main thread, and onto
  a serial queue from a block already running on it.
* `dispatch_after(_f)`: timers per queue (`dispatch_time` uses a monotonic nanosecond clock; `DISPATCH_TIME_NOW` = 0, `FOREVER` = `~0`).
* `dispatch_once(_f)`: `*predicate` goes 0 → 1 (running; other threads wait) → `~0` (done), the value the header's inline fast path tests.
* `dispatch_group_*` (create, enter, leave, async, notify, wait) and `dispatch_semaphore_*` (create, signal, wait with timeout).
* Objects are Objective-C objects (`OS_dispatch_queue`, `OS_dispatch_group`, `OS_dispatch_semaphore`): ARC and `dispatch_retain/release` use the same counts. A worker holds one reference to its
  queue and retires (releasing it) when the program released the queue and nothing is pending.
* A worker that runs a block which calls `exit()` ends the whole process; `exit` inside any queue unwinds every thread.

## Exported functions (29)

`dispatch_async, dispatch_async_f, dispatch_sync, dispatch_sync_f, dispatch_barrier_async, dispatch_barrier_async_f, dispatch_barrier_sync, dispatch_after, dispatch_after_f, dispatch_time,
dispatch_once, dispatch_once_f, dispatch_main, dispatch_get_global_queue, dispatch_queue_create, dispatch_queue_attr_make_with_qos_class, dispatch_queue_get_label, dispatch_retain,
dispatch_release, dispatch_apply, dispatch_group_create/enter/leave/async/notify/wait, dispatch_semaphore_create/signal/wait`; data: `_dispatch_main_q`, `_dispatch_queue_attr_concurrent`.

## Limits that matter

* Starvation: a block that spins forever without calling a libSystem function holds the guest lock; other queues and the main loop never run.
* The main queue is only drained while the main thread is inside `dispatch_main`, `NSApplication run`, `CFRunLoopRun`/`RunInMode`. Blocks submitted while the main thread computes wait.
* `dispatch_sync` from the main thread onto a global/serial queue works (and the worker is idle), as does `dispatch_group_wait`; a *worker* blocked in `dispatch_sync(main)` needs the main loop to run.
* Thread count: each queue with asynchronous work uses a guest thread (64 maximum, shared with `pthread_create`).

## Tests

`tests/runtime/test_dispatch.cpp` + `dispatch_basic.m` (ARC; main-queue ordering, serial FIFO, worker identity via `pthread_self`, sync after async, semaphores incl. timeout, groups with wait/notify, `once`,
`_f` forms, `apply`, `dispatch_after` ordering and delay, a worker calling `dispatch_sync(main)`, captured objects, leak counter 0, 8 repeated runs) and `dispatch_error.m` (self-deadlocks, NULL block,
bad `once` predicate, unbalanced `group_leave`, `exit()` from a worker block).
