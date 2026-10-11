#include "orvyn/memory.hpp"
#include "guest_code.hpp"
#include "test_framework.hpp"

using namespace orvyn;
using namespace orvyn::memory;
using namespace orvyn::testing;

static PageInfo::State info_state(const Region& r, size_t off) { return r.info(off).value().state; }

ORVYN_TEST(page_sizes_and_alignment_helpers) {
    CHECK_EQ(page_size(), size_t{4096});
    CHECK_EQ(allocation_granularity(), size_t{65536});
    CHECK(is_aligned(0x1000, 0x1000));
    CHECK(!is_aligned(0x1001, 0x1000));
    CHECK(!is_aligned(5, 0));
    CHECK_EQ(align_up(1, 4096).value(), uint64_t{4096});
    CHECK_EQ(align_up(4096, 4096).value(), uint64_t{4096});
    CHECK(!align_up(UINT64_MAX, 4096).has_value());   // overflow
    CHECK(!align_up(1, 0).has_value());
}

ORVYN_TEST(mach_o_protections_map_to_wx_safe_prots) {
    CHECK(from_mach_prot(0).value() == Prot::None);
    CHECK(from_mach_prot(1).value() == Prot::Read);
    CHECK(from_mach_prot(3).value() == Prot::ReadWrite);
    CHECK(from_mach_prot(5).value() == Prot::ReadExecute);
    CHECK_ERR(from_mach_prot(7), ErrorCode::Unsupported);   // rwx: refused by W^X
    CHECK_ERR(from_mach_prot(6), ErrorCode::Unsupported);   // -wx
    CHECK_ERR(from_mach_prot(2), ErrorCode::Unsupported);   // write without read
    CHECK_ERR(from_mach_prot(4), ErrorCode::Unsupported);   // execute without read
    CHECK_ERR(from_mach_prot(8), ErrorCode::Malformed);     // unknown bit
    CHECK_ERR(from_mach_prot(-1), ErrorCode::Malformed);
}

ORVYN_TEST(reserve_commit_protect_query_cycle) {
    auto r = Region::reserve(0, 3 * 4096, false);
    REQUIRE(r.ok());
    Region reg = std::move(r.value());
    REQUIRE(reg.valid());
    CHECK_EQ(reg.size(), size_t{3 * 4096});
    CHECK(info_state(reg, 0) == PageInfo::State::Reserved);

    REQUIRE(reg.commit(0, 4096).ok());
    CHECK(info_state(reg, 0) == PageInfo::State::Committed);
    CHECK(info_state(reg, 4096) == PageInfo::State::Reserved);    // only the first page is committed
    reg.base()[0] = 0x5a;                                           // committed pages are writable
    CHECK_EQ(int{reg.base()[100]}, 0);                              // and zero-filled

    auto i = reg.info(0).value();
    CHECK(i.writable && !i.executable);

    REQUIRE(reg.protect(0, 4096, Prot::Read).ok());
    i = reg.info(0).value();
    CHECK(!i.writable && !i.executable && i.prot == Prot::Read);
    REQUIRE(reg.protect(0, 4096, Prot::ReadExecute).ok());
    i = reg.info(0).value();
    CHECK(i.executable && !i.writable);                             // never writable AND executable
    REQUIRE(reg.protect(0, 4096, Prot::None).ok());
    CHECK(reg.info(0).value().prot == Prot::None);
}

ORVYN_TEST(invalid_ranges_are_rejected_without_side_effects) {
    auto reg = take(Region::reserve(0, 4 * 4096, false));
    CHECK_ERR(reg.commit(1, 4096), ErrorCode::InvalidArgument);          // unaligned offset
    CHECK_ERR(reg.commit(0, 100), ErrorCode::InvalidArgument);           // unaligned length
    CHECK_ERR(reg.commit(4 * 4096, 4096), ErrorCode::OutOfBounds);       // past the end
    CHECK_ERR(reg.commit(4096, SIZE_MAX & ~size_t{4095}), ErrorCode::OutOfBounds);   // overflowing length
    CHECK_ERR(reg.protect(0, 4096, Prot::Read), ErrorCode::Io);          // not committed yet: VirtualProtect fails
    CHECK_ERR(reg.protect(2, 4096, Prot::Read), ErrorCode::InvalidArgument);
    CHECK_ERR(reg.protect(0, 8 * 4096, Prot::Read), ErrorCode::OutOfBounds);
    CHECK(info_state(reg, 0) == PageInfo::State::Reserved);
    CHECK(reg.commit(0, 0).ok());                                        // empty range is a no-op
    CHECK_ERR(Region::reserve(0, 0, false), ErrorCode::InvalidArgument);
    CHECK_ERR(Region::reserve(0x100001000, 4096, false), ErrorCode::InvalidArgument);  // not 64 KiB aligned
    Region empty;
    CHECK_ERR(empty.commit(0, 4096), ErrorCode::InvalidArgument);
    CHECK_ERR(empty.info(0), ErrorCode::OutOfBounds);
}

ORVYN_TEST(region_contains_is_overflow_safe) {
    auto reg = take(Region::reserve(0, 8192, false));
    uintptr_t b = reinterpret_cast<uintptr_t>(reg.base());
    CHECK(reg.contains(b, 8192));
    CHECK(reg.contains(b + 100, 100));
    CHECK(!reg.contains(b, 8193));
    CHECK(!reg.contains(b - 1, 2));
    CHECK(!reg.contains(b + 8191, 2));
    CHECK(!reg.contains(UINTPTR_MAX, 2));
    CHECK(!reg.contains(b, SIZE_MAX));
}

ORVYN_TEST(destruction_releases_the_address_range) {
    uint8_t* base = nullptr;
    {
        auto reg = take(Region::reserve(0, 4096, false));
        REQUIRE(reg.commit(0, 4096).ok());
        base = reg.base();
        CHECK(query_address(base).state == PageInfo::State::Committed);
    }
    CHECK(query_address(base).state == PageInfo::State::Free);   // MEM_RELEASE ran
}

ORVYN_TEST(move_semantics_transfer_ownership) {
    auto a = take(Region::reserve(0, 4096, false));
    uint8_t* base = a.base();
    Region b = std::move(a);
    CHECK(!a.valid());
    CHECK(b.valid() && b.base() == base);
    Region c;
    c = std::move(b);
    CHECK(!b.valid());
    CHECK(c.base() == base);
    c.release();
    CHECK(!c.valid());
    CHECK(query_address(base).state == PageInfo::State::Free);
    c.release();   // idempotent
}

ORVYN_TEST(preferred_address_taken_relocates_only_when_allowed) {
    auto first = Region::reserve(0x200000000ull, 0x10000, false);
    if (!first.ok()) SKIP("0x200000000 is not available in this process");
    CHECK_ERR(Region::reserve(0x200000000ull, 0x10000, false), ErrorCode::Io);          // taken, no relocation
    auto moved = Region::reserve(0x200000000ull, 0x10000, true);
    REQUIRE(moved.ok());
    CHECK(reinterpret_cast<uintptr_t>(moved.value().base()) != 0x200000000ull);
}

ORVYN_TEST(executable_pages_run_and_data_pages_do_not_execute) {
    // A page with `mov eax, 123 ; ret` becomes executable only through protect(): RW -> RX.
    CodePage page({0xB8, 0x7B, 0x00, 0x00, 0x00, 0xC3});
    auto info = query_address(reinterpret_cast<void*>(page.address()));
    CHECK(info.executable && !info.writable);
    auto o = run_code({0xB8, 0x7B, 0x00, 0x00, 0x00, 0xC3});
    CHECK_EQ(static_cast<uint32_t>(o.value), 123u);
}

ORVYN_TEST(guest_stack_has_a_guard_page_and_writable_body) {
    auto st = make_stack(64 * 1024);
    auto guard = st.region().info(0).value();
    CHECK(guard.prot == Prot::None);                        // committed but inaccessible
    auto body = st.region().info(4096).value();
    CHECK(body.writable && !body.executable);
    CHECK_EQ(st.top() % 4096, uint64_t{0});
    CHECK_ERR(abi::GuestStack::create(4096), ErrorCode::InvalidArgument);    // too small for guard + body
}
