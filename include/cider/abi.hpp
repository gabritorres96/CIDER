#pragma once
// System V AMD64 (macOS) <-> Windows x64 bridge.
//
//  * cider_enter_guest(): called with the Windows x64 ABI, switches to a guest stack, loads the System V
//    argument registers (rdi, rsi, rdx, rcx) and calls guest code. All registers Windows requires to be
//    preserved (rbx, rbp, rdi, rsi, r12-r15, xmm6-xmm15) are saved and restored, because System V code is
//    free to clobber rdi, rsi and xmm6-xmm15.
//  * Per-symbol stubs + cider_guest_thunk: guest code calls host (Windows ABI) functions through them. The
//    thunk moves rdi,rsi,rdx,rcx -> rcx,rdx,r8,r9, puts the 5th/6th integer arguments in the shadow-space
//    stack slots, switches back to the HOST stack (Windows exception/stack-limit checks stay valid) and
//    calls the function. double arguments in xmm0/xmm1 line up with Windows only when they are the first/
//    second positional argument; mixed integer+double signatures are NOT supported.
//  * exit() is implemented by flagging GuestCall::exited; the thunk then returns straight to the host
//    epilogue instead of back into the guest (no longjmp, no skipped C++ frames).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cider/memory.hpp"
#include "cider/result.hpp"

namespace cider::abi {

// Layout shared with src/abi/entry_trampoline.asm: DO NOT reorder (offsets are hard-coded there).
struct GuestCall {
    void* fn = nullptr;            //  0 guest function to call
    uint64_t stack_top = 0;        //  8 16-byte aligned initial guest rsp
    uint64_t arg[4] = {};          // 16 rdi, rsi, rdx, rcx
    uint64_t host_rsp = 0;         // 48 saved by cider_enter_guest
    uint64_t guest_rsp = 0;        // 56 saved by the thunk while host code runs
    uint64_t result = 0;           // 64 rax of the guest function / exit code
    uint64_t exited = 0;           // 72 set by exit()
    uint64_t exit_code = 0;        // 80
};
static_assert(offsetof(GuestCall, fn) == 0 && offsetof(GuestCall, stack_top) == 8 && offsetof(GuestCall, arg) == 16 &&
              offsetof(GuestCall, host_rsp) == 48 && offsetof(GuestCall, guest_rsp) == 56 && offsetof(GuestCall, result) == 64 &&
              offsetof(GuestCall, exited) == 72 && offsetof(GuestCall, exit_code) == 80 && sizeof(GuestCall) == 88,
              "GuestCall layout is mirrored in entry_trampoline.asm");

extern "C" {
uint64_t cider_enter_guest(GuestCall* call);
void cider_guest_thunk();
extern GuestCall* cider_current_call;
}

// Guest stack: committed read/write pages with one inaccessible guard page at the bottom.
class GuestStack {
public:
    static Result<GuestStack> create(size_t size);
    uint8_t* base() const noexcept { return region_.base(); }
    size_t size() const noexcept { return region_.size(); }
    uint64_t top() const noexcept { return reinterpret_cast<uint64_t>(region_.base()) + region_.size(); }
    const memory::Region& region() const noexcept { return region_; }
    memory::Region& region() noexcept { return region_; }

private:
    memory::Region region_;
};

struct GuestArgs {
    uint64_t argc = 0;
    uint64_t argv = 0, envp = 0, apple = 0;  // guest addresses (NULL terminated arrays)
    uint64_t initial_sp = 0;                 // 16-byte aligned, below the argument block
};

// Copies argv/envp/apple strings and the pointer arrays to the top of the stack (like the kernel does)
// and returns the pointers plus an aligned initial stack pointer.
Result<GuestArgs> build_guest_args(GuestStack& stack, const std::vector<std::string>& argv,
                                   const std::vector<std::string>& envp, const std::vector<std::string>& apple);

// Executable page(s) of per-symbol stubs. Each stub loads r10 = host function and jumps to
// cider_guest_thunk. Owned memory is released on destruction.
class ThunkTable {
public:
    static Result<ThunkTable> build(const std::vector<void*>& host_functions);
    uint64_t address(size_t i) const noexcept { return base_ + i * kStride; }
    size_t size() const noexcept { return count_; }
    static constexpr size_t kStride = 32;

private:
    memory::Region region_;
    uint64_t base_ = 0;
    size_t count_ = 0;
};

struct CallOutcome {
    uint64_t value = 0;      // rax of the guest function (or the exit code when exited)
    bool exited = false;     // guest called exit()
};

// Runs `fn` (an address in executable guest memory) on `stack` with up to four System V integer
// arguments. Single threaded: only one guest call may be active at a time.
CallOutcome call_guest(uint64_t fn, const GuestStack& stack, uint64_t initial_sp, uint64_t a0 = 0, uint64_t a1 = 0,
                       uint64_t a2 = 0, uint64_t a3 = 0);

// Called by exit(): marks the active call as exited with `code`; the thunk then unwinds to the host.
void request_guest_exit(uint64_t code);

}  // namespace cider::abi
