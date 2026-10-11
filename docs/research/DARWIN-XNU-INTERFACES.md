# Darwin / XNU interfaces user programs expect

Goal: identify the **minimum** kernel-level services ORVYN must simulate or adapt. Porting XNU is out of scope and not proposed.
Labels: **VERIFIED**, **INFERRED**, **HYPOTHESIS**, **PENDING VALIDATION**.

## 1. Sources

`apple-oss-distributions/xnu`, tag `xnu-12377.1.9` (`f6217f8`, 2025-10-16; macOS 26.0), APSL 2.0. Cloned **sparse** (`osfmk/mach`, `libsyscall/mach`; the `bsd/` tree was not fetched, so the BSD syscall table was *not* read in this research — PENDING). Reference only; nothing copied.
Relevant files read: `osfmk/mach/message.h`, `mach_traps.h` (`mach_reply_port` `:89`, `thread_self_trap` `:93`, `mach_msg_trap` `:97`, `mach_msg_overwrite_trap` `:118`, `task_self_trap` `:321`), `port.h`, `mach_port.defs`, `task.defs`, `thread_act.defs`, `vm_map.defs`, `semaphore.defs`, `clock.defs`, `host_info.h`, `task_info.h`, `thread_info.h`, `mach_time.h`, and `libsyscall/mach/{mach_msg.c, mach_port.c, mach_init.c}`.

## 2. The central observation (VERIFIED against real import lists)

User programs reach the kernel through **libSystem wrappers** (`libsyscall`) and, above all, through **Apple frameworks** (Foundation, AppKit, CoreFoundation, `libdispatch`, `libobjc`) that use Mach ports and XPC internally. In the four analysed applications:

* CotEditor (4,371 imports): **no `mach_*` symbol** in any library. Its only low-level hardware import is `IOKit` (`IOServiceMatching`, `IOServiceGetMatchingService`, `IORegistryEntryCreateCFProperty`, `IOObjectRelease`).
* Stats (623 imports) is a system monitor: its coverage report shows `IOKit`/system libraries not in its top lists; direct `host_*`/`task_*` use was *not* re-extracted in this session (PENDING: the full import list of Stats is no longer on disk; the earlier report keeps only samples).
* Rectangle (1,208): `ApplicationServices` (`AXIsProcessTrusted`, `AXObserver*` — Accessibility API), `Carbon` (`CopySymbolicHotKeys`, `GetEventDispatcherTarget` — global hot keys), `CGEvent*` (event taps), `CFMachPortInvalidate` (a CoreFoundation call that wraps a Mach port).
* MonitorControl: `CGDisplayModelNumber`/`VendorNumber`, `IOKit`/DDC via private frameworks (`OSD`).

So the Mach layer matters for ORVYN **only where a guest calls it directly or an ORVYN-hosted framework must emulate behavior built on it**. Because ORVYN *replaces* Foundation/AppKit/CoreFoundation/dispatch with host implementations, the Mach/XPC plumbing under them is never executed. That removes the largest XNU dependency from scope (INFERRED).

## 3. Interface inventory and classification

Classes: **W** emulable with Win32 · **S** implementable as an ORVYN service/host object · **N** needs a new abstraction · **H** hard to reproduce · **L** low priority.

| Interface | What programs use it for | Class | Notes |
|---|---|---|---|
| Mach ports and messages (`mach_msg`, `mach_port_allocate`, `mach_port_deallocate`, `mach_reply_port`, `bootstrap_look_up`) | IPC with system services (XPC, launchd, WindowServer, notification centers), `CFMachPort`, `CFMessagePort`, `NSMachPort`, XPC | **H/L** | no Windows equivalent of port rights and send-once; an ORVYN-internal object table with message queues (class S) could satisfy *in-process* use; cross-process services (launchd, WindowServer, `pbs`, `cfprefsd`) do not exist: emulate at the *framework* level, not at the port level |
| Mach tasks/threads (`mach_task_self`, `task_info`, `thread_info`, `mach_thread_self`, `task_threads`, `thread_get_state`) | CPU/memory statistics (Stats-like monitors), crash handlers, profilers | **S** | `task_info(TASK_BASIC_INFO)`, `thread_info` can be answered from `GetProcessMemoryInfo`/`GetThreadTimes`; `thread_get_state` is H |
| Host info (`host_info`, `host_statistics`, `host_processor_info`, `mach_host_self`) | system monitors | **S** | map to `GetSystemInfo`, `GlobalMemoryStatusEx`, performance counters; structure layouts are Darwin's (`host_info.h`) |
| Virtual memory (`vm_allocate`, `vm_deallocate`, `vm_protect`, `vm_region`, `mach_vm_*`) | allocators, JIT, GC, `libmalloc` | **W** | `VirtualAlloc`/`VirtualProtect`/`VirtualQuery`; granularity 64 KiB vs 4/16 KiB pages (see LIBSYSTEM doc); `vm_region` flavors have no Windows twin |
| Time (`mach_absolute_time`, `mach_timebase_info`, `mach_wait_until`) | high-resolution timers, benchmarking | **W** | `QueryPerformanceCounter` with a timebase of 1/1 or the QPC ratio; trivial and widely used by C/C++ code (`std::chrono::steady_clock` in libc++ on macOS calls `clock_gettime` / `mach_absolute_time`) |
| Semaphores (`semaphore_create/wait/signal`, `mach_semaphore`) | `dispatch_semaphore` uses them internally; direct use rare | **S** | ORVYN's dispatch already implements its own |
| System calls by number (`syscall`, `int 0x80`/`syscall` instruction) | Go runtime, some injected code | **L** | the guest never needs raw syscalls if it uses libSystem; a `syscall` instruction in guest code would hit the Windows kernel *unmodified*: **must stay refused/trapped** (security) |
| Process lifecycle (`fork`, `exec`, `posix_spawn`, `waitpid`, `exit`, `_exit`, `atexit`) | launching helpers, daemons | **N/H** | `exit` family exists; `posix_spawn` could create a Windows process only for host tools, not for guests unless ORVYN launches a *new worker*; fork has no equivalent (see LIBSYSTEM doc) |
| Signals | crash reporting, `SIGPIPE`, child exit | **H** | async-signal semantics vs SEH; see LIBSYSTEM doc |
| Timers (`setitimer`, `kevent` timers, `dispatch_source` timers) | UI animations, polling | **S** | through the run loop / dispatch work (see LIBDISPATCH doc) |
| `kqueue`/`kevent` | file watching, sockets | **N** | no Windows equivalent: IOCP/`ReadDirectoryChangesW` emulation |
| Synchronization (`os_unfair_lock`, `pthread_*`, `OSSpinLock`, `__ulock_wait`) | locks | **W/S** | cooperative scheduler: easy; `__ulock_*` are syscalls used by libSystem internals |
| Notifications (`notify_post`, `Libnotify`, `CFNotificationCenterGetDistributedCenter`) | inter-app notifications | **S** | in-process works today for `NSNotificationCenter`; distributed center/`notify_*` need a named-pipe-based ORVYN service if ever |
| Kernel events/`kext`, `IOKit` (`IOServiceMatching`, `IORegistry*`), `DiskArbitration` | hardware access | **H** | CotEditor/MonitorControl import IOKit; **not reproducible** in general: each concrete use (serial number, display DDC) needs a *per-property* answer or a refusal |
| Sandbox/entitlements/TCC/Gatekeeper/code signing | privacy prompts, permission checks | **N** | ORVYN has no such model; apps that *require* trust (`AXIsProcessTrusted`) cannot be honestly satisfied (return false) |
| `sysctl` (`kern.*`, `hw.*`, `machdep.*`) | OS version, CPU brand, memory | **S** | policy decision on what ORVYN reports |
| `getrusage`, `proc_pidinfo`, `proc_listpids` (libproc) | process monitors | **S** | map to Windows process APIs |

## 4. Minimal set ORVYN should simulate first (INFERRED priority)

1. `mach_absolute_time` + `mach_timebase_info` (W, trivial; used by libc++ and benchmarks).
2. `mach_task_self`/`mach_host_self`/`task_info`/`host_statistics`/`host_processor_info`/`host_info` for system-monitor style apps (S; layouts from `host_info.h`/`task_info.h`).
3. `vm_allocate`/`vm_deallocate`/`vm_protect` (W).
4. `sysctl`/`sysctlbyname` with an *explicit table* of the keys ORVYN answers (S); unknown keys fail with `ENOENT`, never invented values.
5. In-process Mach ports (S) only if a real application demands `CFMachPort`/`NSMachPort`; otherwise refuse.

## 5. Security note (VERIFIED, [LIMITATIONS.md](../LIMITATIONS.md))

The worker is a Job Object-restricted process, **not a sandbox**: Mach/XNU emulation must never expand the guest's reach (no raw syscalls, no handle passing, no way to open arbitrary host files outside `--root`). A guest asking for a service ORVYN does not emulate (launchd, WindowServer, XPC) must fail visibly, not receive a stub that pretends to succeed.
