#pragma once
// Virtual-memory layer for the Mach-O loader (Windows: VirtualAlloc/VirtualProtect/VirtualFree/VirtualQuery).
// Policy: a page is never writable and executable at the same time (W^X). RWX requests are errors.
#include <cstddef>
#include <cstdint>
#include <optional>

#include "orvyn/result.hpp"

namespace orvyn::memory {

enum class Prot { None, Read, ReadWrite, ReadExecute };
const char* to_string(Prot p) noexcept;

// Mach-O VM_PROT_* bits.
inline constexpr int32_t kVmRead = 1, kVmWrite = 2, kVmExecute = 4;

// Maps Mach-O protection bits to a Prot. W+X (any combination containing both) and execute-without-read
// are rejected; write-without-read is rejected (cannot be expressed on Windows).
Result<Prot> from_mach_prot(int32_t vm_prot);

size_t page_size();               // 4096 on x86-64 Windows
size_t allocation_granularity();  // 65536 on Windows
bool is_aligned(uint64_t v, uint64_t align) noexcept;
std::optional<uint64_t> align_up(uint64_t v, uint64_t align) noexcept;  // nullopt on overflow

struct PageInfo {
    enum class State { Free, Reserved, Committed } state = State::Free;
    Prot prot = Prot::None;      // meaningful when Committed
    bool executable = false;
    bool writable = false;
    size_t region_size = 0;      // size of the run of pages sharing these attributes
};
PageInfo query_address(const void* p);  // VirtualQuery wrapper (tests and diagnostics)

// RAII reservation. Freed (MEM_RELEASE) on destruction, so a failed load leaves nothing behind.
class Region {
public:
    Region() = default;
    ~Region();
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;
    Region(Region&& o) noexcept;
    Region& operator=(Region&& o) noexcept;

    // Reserves `size` bytes at `preferred` (must be allocation-granularity aligned; 0 = anywhere). If the
    // preferred address is taken and allow_relocation is true, the OS picks another address.
    static Result<Region> reserve(uintptr_t preferred, size_t size, bool allow_relocation);

    uint8_t* base() const noexcept { return base_; }
    size_t size() const noexcept { return size_; }
    bool valid() const noexcept { return base_ != nullptr; }
    bool contains(uintptr_t addr, size_t len) const noexcept;

    // Commits zero-filled pages as ReadWrite; offset and length must be page aligned and inside the region.
    Status commit(size_t offset, size_t length);
    // Changes protection of committed pages (page aligned). Executable pages get an instruction-cache flush.
    Status protect(size_t offset, size_t length, Prot p);
    Result<PageInfo> info(size_t offset) const;
    void release() noexcept;

private:
    uint8_t* base_ = nullptr;
    size_t size_ = 0;
};

}  // namespace orvyn::memory
