#include <format>
#include <optional>

#include "orvyn/memory.hpp"

namespace orvyn::memory {

const char* to_string(Prot p) noexcept {
    switch (p) {
        case Prot::None: return "---";
        case Prot::Read: return "r--";
        case Prot::ReadWrite: return "rw-";
        case Prot::ReadExecute: return "r-x";
    }
    return "?";
}

Result<Prot> from_mach_prot(int32_t p) {
    const bool r = p & kVmRead, w = p & kVmWrite, x = p & kVmExecute;
    if ((p & ~(kVmRead | kVmWrite | kVmExecute)) != 0)
        return Error{ErrorCode::Malformed, std::format("invalid VM protection bits {:#x}", p)};
    if (w && x)
        return Error{ErrorCode::Unsupported, "segment is writable and executable (RWX): refused by the W^X policy"};
    if (!r && (w || x))
        return Error{ErrorCode::Unsupported, std::format("protection {:#x} (write/execute without read) cannot be represented", p)};
    if (x) return Prot::ReadExecute;
    if (w) return Prot::ReadWrite;
    if (r) return Prot::Read;
    return Prot::None;
}

bool is_aligned(uint64_t v, uint64_t align) noexcept { return align != 0 && v % align == 0; }

std::optional<uint64_t> align_up(uint64_t v, uint64_t align) noexcept {
    if (align == 0) return std::nullopt;
    uint64_t rem = v % align;
    if (rem == 0) return v;
    if (v > UINT64_MAX - (align - rem)) return std::nullopt;
    return v + (align - rem);
}

}  // namespace orvyn::memory
