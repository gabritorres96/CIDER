#include "orvyn/bytes.hpp"

namespace orvyn {

const char* to_string(ErrorCode c) noexcept {
    switch (c) {
        case ErrorCode::Io: return "io-error";
        case ErrorCode::TooLarge: return "too-large";
        case ErrorCode::Truncated: return "truncated";
        case ErrorCode::BadMagic: return "bad-magic";
        case ErrorCode::Unsupported: return "unsupported";
        case ErrorCode::Malformed: return "malformed";
        case ErrorCode::OutOfBounds: return "out-of-bounds";
        case ErrorCode::Overflow: return "overflow";
        case ErrorCode::Overlap: return "overlap";
        case ErrorCode::NotFound: return "not-found";
        case ErrorCode::InvalidArgument: return "invalid-argument";
        case ErrorCode::LimitExceeded: return "limit-exceeded";
    }
    return "unknown";
}

std::optional<uint64_t> checked_add(uint64_t a, uint64_t b) noexcept {
    if (a > UINT64_MAX - b) return std::nullopt;
    return a + b;
}

std::optional<uint64_t> checked_mul(uint64_t a, uint64_t b) noexcept {
    if (a != 0 && b > UINT64_MAX / a) return std::nullopt;
    return a * b;
}

Result<ByteView> ByteView::slice(uint64_t off, uint64_t len) const {
    if (!contains(off, len))
        return Error{ErrorCode::OutOfBounds,
                     std::format("range [{:#x}, +{:#x}) outside of {:#x} bytes", off, len, size_)};
    return ByteView(data_ + off, static_cast<size_t>(len));
}

namespace {
template <class T>
T load(const uint8_t* p, bool big) noexcept {
    T v = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        size_t idx = big ? i : sizeof(T) - 1 - i;  // most significant byte first
        v = static_cast<T>((v << 8) | p[idx]);
    }
    return v;
}
}  // namespace

Result<uint8_t> ByteView::u8(uint64_t off) const {
    if (!contains(off, 1)) return Error{ErrorCode::Truncated, std::format("read u8 at {:#x} out of range", off)};
    return data_[off];
}
Result<uint16_t> ByteView::u16(uint64_t off, bool big) const {
    if (!contains(off, 2)) return Error{ErrorCode::Truncated, std::format("read u16 at {:#x} out of range", off)};
    return load<uint16_t>(data_ + off, big);
}
Result<uint32_t> ByteView::u32(uint64_t off, bool big) const {
    if (!contains(off, 4)) return Error{ErrorCode::Truncated, std::format("read u32 at {:#x} out of range", off)};
    return load<uint32_t>(data_ + off, big);
}
Result<uint64_t> ByteView::u64(uint64_t off, bool big) const {
    if (!contains(off, 8)) return Error{ErrorCode::Truncated, std::format("read u64 at {:#x} out of range", off)};
    return load<uint64_t>(data_ + off, big);
}

uint32_t ByteView::peek_u32(uint64_t off, bool big) const noexcept {
    return contains(off, 4) ? load<uint32_t>(data_ + off, big) : 0;
}
uint64_t ByteView::peek_u64(uint64_t off, bool big) const noexcept {
    return contains(off, 8) ? load<uint64_t>(data_ + off, big) : 0;
}

Result<std::string> ByteView::cstring(uint64_t off, uint64_t end) const {
    if (end > size_) end = size_;
    if (off >= end) return Error{ErrorCode::OutOfBounds, std::format("string offset {:#x} out of range", off)};
    for (uint64_t i = off; i < end; ++i) {
        if (data_[i] == 0) return std::string(reinterpret_cast<const char*>(data_ + off), static_cast<size_t>(i - off));
    }
    return Error{ErrorCode::Malformed, std::format("unterminated string at {:#x}", off)};
}

std::string ByteView::fixed_string(uint64_t off, uint64_t len) const {
    if (!contains(off, len)) return {};
    size_t n = 0;
    while (n < len && data_[off + n] != 0) ++n;
    return std::string(reinterpret_cast<const char*>(data_ + off), n);
}

}  // namespace orvyn
