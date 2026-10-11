# Guest threads (ORVYN 0.4.2)

Mach-O programs can create threads with the pthread API. ORVYN implements them with **real Windows threads under a cooperative scheduler**: at any
moment exactly one guest thread executes guest code or a host function. This is an honest, deliberately small model, not a general threading
implementation.

## Model

* Each guest thread is an OS thread with its own guest stack (default 1 MiB, from `pthread_attr_setstacksize` when set), its own chain of
  callback records, its own `errno`, its own thread-local storage blocks (see [TLS.md](TLS.md)), its own `pthread_setspecific` values and its own
  Objective-C autorelease pools.
* A **fair ticket lock** (the "guest lock") serialises all guest execution. The lock is released only at blocking or yielding points: `pthread_join`,
  mutex contention, `pthread_cond_wait`/`timedwait`, `sleep`/`usleep`/`nanosleep`, `sched_yield`, console and file reads, thread start and end. The ABI state
  that the bridge keeps in globals (current call record, callback depth, stack bound) is saved and restored when the lock changes hands.
* Maximum 64 threads (`pthread_create` returns `EAGAIN` beyond that); a worker process can also be ended by `--timeout`.
* The Windows TEB, `%gs` and `TlsAlloc` slots are **never** touched. A `pthread_t` is an opaque number, not a pointer. `pthread_mutex_t` and `pthread_cond_t`
  store only a signature and a host-table index inside the guest's storage; attributes are small host records. Nothing from the guest is ever handed to a
  Windows synchronisation object.

## Supported API

`pthread_create`, `pthread_join`, `pthread_detach`, `pthread_self`, `pthread_equal`, `pthread_exit`, `sched_yield`;
`pthread_attr_init/destroy/setstacksize/getstacksize/setdetachstate`;
`pthread_mutexattr_init/destroy/settype/gettype`, `pthread_mutex_init/destroy/lock/trylock/unlock` (normal, error-checking and recursive types);
`pthread_cond_init/destroy/wait/timedwait/signal/broadcast`; `pthread_once`; `pthread_key_create/delete`, `pthread_getspecific/setspecific`.
Errors follow Darwin's errno values (`EDEADLK` = 11, `EBUSY` = 16, `ETIMEDOUT` = 60, `EAGAIN` = 35 …). Misuse of an object ORVYN did not create (bad
signature, destroyed mutex, foreign `pthread_t`) returns an error or ends the guest with a diagnosis; it never dereferences guest data as a host pointer.

`exit()` from any thread requests process exit: every other thread unwinds at its next wake-up, `atexit` handlers run on the exiting thread, and the exit code is
the one passed. Returning from `main` ends the process even if threads are still running (as on Darwin).

## Limits (read these before trusting a result)

* **No parallelism.** A CPU-bound guest thread never runs concurrently with another one.
* **Starvation:** a thread that spins on shared memory without ever calling a libSystem function holds the lock forever and the others (and `pthread_join`
  waiters) never run. The only way out is the worker timeout. Programs that rely on preemption (spin locks, busy waits, lock-free algorithms) do not work.
* Data races can only manifest at yield points, so a program with unprotected data races may "work" here and fail elsewhere. ORVYN is therefore **not a race
  detector**; there is no ThreadSanitizer coverage and none is claimed.
* Not implemented: read/write locks, barriers, semaphores (`dispatch_semaphore`, `sem_*`), `pthread_cancel`, signals, thread priorities and affinity,
  `pthread_setname_np`, `pthread_getattr_np`-style introspection, process-shared mutexes, `fork`.
* Thread-local storage works for the Mach-O TLV descriptors only; `__thread_init` (C++ dynamic initialisers) is still refused.
* Stack size and thread count are bounded.

## Tests

`tests/runtime/test_threads.cpp` (13 tests, real Mach-O fixtures `thread_basic thread_mutex thread_race thread_tls thread_sync thread_exit`): create/join/self/equal/exit and
error codes, four threads with a mutex counting exactly, the same program without the mutex losing updates (proof of real interleaving), per-thread TLS from the
template, condition variables + mutex kinds + `pthread_once` + per-thread `errno`, `exit()` from a non-main thread, returning from `main` while threads run, the
thread limit (`EAGAIN`), the host's Windows thread state untouched (TEB/`gs`), no leaked threads or handles over repeated runs, deterministic scheduling over many
runs, opaque handles and Darwin mutex storage layout, and a fault in a guest thread killing only the worker. The whole suite also runs under AddressSanitizer.
