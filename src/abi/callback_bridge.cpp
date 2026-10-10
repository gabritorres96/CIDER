// Calls INTO guest code: the top-level entry (main, initializers, atexit handlers) and host -> guest callbacks (qsort
// comparators, bsearch, Objective-C method implementations...). Both use cider_enter_guest with a fresh GuestCall record;
// a nested call simply nests the records: cider_current_call always points to the innermost one, so the thunks of any guest
// code that runs inside a callback switch to the host stack of THAT call (below the interrupted host frames).
//
// Security / robustness rules enforced here:
//   * a callback target must be accepted by the validator (executable guest or thunk memory, never arbitrary host code);
//   * recursion depth is limited and the guest stack must keep a safety margin (no silent stack overflow into the guard page);
//   * the red zone (128 bytes) below the interrupted guest rsp is never touched;
//   * exit() inside a callback unwinds every nested host frame by returning `exited` up the chain (no longjmp, no skipped C++ frames).
#include <cstring>
#include <format>

#include "cider/abi.hpp"
#include "cider/log.hpp"

namespace cider::abi {
namespace {

CallbackValidator g_validator = nullptr;
unsigned g_depth = 0;
uint64_t g_stack_lo = 0;          // lowest address a nested frame may use (guard page + margin) for the active top-level call
constexpr uint64_t kRedZone = 128;
constexpr uint64_t kStackMargin = 64 * 1024;

uint64_t bits_of(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }

// Stores stack arguments below `top` (16-byte aligned) and fills the register fields.
void prepare(GuestCall& c, const GuestCallArgs& a, uint64_t top) {
    const size_t n = a.stack.size();
    const size_t slots = (n + 1) & ~size_t{1};            // keep rsp 16-byte aligned at the call
    const uint64_t sp = top - slots * 8;
    if (n) std::memcpy(reinterpret_cast<void*>(sp), a.stack.data(), n * 8);
    c.stack_top = sp;
    for (int i = 0; i < 4; ++i) c.arg[i] = a.ints[i];
    c.arg_r8 = a.ints[4];
    c.arg_r9 = a.ints[5];
    for (int i = 0; i < 8; ++i) c.xmm_arg[i] = a.xmm[i];
    c.al = a.variadic_ ? a.n_xmm : 0;
}

CallOutcome finish(const GuestCall& c) {
    CallOutcome o;
    o.exited = c.exited != 0;
    o.value = c.result;
    o.rdx = c.ret_rdx;
    o.xmm0 = c.ret_xmm0;
    o.xmm1 = c.ret_xmm1;
    return o;
}

}  // namespace

GuestCallArgs& GuestCallArgs::push_int(uint64_t v) {
    if (n_int < 6) ints[n_int++] = v; else stack.push_back(v);
    return *this;
}
GuestCallArgs& GuestCallArgs::push_double(double d) {
    if (n_xmm < 8) xmm[n_xmm++] = bits_of(d); else stack.push_back(bits_of(d));
    return *this;
}
GuestCallArgs& GuestCallArgs::push_float(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    if (n_xmm < 8) xmm[n_xmm++] = u; else stack.push_back(u);
    return *this;
}

void set_callback_validator(CallbackValidator v) { g_validator = v; }
AbiThreadState save_thread_state() { return {cider_current_call, g_depth, g_stack_lo}; }
void restore_thread_state(const AbiThreadState& s) {
    cider_current_call = s.current;
    g_depth = s.depth;
    g_stack_lo = s.stack_lo;
}
unsigned callback_depth() { return g_depth; }

CallOutcome call_guest_args(uint64_t fn, const GuestStack& stack, uint64_t initial_sp, const GuestCallArgs& args) {
    GuestCall call;
    call.fn = reinterpret_cast<void*>(fn);
    prepare(call, args, initial_sp & ~uint64_t{15});
    const uint64_t saved_lo = g_stack_lo;
    g_stack_lo = reinterpret_cast<uint64_t>(stack.base()) + 2 * memory::page_size();
    GuestCall* prev = cider_current_call;
    log::debug(log::Category::Abi, "entering guest function {:#x} (rsp {:#x})", fn, call.stack_top);
    cider_enter_guest(&call);
    cider_current_call = prev;
    g_stack_lo = saved_lo;
    return finish(call);
}

CallOutcome call_guest(uint64_t fn, const GuestStack& stack, uint64_t initial_sp, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    GuestCallArgs a;
    a.push_int(a0).push_int(a1).push_int(a2).push_int(a3);
    return call_guest_args(fn, stack, initial_sp, a);
}

CallOutcome call_guest_nested(uint64_t fn, const GuestCallArgs& args) {
    CallOutcome refused;
    refused.refused = true;
    GuestCall* parent = cider_current_call;
    if (!parent) { refused.refusal = "no guest call is active"; return refused; }
    if (g_depth >= kMaxCallbackDepth) { refused.refusal = std::format("callback nesting deeper than {}", kMaxCallbackDepth); return refused; }
    if (!g_validator || !g_validator(fn)) { refused.refusal = std::format("{:#x} is not executable guest code", fn); return refused; }
    // The interrupted guest frame ends at parent->guest_rsp (saved by the thunk); keep the red zone below it intact.
    const uint64_t top = (parent->guest_rsp - kRedZone) & ~uint64_t{15};
    const uint64_t need = (args.stack.size() + 2) * 8 + kStackMargin;
    if (top < g_stack_lo + need) { refused.refusal = "guest stack exhausted"; return refused; }

    GuestCall call;
    call.fn = reinterpret_cast<void*>(fn);
    prepare(call, args, top);
    ++g_depth;
    cider_enter_guest(&call);
    --g_depth;
    cider_current_call = parent;
    if (call.exited) {                       // propagate exit() to the interrupted call: its thunk will unwind
        parent->exit_code = call.exit_code;
        parent->exited = 1;
    }
    return finish(call);
}

}  // namespace cider::abi
