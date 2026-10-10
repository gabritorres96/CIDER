#include <format>
#include <utility>

#include "cider/log.hpp"
#include "cider/memory.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace cider::memory {

#ifdef _WIN32
namespace {
DWORD to_win(Prot p) {
    switch (p) {
        case Prot::None: return PAGE_NOACCESS;
        case Prot::Read: return PAGE_READONLY;
        case Prot::ReadWrite: return PAGE_READWRITE;
        case Prot::ReadExecute: return PAGE_EXECUTE_READ;
    }
    return PAGE_NOACCESS;
}
Prot from_win(DWORD p) {
    switch (p & 0xff) {
        case PAGE_READONLY: return Prot::Read;
        case PAGE_READWRITE: case PAGE_WRITECOPY: return Prot::ReadWrite;
        case PAGE_EXECUTE_READ: return Prot::ReadExecute;
        case PAGE_EXECUTE: return Prot::ReadExecute;
        case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return Prot::ReadWrite;  // flagged via writable/executable
        default: return Prot::None;
    }
}
}  // namespace

size_t page_size() {
    static const size_t v = [] { SYSTEM_INFO si; GetSystemInfo(&si); return static_cast<size_t>(si.dwPageSize); }();
    return v;
}
size_t allocation_granularity() {
    static const size_t v = [] { SYSTEM_INFO si; GetSystemInfo(&si); return static_cast<size_t>(si.dwAllocationGranularity); }();
    return v;
}

PageInfo query_address(const void* p) {
    PageInfo pi;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof mbi) == 0) return pi;
    pi.region_size = mbi.RegionSize;
    if (mbi.State == MEM_FREE) return pi;
    pi.state = mbi.State == MEM_COMMIT ? PageInfo::State::Committed : PageInfo::State::Reserved;
    if (pi.state == PageInfo::State::Committed) {
        pi.prot = from_win(mbi.Protect);
        DWORD b = mbi.Protect & 0xff;
        pi.executable = b == PAGE_EXECUTE || b == PAGE_EXECUTE_READ || b == PAGE_EXECUTE_READWRITE || b == PAGE_EXECUTE_WRITECOPY;
        pi.writable = b == PAGE_READWRITE || b == PAGE_WRITECOPY || b == PAGE_EXECUTE_READWRITE || b == PAGE_EXECUTE_WRITECOPY;
    }
    return pi;
}

Result<Region> Region::reserve(uintptr_t preferred, size_t size, bool allow_relocation) {
    if (size == 0) return Error{ErrorCode::InvalidArgument, "cannot reserve an empty region"};
    auto rounded = align_up(size, page_size());
    if (!rounded) return Error{ErrorCode::Overflow, "region size overflows"};
    if (preferred && !is_aligned(preferred, allocation_granularity()))
        return Error{ErrorCode::InvalidArgument, std::format("preferred address {:#x} is not {}-byte aligned", preferred, allocation_granularity())};
    void* p = VirtualAlloc(reinterpret_cast<void*>(preferred), static_cast<size_t>(*rounded), MEM_RESERVE, PAGE_NOACCESS);
    if (!p && preferred && allow_relocation) {
        log::debug(log::Category::Memory, "preferred base {:#x} unavailable (error {}); letting Windows choose", preferred, GetLastError());
        p = VirtualAlloc(nullptr, static_cast<size_t>(*rounded), MEM_RESERVE, PAGE_NOACCESS);
    }
    if (!p) return Error{ErrorCode::Io, std::format("VirtualAlloc(MEM_RESERVE, {:#x} bytes) failed (error {})", *rounded, GetLastError())};
    Region r;
    r.base_ = static_cast<uint8_t*>(p);
    r.size_ = static_cast<size_t>(*rounded);
    return r;
}

bool Region::contains(uintptr_t addr, size_t len) const noexcept {
    uintptr_t b = reinterpret_cast<uintptr_t>(base_);
    return valid() && addr >= b && len <= size_ && addr - b <= size_ - len;
}

Status Region::commit(size_t offset, size_t length) {
    if (!valid()) return Error{ErrorCode::InvalidArgument, "region is not reserved"};
    if (length == 0) return {};
    if (!is_aligned(offset, page_size()) || !is_aligned(length, page_size()))
        return Error{ErrorCode::InvalidArgument, std::format("commit range [{:#x}, +{:#x}) is not page aligned", offset, length)};
    if (offset > size_ || length > size_ - offset)
        return Error{ErrorCode::OutOfBounds, std::format("commit range [{:#x}, +{:#x}) exceeds region size {:#x}", offset, length, size_)};
    if (!VirtualAlloc(base_ + offset, length, MEM_COMMIT, PAGE_READWRITE))
        return Error{ErrorCode::Io, std::format("VirtualAlloc(MEM_COMMIT, {:#x} bytes) failed (error {})", length, GetLastError())};
    return {};
}

Status Region::protect(size_t offset, size_t length, Prot p) {
    if (!valid()) return Error{ErrorCode::InvalidArgument, "region is not reserved"};
    if (length == 0) return {};
    if (!is_aligned(offset, page_size()) || !is_aligned(length, page_size()))
        return Error{ErrorCode::InvalidArgument, std::format("protect range [{:#x}, +{:#x}) is not page aligned", offset, length)};
    if (offset > size_ || length > size_ - offset)
        return Error{ErrorCode::OutOfBounds, std::format("protect range [{:#x}, +{:#x}) exceeds region size {:#x}", offset, length, size_)};
    DWORD old = 0;
    if (!VirtualProtect(base_ + offset, length, to_win(p), &old))
        return Error{ErrorCode::Io, std::format("VirtualProtect({}) failed (error {})", to_string(p), GetLastError())};
    if (p == Prot::ReadExecute) FlushInstructionCache(GetCurrentProcess(), base_ + offset, length);
    return {};
}

Result<PageInfo> Region::info(size_t offset) const {
    if (!valid() || offset >= size_) return Error{ErrorCode::OutOfBounds, "offset outside the region"};
    return query_address(base_ + offset);
}

void Region::release() noexcept {
    if (base_) {
        VirtualFree(base_, 0, MEM_RELEASE);
        base_ = nullptr;
        size_ = 0;
    }
}
#else
size_t page_size() { return 4096; }
size_t allocation_granularity() { return 4096; }
PageInfo query_address(const void*) { return {}; }
Result<Region> Region::reserve(uintptr_t, size_t, bool) { return Error{ErrorCode::Unsupported, "the Mach-O loader is only implemented on Windows"}; }
bool Region::contains(uintptr_t, size_t) const noexcept { return false; }
Status Region::commit(size_t, size_t) { return Error{ErrorCode::Unsupported, "unsupported platform"}; }
Status Region::protect(size_t, size_t, Prot) { return Error{ErrorCode::Unsupported, "unsupported platform"}; }
Result<PageInfo> Region::info(size_t) const { return Error{ErrorCode::Unsupported, "unsupported platform"}; }
void Region::release() noexcept {}
#endif

Region::~Region() { release(); }
Region::Region(Region&& o) noexcept : base_(std::exchange(o.base_, nullptr)), size_(std::exchange(o.size_, 0)) {}
Region& Region::operator=(Region&& o) noexcept {
    if (this != &o) {
        release();
        base_ = std::exchange(o.base_, nullptr);
        size_ = std::exchange(o.size_, 0);
    }
    return *this;
}

}  // namespace cider::memory
