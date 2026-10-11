#pragma once
// Guest threads (ORVYN 0.4.2): pthreads for Mach-O programs, implemented with real Windows threads under a COOPERATIVE scheduler.
//
//  * Every guest thread is an OS thread with its own guest stack and its own chain of GuestCall records. The ABI bridge keeps
//    `orvyn_current_call` (and the callback depth / stack bound) in globals, so only ONE guest thread executes at a time: a fair
//    FIFO "global guest lock" (ticket lock) is held while guest code or any host function runs, and released only at blocking or
//    yielding points (pthread_join, mutex contention, cond wait, sleeps, sched_yield, console/file reads, thread start/end).
//    The abi state of the thread that gives up the lock is saved and restored when it gets it back.
//  * Consequences, documented in docs/THREADING.md: no true parallelism; a guest thread that spins on shared memory without ever
//    calling a libSystem function starves the others (the worker's timeout ends it); data races can only happen at the yield
//    points, so races between unprotected read-modify-write sequences are visible only if the program yields in between.
//  * Darwin objects are adapted, never shared with Windows: pthread_t is an opaque number (no pointer to anything), pthread_mutex_t
//    and pthread_cond_t only keep a signature and a host-table index in the guest's storage, attributes are small host records.
//  * The Windows TEB, %gs, TlsAlloc slots are never touched; thread-local storage blocks are per guest thread (see tls.cpp).
#include <atomic>
#include <condition_variable>
#include <functional>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "orvyn/abi.hpp"

namespace orvyn::libsystem {

struct RuntimeContext;

struct ThreadState {
    uint64_t id = 0;                         // 1 = the main thread
    bool is_main = false;
    bool started = false, finished = false, detached = false, joined = false;
    bool exiting = false;                    // pthread_exit() was called
    uint64_t retval = 0;
    uint64_t start_fn = 0, start_arg = 0;
    std::function<void()> native_body;       // host-implemented thread (libdispatch workers): runs under the guest lock instead of start_fn
    std::unique_ptr<abi::GuestStack> stack;
    void* os_thread = nullptr;               // Win32 thread handle (owned)
    abi::AbiThreadState abi{};               // saved while the thread does not own the guest lock
    int errno_value = 0;                     // errno is per thread on Darwin
    std::vector<uint8_t*> tls_blocks;        // thread-local storage, one block per TlsImage (lazily created)
    std::map<uint32_t, uint64_t> specific;   // pthread_setspecific values
    std::vector<std::vector<uint64_t>> pools;   // Objective-C autorelease pools of this thread (innermost last)
};

struct MutexObject { bool in_use = false; int type = 0; uint64_t owner = 0; int count = 0; };
struct CondObject { bool in_use = false; uint64_t waiters = 0, wake = 0; };

class GuestThreads {
public:
    explicit GuestThreads(RuntimeContext& ctx);
    ~GuestThreads();                          // shutdown(): joins every OS thread
    GuestThreads(const GuestThreads&) = delete;
    GuestThreads& operator=(const GuestThreads&) = delete;

    // The calling OS thread becomes guest thread 1 and takes the guest lock.
    void attach_main();
    // At the end of the process: no thread may run guest code any more; waits (bounded) for the OS threads to finish.
    // Returns false if some thread did not stop in time (it is detached and leaked: only the worker exit can kill it).
    bool shutdown();

    static ThreadState* current();           // nullptr on a thread that is not a guest thread
    uint64_t current_id();

    // ---- scheduling (call only while holding the guest lock, i.e. from host functions) ---------------------------------------
    void yield();                             // let waiting threads run
    void sleep_ms(uint64_t ms);               // gives up the lock while sleeping
    // Gives up the lock while `fn()` blocks in the OS (stdin read...). If the process is exiting afterwards the guest is unwound.
    template <class F> auto blocking(F&& fn) {
        ThreadState* ts = current();
        release(ts);
        struct Back { GuestThreads* g; ThreadState* t; ~Back() { g->acquire(t); g->after_wake(); } } back{this, ts};
        return fn();
    }
    // Blocks until pred() is true (evaluated under the lock), or `timeout_ms` passes (>= 0). Returns pred().
    template <class Pred> bool wait_until(Pred pred, int64_t timeout_ms = -1) {
        const uint64_t deadline = timeout_ms < 0 ? 0 : now_ms() + static_cast<uint64_t>(timeout_ms);
        while (!pred()) {
            if (exit_requested()) { after_wake(); return false; }
            if (timeout_ms >= 0 && now_ms() >= deadline) return false;
            park();
        }
        if (exit_requested()) after_wake();       // the condition came true because the process is dying: unwind anyway
        return true;
    }
    void notify();                            // state changed: wake parked threads

    // ---- process exit -----------------------------------------------------------------------------------------------------------
    void request_exit(int code);              // exit(): every other thread unwinds at its next wake-up
    bool exit_requested() const { return exit_requested_.load(); }
    int exit_code() const { return exit_code_; }

    // ---- pthread operations (return Darwin errno values) -------------------------------------------------------------------------
    int create(uint64_t fn, uint64_t arg, size_t stack_size, uint64_t& handle, std::function<void()> native = {});
    // A thread whose body is host code (it may call guest code through Runtime::call_imp on its own guest stack). Detached; stops when the process exits.
    int create_native(std::function<void()> body, uint64_t& handle);
    int join(uint64_t handle, uint64_t* retval);
    int detach(uint64_t handle);
    void exit_thread(uint64_t retval);        // pthread_exit
    static uint64_t handle_of(uint64_t id) { return 0x10000000000ull + id * 0x1000; }
    static uint64_t id_of(uint64_t handle) { return handle >= 0x10000000000ull && (handle - 0x10000000000ull) % 0x1000 == 0 ? (handle - 0x10000000000ull) / 0x1000 : 0; }
    ThreadState* find(uint64_t id);

    std::vector<MutexObject> mutexes;         // index = host id - 1
    std::vector<CondObject> conds;
    size_t live_threads() const;

    static constexpr size_t kMaxThreads = 64;
    static constexpr size_t kDefaultStack = 1u << 20;

private:
    RuntimeContext& ctx_;
    std::mutex m_;                            // protects the ticket counters
    std::condition_variable cv_;
    uint64_t next_ticket_ = 0, serving_ = 0;
    std::mutex sync_m_;
    std::condition_variable sync_cv_;
    std::map<uint64_t, std::unique_ptr<ThreadState>> threads_;
    uint64_t next_id_ = 2;
    std::atomic<bool> exit_requested_{false};
    int exit_code_ = 0;
    bool shut_down_ = false;

    void acquire(ThreadState* ts);
    void release(ThreadState* ts);
    void park();                              // release, wait for notify/2 ms, re-acquire, unwind if exiting
    void after_wake();                        // exit requested while we were away: unwind this thread
    static uint64_t now_ms();
    static unsigned long __stdcall thread_main(void* arg);
    void run_thread(ThreadState* ts);
};

}  // namespace orvyn::libsystem
