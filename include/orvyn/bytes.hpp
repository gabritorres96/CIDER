#pragma once
// Bounds-checked read-only byte view. Every checked accessor returns an Error instead of
// reading outside the view. The `peek_*` accessors never fault (they return 0 when out of
// range) and are meant for use after the caller verified a minimum size.
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "orvyn/result.hpp"

namespace orvyn {

std::optional<uint64_t> checked_add(uint64_t a, uint64_t b) noexcept;
std::optional<uint64_t> checked_mul(uint64_t a, uint64_t b) noexcept;

class ByteView {
public:
    ByteView() = default;
    ByteView(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    ByteView(const std::vector<uint8_t>& v) : data_(v.data()), size_(v.size()) {}  // NOLINT

    const uint8_t* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    // True when [off, off+len) lies inside the view (overflow-safe).
    bool contains(uint64_t off, uint64_t len) const noexcept { return off <= size_ && len <= size_ - off; }

    Result<ByteView> slice(uint64_t off, uint64_t len) const;

    Result<uint8_t> u8(uint64_t off) const;
    Result<uint16_t> u16(uint64_t off, bool big_endian = false) const;
    Result<uint32_t> u32(uint64_t off, bool big_endian = false) const;
    Result<uint64_t> u64(uint64_t off, bool big_endian = false) const;

    // NUL-terminated string starting at `off`; the terminator must exist before `end`
    // (end is clamped to size()). Fails if unterminated.
    Result<std::string> cstring(uint64_t off, uint64_t end) const;

    // Fixed-width field (e.g. 16-byte segment names): up to the first NUL or `len` bytes.
    std::string fixed_string(uint64_t off, uint64_t len) const;

    uint32_t peek_u32(uint64_t off, bool big_endian = false) const noexcept;
    uint64_t peek_u64(uint64_t off, bool big_endian = false) const noexcept;

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

}  // namespace orvyn
