#pragma once
// Helpers for runtime tests: hand-assembled guest machine code in an executable page, and the paths
// of the real Mach-O execution fixtures.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "cider/abi.hpp"
#include "cider/libsystem.hpp"
#include "cider/memory.hpp"
#include "cider/util.hpp"
#include "test_framework.hpp"

namespace cider::testing {

// Unwraps a Result (move-only payloads allowed); a failure aborts the test with the error message.
template <class T>
T take(Result<T> r) {
    if (!r.ok()) { report_failure(__FILE__, __LINE__, "unexpected error: " + r.error().message); throw AbortTest{}; }
    return std::move(r.value());
}

inline const std::filesystem::path kExecFixtures = std::filesystem::path(CIDER_EXEC_FIXTURES_DIR);

inline std::vector<uint8_t> exec_fixture(const std::string& name) {
    auto r = read_file(kExecFixtures / "bin" / name, 16u << 20);
    if (!r.ok()) { report_failure(__FILE__, __LINE__, "execution fixture missing: " + name); throw AbortTest{}; }
    return r.value();
}

// Machine code copied into a private RW region, then switched to RX (never RWX).
class CodePage {
public:
    explicit CodePage(const std::vector<uint8_t>& code) {
        size_t sz = memory::align_up(std::max<size_t>(code.size(), 1), memory::page_size()).value();
        auto r = memory::Region::reserve(0, sz, false);
        if (!r.ok()) { report_failure(__FILE__, __LINE__, r.error().message); throw AbortTest{}; }
        region_ = std::move(r.value());
        if (!region_.commit(0, sz).ok()) throw AbortTest{};
        std::memcpy(region_.base(), code.data(), code.size());
        if (!region_.protect(0, sz, memory::Prot::ReadExecute).ok()) throw AbortTest{};
    }
    uint64_t address() const { return reinterpret_cast<uint64_t>(region_.base()); }

private:
    memory::Region region_;
};

inline abi::GuestStack make_stack(size_t size = 256 * 1024) {
    auto s = abi::GuestStack::create(size);
    if (!s.ok()) { report_failure(__FILE__, __LINE__, s.error().message); throw AbortTest{}; }
    return std::move(s.value());
}

// Runs raw machine code as a System V function with up to 4 integer arguments.
inline abi::CallOutcome run_code(const std::vector<uint8_t>& code, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0) {
    CodePage page(code);
    auto stack = make_stack();
    uint64_t sp = stack.top() - 64;  // 16-byte aligned (top is page aligned)
    return abi::call_guest(page.address(), stack, sp, a0, a1, a2, a3);
}

inline void put_u64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }

// Calls a libSystem symbol (a System V thunk) from guest code with up to four integer arguments and
// returns rax. The call goes guest -> thunk -> host function -> thunk -> guest, exactly like a Mach-O binary's.
inline abi::CallOutcome call_host(libsystem::LibSystemProvider& p, const char* symbol, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0) {
    auto sym = p.lookup("", symbol);
    if (!sym) { report_failure(__FILE__, __LINE__, std::string("symbol not implemented: ") + symbol); throw AbortTest{}; }
    std::vector<uint8_t> c = {0x48, 0x83, 0xEC, 0x08};                    // sub rsp, 8 (System V call alignment)
    auto movq = [&](std::initializer_list<uint8_t> op, uint64_t v) { c.insert(c.end(), op); put_u64(c, v); };
    movq({0x48, 0xBF}, a0); movq({0x48, 0xBE}, a1); movq({0x48, 0xBA}, a2); movq({0x48, 0xB9}, a3);   // rdi rsi rdx rcx
    movq({0x48, 0xB8}, sym->address);                                      // mov rax, thunk
    c.insert(c.end(), {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x08, 0xC3});         // call rax ; add rsp,8 ; ret
    return run_code(c);
}

}  // namespace cider::testing
