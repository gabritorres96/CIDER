#include <cstring>
#include <format>

#include "orvyn/abi.hpp"
#include "orvyn/log.hpp"

namespace orvyn::abi {

Result<GuestStack> GuestStack::create(size_t size) {
    auto rounded = memory::align_up(size, memory::page_size());
    if (!rounded || *rounded < 2 * memory::page_size()) return Error{ErrorCode::InvalidArgument, "guest stack too small"};
    auto region = memory::Region::reserve(0, static_cast<size_t>(*rounded), false);
    if (!region.ok()) return region.error();
    GuestStack s;
    s.region_ = std::move(region.value());
    if (auto c = s.region_.commit(0, s.region_.size()); !c.ok()) return c.error();
    // guard page at the lowest addresses: overflowing the guest stack faults instead of corrupting memory
    if (auto g = s.region_.protect(0, memory::page_size(), memory::Prot::None); !g.ok()) return g.error();
    log::debug(log::Category::Memory, "guest stack {:#x}..{:#x} ({} KiB, guard page at the bottom)", reinterpret_cast<uint64_t>(s.region_.base()), s.top(), s.region_.size() / 1024);
    return s;
}

Result<GuestArgs> build_guest_args(GuestStack& stack, const std::vector<std::string>& argv, const std::vector<std::string>& envp,
                                   const std::vector<std::string>& apple) {
    // Layout, growing downwards from the top of the stack:
    //   strings (apple, env, argv)  |  NULL apple[] NULL env[] NULL argv[]  | 16-byte aligned initial sp
    uint8_t* const lo = stack.base() + memory::page_size();  // above the guard page
    uint8_t* cur = stack.base() + stack.size();

    auto put_strings = [&](const std::vector<std::string>& v, std::vector<uint64_t>& addrs) -> Status {
        addrs.resize(v.size());
        for (size_t i = v.size(); i-- > 0;) {
            size_t need = v[i].size() + 1;
            if (static_cast<size_t>(cur - lo) < need) return Error{ErrorCode::LimitExceeded, "arguments do not fit on the guest stack"};
            cur -= need;
            std::memcpy(cur, v[i].c_str(), need);
            addrs[i] = reinterpret_cast<uint64_t>(cur);
        }
        return {};
    };
    std::vector<uint64_t> a_ptr, e_ptr, v_ptr;
    ORVYN_CHECK(put_strings(apple, a_ptr));
    ORVYN_CHECK(put_strings(envp, e_ptr));
    ORVYN_CHECK(put_strings(argv, v_ptr));

    const size_t pointers = (a_ptr.size() + 1) + (e_ptr.size() + 1) + (v_ptr.size() + 1);
    cur = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(cur) & ~uintptr_t{15});
    const size_t bytes = pointers * 8;
    if (static_cast<size_t>(cur - lo) < bytes + 4096) return Error{ErrorCode::LimitExceeded, "arguments leave no room for the guest stack"};
    cur -= bytes;
    cur = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(cur) & ~uintptr_t{15});

    GuestArgs g;
    uint64_t* w = reinterpret_cast<uint64_t*>(cur);
    g.argv = reinterpret_cast<uint64_t>(w);
    for (uint64_t p : v_ptr) *w++ = p;
    *w++ = 0;
    g.envp = reinterpret_cast<uint64_t>(w);
    for (uint64_t p : e_ptr) *w++ = p;
    *w++ = 0;
    g.apple = reinterpret_cast<uint64_t>(w);
    for (uint64_t p : a_ptr) *w++ = p;
    *w++ = 0;
    g.argc = argv.size();
    g.initial_sp = reinterpret_cast<uint64_t>(cur) & ~uint64_t{15};
    return g;
}

}  // namespace orvyn::abi
