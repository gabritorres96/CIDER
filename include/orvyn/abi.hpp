#pragma once
// System V AMD64 (macOS) <-> Windows x64 bridge.
//
//  * orvyn_enter_guest(): called with the Windows x64 ABI, switches to a guest stack, loads the System V
//    argument registers (rdi, rsi, rdx, rcx) and calls guest code. All registers Windows requires to be
//    preserved (rbx, rbp, rdi, rsi, r12-r15, xmm6-xmm15) are saved and restored, because System V code is
//    free to clobber rdi, rsi and xmm6-xmm15.
//  * Per-symbol stubs + orvyn_guest_thunk: guest code calls host (Windows ABI) functions through them. The
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

#include "orvyn/memory.hpp"
#include "orvyn/result.hpp"

namespace orvyn::abi {

// Layout shared with src/abi/entry_trampoline.asm: DO NOT reorder (offsets are hard-coded there).
struct GuestCall {
    void* fn = nullptr;            //  0 guest function to call
    uint64_t stack_top = 0;        //  8 16-byte aligned initial guest rsp
    uint64_t arg[4] = {};          // 16 rdi, rsi, rdx, rcx
    uint64_t host_rsp = 0;         // 48 saved by orvyn_enter_guest
    uint64_t guest_rsp = 0;        // 56 saved by the thunk while host code runs
    uint64_t result = 0;           // 64 rax of the guest function / exit code
    uint64_t exited = 0;           // 72 set by exit()
    uint64_t exit_code = 0;        // 80
    uint64_t arg_r8 = 0;           // 88  5th and 6th integer arguments (callbacks)
    uint64_t arg_r9 = 0;           // 96
    uint64_t xmm_arg[8] = {};      // 104 xmm0..xmm7 (low 64 bits: doubles, or floats in the low 32 bits)
    uint64_t ret_rdx = 0;          // 168 second integer result (rax:rdx aggregates)
    uint64_t ret_xmm0 = 0;         // 176
    uint64_t ret_xmm1 = 0;         // 184
    uint64_t al = 0;               // 192 number of vector registers used (variadic callees)
};
static_assert(offsetof(GuestCall, fn) == 0 && offsetof(GuestCall, stack_top) == 8 && offsetof(GuestCall, arg) == 16 &&
              offsetof(GuestCall, host_rsp) == 48 && offsetof(GuestCall, guest_rsp) == 56 && offsetof(GuestCall, result) == 64 &&
              offsetof(GuestCall, exited) == 72 && offsetof(GuestCall, exit_code) == 80 && offsetof(GuestCall, arg_r8) == 88 &&
              offsetof(GuestCall, xmm_arg) == 104 && offsetof(GuestCall, ret_rdx) == 168 && offsetof(GuestCall, ret_xmm1) == 184 &&
              offsetof(GuestCall, al) == 192 && sizeof(GuestCall) == 200,
              "GuestCall layout is mirrored in entry_trampoline.asm");

// Everything a System V callee can look at when guest code calls a "raw" host function (src/abi/entry_trampoline.asm,
// orvyn_guest_thunk_raw). Layout is hard-coded in the assembly: DO NOT reorder. The raw thunk exists for signatures the
// positional register remapping of orvyn_guest_thunk cannot express: mixed integer/double arguments, more than six
// integer arguments, arguments on the stack, variadic functions (AL = number of vector registers used) and
// aggregate returns in rax:rdx / xmm0:xmm1 or through a hidden pointer.
struct GuestRegs {
    uint64_t gp[6] = {};          //   0 rdi rsi rdx rcx r8 r9
    uint64_t rax = 0;             //  48 on entry: AL is the number of vector registers used by a variadic call
    uint64_t stack_args = 0;      //  56 guest address of the first stack-passed argument (guest rsp + 8)
    uint64_t xmm[8] = {};         //  64 low 64 bits of xmm0..xmm7 (doubles; floats in the low 32 bits)
    uint64_t ret_rax = 0;         // 128 returned in rax
    uint64_t ret_rdx = 0;         // 136 returned in rdx (second INTEGER eightbyte)
    uint64_t ret_xmm0 = 0;        // 144 returned in xmm0 (low 64 bits)
    uint64_t ret_xmm1 = 0;        // 152 returned in xmm1 (second SSE eightbyte)
    uint64_t pad = 0;             // 160
};
static_assert(offsetof(GuestRegs, rax) == 48 && offsetof(GuestRegs, stack_args) == 56 && offsetof(GuestRegs, xmm) == 64 &&
              offsetof(GuestRegs, ret_rax) == 128 && offsetof(GuestRegs, ret_xmm1) == 152 && sizeof(GuestRegs) == 168,
              "GuestRegs layout is mirrored in entry_trampoline.asm");

// A raw host function receives the guest registers and returns nothing: results go into ret_*.
using RawHostFn = void (*)(GuestRegs*);

extern "C" {
uint64_t orvyn_enter_guest(GuestCall* call);
void orvyn_guest_thunk();
void orvyn_guest_thunk_raw();
void orvyn_guest_thunk_tlv();
extern GuestCall* orvyn_current_call;
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
// orvyn_guest_thunk. Owned memory is released on destruction.
struct ThunkSpec {
    void* fn = nullptr;     // Windows x64 function, or a RawHostFn when raw
    bool raw = false;       // true: the function receives GuestRegs* (see above)
    // true: the Darwin TLV calling convention (orvyn_guest_thunk_tlv): one pointer argument in rdi, result in rax, and EVERY
    // other register (rcx rdx rsi rdi r8-r10 xmm0-5 ...) is preserved for the guest, which keeps live values in them across
    // the call. Used for tlv_get_addr only.
    bool preserve_all = false;
};

class ThunkTable {
public:
    static Result<ThunkTable> build(const std::vector<void*>& host_functions);   // all positional (orvyn_guest_thunk)
    static Result<ThunkTable> build_specs(const std::vector<ThunkSpec>& host_functions);
    uint64_t address(size_t i) const noexcept { return base_ + i * kStride; }
    size_t size() const noexcept { return count_; }
    static constexpr size_t kStride = 32;

private:
    memory::Region region_;
    uint64_t base_ = 0;
    size_t count_ = 0;
};

// Sequential reader of System V arguments, shared by the register/stack path (a raw thunk) and by `va_list`.
class ArgReader {
public:
    virtual ~ArgReader() = default;
    virtual uint64_t next_int() = 0;       // INTEGER class argument (also pointers)
    virtual double next_double() = 0;      // SSE class argument (double)
};

// Arguments of a direct call: INTEGER class from rdi..r9 then the stack; SSE class from xmm0..xmm7 then the stack.
// Stack slots are shared by both classes in the order the arguments were passed, exactly as in the psABI.
class RegsReader final : public ArgReader {
public:
    explicit RegsReader(const GuestRegs& r, unsigned first_int = 0) : r_(r), gp_(first_int) {}
    uint64_t next_int() override;
    double next_double() override;

private:
    const GuestRegs& r_;
    unsigned gp_ = 0, fp_ = 0;
    uint64_t stack_ = 0;     // bytes consumed from the stack-argument area
};

// A System V `va_list` living in guest memory: {gp_offset, fp_offset, overflow_arg_area, reg_save_area}.
// Reading advances (and writes back) the structure like va_arg does.
class VaListReader final : public ArgReader {
public:
    explicit VaListReader(uint64_t guest_va_list) : va_(reinterpret_cast<VaList*>(guest_va_list)) {}
    uint64_t next_int() override;
    double next_double() override;
    struct VaList { uint32_t gp_offset; uint32_t fp_offset; uint64_t overflow_arg_area; uint64_t reg_save_area; };

private:
    VaList* va_;
};

struct CallOutcome {
    uint64_t value = 0;      // rax of the guest function (or the exit code when exited)
    bool exited = false;     // guest called exit()
    uint64_t rdx = 0;        // second integer result
    uint64_t xmm0 = 0, xmm1 = 0;   // vector results (low 64 bits)
    bool refused = false;    // a nested call was refused (depth, stack space, invalid target); nothing ran
    std::string refusal;
};

// Arguments of a call INTO guest code, System V classification: integers in rdi..r9 then the stack, doubles in xmm0-7 then the
// stack (stack slots shared in call order, 8 bytes each).
class GuestCallArgs {
public:
    GuestCallArgs& push_int(uint64_t v);
    GuestCallArgs& push_double(double d);
    GuestCallArgs& push_float(float f);          // a float occupies the low 32 bits of an xmm register / stack slot
    GuestCallArgs& variadic() { variadic_ = true; return *this; }   // set AL = number of vector registers used
    uint64_t ints[6] = {};
    uint64_t xmm[8] = {};
    unsigned n_int = 0, n_xmm = 0;
    std::vector<uint64_t> stack;
    bool variadic_ = false;
};

// Runs `fn` (an address in executable guest memory) on `stack` with up to four System V integer
// arguments. Single threaded: only one guest call may be active at a time.
CallOutcome call_guest(uint64_t fn, const GuestStack& stack, uint64_t initial_sp, uint64_t a0 = 0, uint64_t a1 = 0,
                       uint64_t a2 = 0, uint64_t a3 = 0);

// CALLBACKS (host -> guest). Valid only from a host function that the guest itself called through a thunk, i.e. while a guest
// call is active. The callee runs on the SAME guest stack, below the red zone (128 bytes) of the interrupted guest frame, with a fresh
// GuestCall; the host's callee-saved registers are preserved by orvyn_enter_guest and the guest's own by the System V callee.
// Nested callbacks are allowed up to kMaxCallbackDepth. If the callee (or anything it calls) invokes exit(), the request is
// propagated to the interrupted call and `exited` is true: the host function must return promptly.
// `fn` must be accepted by the validator (set_callback_validator); refused calls run nothing.
constexpr unsigned kMaxCallbackDepth = 64;
CallOutcome call_guest_nested(uint64_t fn, const GuestCallArgs& args);
// Top-level call with all argument classes.
CallOutcome call_guest_args(uint64_t fn, const GuestStack& stack, uint64_t initial_sp, const GuestCallArgs& args);
// Predicate telling whether `fn` is an address guest code may legitimately be called at. Default (none): nothing is accepted.
using CallbackValidator = bool (*)(uint64_t fn);
void set_callback_validator(CallbackValidator v);
unsigned callback_depth();

// Per-guest-thread bridge state (the globals behind callbacks). The guest-thread scheduler saves it when a thread gives up the
// guest lock and restores it when the thread gets the lock back; a new thread starts with a default-constructed state.
struct AbiThreadState {
    GuestCall* current = nullptr;
    unsigned depth = 0;
    uint64_t stack_lo = 0;
};
AbiThreadState save_thread_state();
void restore_thread_state(const AbiThreadState& s);

// Called by exit(): marks the active call as exited with `code`; the thunk then unwinds to the host.
void request_guest_exit(uint64_t code);

}  // namespace orvyn::abi
