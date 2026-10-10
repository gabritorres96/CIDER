#include <algorithm>
#include <cstring>
#include <format>

#include "cider/abi.hpp"
#include "cider/log.hpp"

namespace cider::abi {

Result<ThunkTable> ThunkTable::build(const std::vector<void*>& fns) {
    ThunkTable t;
    size_t bytes = std::max<size_t>(fns.size(), 1) * kStride;
    auto rounded = memory::align_up(bytes, memory::page_size());
    if (!rounded) return Error{ErrorCode::Overflow, "thunk table size overflows"};
    auto region = memory::Region::reserve(0, static_cast<size_t>(*rounded), false);
    if (!region.ok()) return region.error();
    t.region_ = std::move(region.value());
    if (auto c = t.region_.commit(0, t.region_.size()); !c.ok()) return c.error();  // RW while writing: never RWX

    const uint64_t common = reinterpret_cast<uint64_t>(&cider_guest_thunk);
    uint8_t* p = t.region_.base();
    for (size_t i = 0; i < fns.size(); ++i, p += kStride) {
        std::memset(p, 0xCC, kStride);                       // int3 padding
        const uint64_t target = reinterpret_cast<uint64_t>(fns[i]);
        p[0] = 0x49; p[1] = 0xBA;                            // mov r10, imm64
        std::memcpy(p + 2, &target, 8);
        p[10] = 0xFF; p[11] = 0x25; p[12] = p[13] = p[14] = p[15] = 0;   // jmp qword ptr [rip+0]
        std::memcpy(p + 16, &common, 8);                     // ... -> cider_guest_thunk
    }
    if (auto pr = t.region_.protect(0, t.region_.size(), memory::Prot::ReadExecute); !pr.ok()) return pr.error();
    t.base_ = reinterpret_cast<uint64_t>(t.region_.base());
    t.count_ = fns.size();
    log::debug(log::Category::Abi, "built {} System V -> Windows thunk(s) at {:#x}", fns.size(), t.base_);
    return t;
}

CallOutcome call_guest(uint64_t fn, const GuestStack& stack, uint64_t initial_sp, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)stack;
    GuestCall call;
    call.fn = reinterpret_cast<void*>(fn);
    call.stack_top = initial_sp & ~uint64_t{15};
    call.arg[0] = a0; call.arg[1] = a1; call.arg[2] = a2; call.arg[3] = a3;
    log::debug(log::Category::Abi, "entering guest function {:#x} (rsp {:#x})", fn, call.stack_top);
    cider_enter_guest(&call);
    CallOutcome o;
    o.exited = call.exited != 0;
    o.value = call.result;
    return o;
}

void request_guest_exit(uint64_t code) {
    if (cider_current_call) {
        cider_current_call->exit_code = code;
        cider_current_call->exited = 1;
    }
}

}  // namespace cider::abi
