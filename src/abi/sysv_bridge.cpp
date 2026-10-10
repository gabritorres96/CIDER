#include <algorithm>
#include <cstring>
#include <format>

#include "cider/abi.hpp"
#include "cider/log.hpp"

namespace cider::abi {

Result<ThunkTable> ThunkTable::build(const std::vector<void*>& fns) {
    std::vector<ThunkSpec> specs;
    specs.reserve(fns.size());
    for (void* f : fns) specs.push_back({f, false});
    return build_specs(specs);
}

Result<ThunkTable> ThunkTable::build_specs(const std::vector<ThunkSpec>& fns) {
    ThunkTable t;
    size_t bytes = std::max<size_t>(fns.size(), 1) * kStride;
    auto rounded = memory::align_up(bytes, memory::page_size());
    if (!rounded) return Error{ErrorCode::Overflow, "thunk table size overflows"};
    auto region = memory::Region::reserve(0, static_cast<size_t>(*rounded), false);
    if (!region.ok()) return region.error();
    t.region_ = std::move(region.value());
    if (auto c = t.region_.commit(0, t.region_.size()); !c.ok()) return c.error();  // RW while writing: never RWX

    const uint64_t positional = reinterpret_cast<uint64_t>(&cider_guest_thunk);
    const uint64_t raw = reinterpret_cast<uint64_t>(&cider_guest_thunk_raw);
    const uint64_t tlv = reinterpret_cast<uint64_t>(&cider_guest_thunk_tlv);
    uint8_t* p = t.region_.base();
    for (size_t i = 0; i < fns.size(); ++i, p += kStride) {
        std::memset(p, 0xCC, kStride);                       // int3 padding
        const uint64_t common = fns[i].preserve_all ? tlv : (fns[i].raw ? raw : positional);
        const uint64_t target = reinterpret_cast<uint64_t>(fns[i].fn);
        p[0] = 0x49; p[1] = fns[i].preserve_all ? 0xBB : 0xBA;   // mov r10, imm64  (r11 for the TLV thunk: r10 belongs to the guest there)
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

// ---- System V argument readers ---------------------------------------------------------------------------------
uint64_t RegsReader::next_int() {
    if (gp_ < 6) return r_.gp[gp_++];
    uint64_t v;
    std::memcpy(&v, reinterpret_cast<const void*>(r_.stack_args + stack_), 8);
    stack_ += 8;
    return v;
}

double RegsReader::next_double() {
    uint64_t bits;
    if (fp_ < 8) {
        bits = r_.xmm[fp_++];
    } else {
        std::memcpy(&bits, reinterpret_cast<const void*>(r_.stack_args + stack_), 8);
        stack_ += 8;
    }
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}

uint64_t VaListReader::next_int() {
    uint64_t v;
    if (va_->gp_offset < 48) {
        std::memcpy(&v, reinterpret_cast<const void*>(va_->reg_save_area + va_->gp_offset), 8);
        va_->gp_offset += 8;
    } else {
        std::memcpy(&v, reinterpret_cast<const void*>(va_->overflow_arg_area), 8);
        va_->overflow_arg_area += 8;
    }
    return v;
}

double VaListReader::next_double() {
    double d;
    if (va_->fp_offset < 176) {
        std::memcpy(&d, reinterpret_cast<const void*>(va_->reg_save_area + va_->fp_offset), 8);
        va_->fp_offset += 16;
    } else {
        std::memcpy(&d, reinterpret_cast<const void*>(va_->overflow_arg_area), 8);
        va_->overflow_arg_area += 8;
    }
    return d;
}

void request_guest_exit(uint64_t code) {
    if (cider_current_call) {
        cider_current_call->exit_code = code;
        cider_current_call->exited = 1;
    }
}

}  // namespace cider::abi
