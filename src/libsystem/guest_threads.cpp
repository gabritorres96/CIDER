#include "cider/guest_threads.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "cider/libsystem.hpp"
#include "cider/log.hpp"
#include "internal.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace cider::libsystem {
namespace {
thread_local ThreadState* t_current = nullptr;   // HOST thread_local (a C++ variable): not related to guest TLS
}

ThreadState* GuestThreads::current() { return t_current; }
uint64_t GuestThreads::now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

GuestThreads::GuestThreads(RuntimeContext& ctx) : ctx_(ctx) {}
GuestThreads::~GuestThreads() { shutdown(); }

uint64_t GuestThreads::current_id() { return t_current ? t_current->id : 1; }

// ---- the guest lock: a fair FIFO ticket lock ----------------------------------------------------------------------
void GuestThreads::acquire(ThreadState* ts) {
    std::unique_lock<std::mutex> l(m_);
    const uint64_t my = next_ticket_++;
    cv_.wait(l, [&] { return serving_ == my; });
    l.unlock();
    t_current = ts;
    abi::restore_thread_state(ts->abi);
}

void GuestThreads::release(ThreadState* ts) {
    ts->abi = abi::save_thread_state();
    {
        std::lock_guard<std::mutex> l(m_);
        ++serving_;
    }
    cv_.notify_all();
}

void GuestThreads::notify() { sync_cv_.notify_all(); }

void GuestThreads::after_wake() {
    ThreadState* ts = t_current;
    if (exit_requested_ && ts && !ts->exiting) abi::request_guest_exit(static_cast<uint64_t>(static_cast<uint32_t>(exit_code_)));
}

void GuestThreads::park() {
    ThreadState* ts = t_current;
    release(ts);
    {
        std::unique_lock<std::mutex> l(sync_m_);
        sync_cv_.wait_for(l, std::chrono::milliseconds(2));
    }
    acquire(ts);
}

void GuestThreads::yield() {
    ThreadState* ts = t_current;
    if (!ts) return;
    {
        std::lock_guard<std::mutex> l(m_);
        if (next_ticket_ - serving_ <= 1) return;                // nobody else wants to run
    }
    release(ts);
    std::this_thread::yield();
    acquire(ts);
    after_wake();
}

void GuestThreads::sleep_ms(uint64_t ms) {
    ThreadState* ts = t_current;
    if (!ts) { Sleep(static_cast<DWORD>(std::min<uint64_t>(ms, 0x7fffffff))); return; }
    release(ts);
    const uint64_t end = now_ms() + ms;
    while (!exit_requested_) {
        const uint64_t now = now_ms();
        if (now >= end) break;
        Sleep(static_cast<DWORD>(std::min<uint64_t>(end - now, 20)));
    }
    acquire(ts);
    after_wake();
}

void GuestThreads::attach_main() {
    auto main = std::make_unique<ThreadState>();
    main->id = 1;
    main->is_main = true;
    main->started = true;
    ThreadState* raw = main.get();
    threads_[1] = std::move(main);
    acquire(raw);
}

void GuestThreads::request_exit(int code) {
    if (!exit_requested_) { exit_code_ = code; exit_requested_ = true; }
    notify();
}

size_t GuestThreads::live_threads() const {
    size_t n = 0;
    for (auto& kv : threads_)
        if (!kv.second->finished) ++n;
    return n;
}

ThreadState* GuestThreads::find(uint64_t id) {
    auto it = threads_.find(id);
    return it == threads_.end() ? nullptr : it->second.get();
}

// ---- thread lifecycle --------------------------------------------------------------------------------------------------
unsigned long __stdcall GuestThreads::thread_main(void* arg) {
    auto* ts = static_cast<ThreadState*>(arg);
    // `ts` carries a back pointer through start_arg's companion field: the owning scheduler is found through the context
    RuntimeContext* c = context();
    if (c && c->threads) c->threads->run_thread(ts);
    return 0;
}

void GuestThreads::run_thread(ThreadState* ts) {
    acquire(ts);                                                  // waits for the guest lock; the thread starts with a clean abi state
    ts->started = true;
    if (!exit_requested_ && ts->native_body) {
        ts->native_body();
    } else if (!exit_requested_) {
        abi::GuestCallArgs args;
        args.push_int(ts->start_arg);
        log::debug(log::Category::Execution, "guest thread {} starts at {:#x}", ts->id, ts->start_fn);
        abi::CallOutcome o = abi::call_guest_args(ts->start_fn, *ts->stack, ts->stack->top() - 64, args);
        if (!ts->exiting && !o.exited) ts->retval = o.value;
        // o.exited without ts->exiting: exit() was called by this thread; request_exit() already told everybody
    }
    ts->finished = true;
    notify();
    release(ts);
}

int GuestThreads::create_native(std::function<void()> body, uint64_t& handle) {
    const int rc = create(0, 0, 0, handle, std::move(body));
    if (rc == 0) if (ThreadState* t = find(id_of(handle))) t->detached = true;
    return rc;
}

int GuestThreads::create(uint64_t fn, uint64_t arg, size_t stack_size, uint64_t& handle, std::function<void()> native) {
    // sweep finished detached threads
    for (auto it = threads_.begin(); it != threads_.end();) {
        ThreadState& t = *it->second;
        if (t.detached && t.finished && t.os_thread && WaitForSingleObject(static_cast<HANDLE>(t.os_thread), 0) == WAIT_OBJECT_0) {
            CloseHandle(static_cast<HANDLE>(t.os_thread));
            it = threads_.erase(it);
        } else {
            ++it;
        }
    }
    if (live_threads() >= kMaxThreads) return 35;                // EAGAIN
    if (stack_size == 0) stack_size = kDefaultStack;
    stack_size = std::clamp<size_t>(stack_size, 64 * 1024, 64u << 20);
    auto stack = abi::GuestStack::create(stack_size);
    if (!stack.ok()) return 35;
    auto ts = std::make_unique<ThreadState>();
    ts->id = next_id_++;
    ts->start_fn = fn;
    ts->start_arg = arg;
    ts->native_body = std::move(native);
    ts->stack = std::make_unique<abi::GuestStack>(std::move(stack.value()));
    ThreadState* raw = ts.get();
    HANDLE h = CreateThread(nullptr, 4u << 20, reinterpret_cast<LPTHREAD_START_ROUTINE>(&GuestThreads::thread_main), raw, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) return 35;
    raw->os_thread = h;
    handle = handle_of(raw->id);
    threads_[raw->id] = std::move(ts);
    return 0;
}

int GuestThreads::join(uint64_t handle, uint64_t* retval) {
    const uint64_t id = id_of(handle);
    ThreadState* ts = id ? find(id) : nullptr;
    if (!ts) return 3;                                           // ESRCH
    if (ts == t_current) return 11;                              // EDEADLK
    if (ts->detached) return 22;                                 // EINVAL
    if (!wait_until([&] { return ts->finished; })) return 0;     // the process is exiting: the guest is being unwound anyway
    if (retval) *retval = ts->retval;
    if (ts->os_thread) {
        WaitForSingleObject(static_cast<HANDLE>(ts->os_thread), INFINITE);   // the thread only has to leave release(): it needs no guest lock
        CloseHandle(static_cast<HANDLE>(ts->os_thread));
        ts->os_thread = nullptr;
    }
    threads_.erase(id);
    return 0;
}

int GuestThreads::detach(uint64_t handle) {
    const uint64_t id = id_of(handle);
    ThreadState* ts = id ? find(id) : nullptr;
    if (!ts) return 3;
    if (ts->detached) return 22;
    ts->detached = true;
    return 0;
}

void GuestThreads::exit_thread(uint64_t retval) {
    ThreadState* ts = t_current;
    if (!ts) return;
    ts->exiting = true;
    ts->retval = retval;
    abi::request_guest_exit(0);                                  // unwinds this thread's whole guest call chain
}

bool GuestThreads::shutdown() {
    if (shut_down_) return true;
    shut_down_ = true;
    exit_requested_ = true;
    ThreadState* self = t_current;
    if (self) release(self);                                     // let the other threads run, they will unwind
    notify();
    bool clean = true;
    for (auto& kv : threads_) {
        ThreadState& t = *kv.second;
        if (!t.os_thread) continue;
        const DWORD r = WaitForSingleObject(static_cast<HANDLE>(t.os_thread), 5000);
        if (r != WAIT_OBJECT_0) { clean = false; log::warn(log::Category::Execution, "guest thread {} did not stop within 5 s", t.id); }
        else CloseHandle(static_cast<HANDLE>(t.os_thread));
        t.os_thread = nullptr;
    }
    if (clean) threads_.clear();
    t_current = nullptr;
    return clean;
}

}  // namespace cider::libsystem
