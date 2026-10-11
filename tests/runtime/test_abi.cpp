// System V AMD64 <-> Windows x64 bridge, tested with hand-assembled machine code (not Mach-O files):
// argument registers, stack alignment, callee-saved registers, callbacks into host functions, exit.
#include <cmath>
#include <cstring>

#include "orvyn/abi.hpp"
#include "guest_code.hpp"
#include "test_framework.hpp"

using namespace orvyn;
using namespace orvyn::testing;

extern "C" uint64_t orvyn_abi_selftest(abi::GuestCall* call, void* snapshot);

namespace {

struct RegSnapshot {
    uint64_t gpr[8];      // rbx rbp rdi rsi r12 r13 r14 r15
    uint8_t xmm[10][16];  // xmm6..xmm15
};

// Host (Windows ABI) functions the guest calls through thunks.
int host_add(int a, int b) { return a + b; }
int64_t host_six(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f) { return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f; }
double host_half(double x) { return x / 2.0; }
double host_hypot2(double a, double b) { return a * a + b * b; }
int g_calls = 0;
void host_count() { ++g_calls; }
void host_exit77() { abi::request_guest_exit(77); }
const char* host_text() { return "from-host"; }

// Builds guest code: sub rsp,8 ; <body> ; add rsp,8 ; ret   (body is entered with rsp%16==0 as System V requires at calls)
std::vector<uint8_t> framed(const std::vector<uint8_t>& body) {
    std::vector<uint8_t> c = {0x48, 0x83, 0xEC, 0x08};
    c.insert(c.end(), body.begin(), body.end());
    c.insert(c.end(), {0x48, 0x83, 0xC4, 0x08, 0xC3});
    return c;
}
void mov_rdi(std::vector<uint8_t>& c, uint64_t v) { c.insert(c.end(), {0x48, 0xBF}); put_u64(c, v); }
void mov_rsi(std::vector<uint8_t>& c, uint64_t v) { c.insert(c.end(), {0x48, 0xBE}); put_u64(c, v); }
void mov_rdx(std::vector<uint8_t>& c, uint64_t v) { c.insert(c.end(), {0x48, 0xBA}); put_u64(c, v); }
void mov_rcx(std::vector<uint8_t>& c, uint64_t v) { c.insert(c.end(), {0x48, 0xB9}); put_u64(c, v); }
void mov_r8(std::vector<uint8_t>& c, uint64_t v) { c.insert(c.end(), {0x49, 0xB8}); put_u64(c, v); }
void mov_r9(std::vector<uint8_t>& c, uint64_t v) { c.insert(c.end(), {0x49, 0xB9}); put_u64(c, v); }
void call_abs(std::vector<uint8_t>& c, uint64_t target) {
    c.insert(c.end(), {0x48, 0xB8}); put_u64(c, target);   // mov rax, target
    c.insert(c.end(), {0xFF, 0xD0});                       // call rax
}

}  // namespace

ORVYN_TEST(guest_returns_a_constant_computed_by_guest_instructions) {
    auto o = run_code({0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3});      // mov eax, 42 ; ret
    CHECK(!o.exited);
    CHECK_EQ(static_cast<uint32_t>(o.value), 42u);
    // different immediate => different result: the value comes from the executed instruction
    CHECK_EQ(static_cast<uint32_t>(run_code({0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3}).value), 7u);
    CHECK_EQ(static_cast<uint32_t>(run_code({0xB8, 0x63, 0x00, 0x00, 0x00, 0xC3}).value), 99u);
}

ORVYN_TEST(system_v_integer_argument_registers_are_rdi_rsi_rdx_rcx) {
    // rax = rdi*1 + rsi*10 + rdx*100 + rcx*1000 using lea/imul so each register's role is distinguishable
    std::vector<uint8_t> code = {
        0x48, 0x6B, 0xF6, 0x0A,              // imul rsi, rsi, 10
        0x48, 0x6B, 0xD2, 0x64,              // imul rdx, rdx, 100
        0x48, 0x69, 0xC9, 0xE8, 0x03, 0x00, 0x00,   // imul rcx, rcx, 1000
        0x48, 0x89, 0xF8,                    // mov rax, rdi
        0x48, 0x01, 0xF0,                    // add rax, rsi
        0x48, 0x01, 0xD0,                    // add rax, rdx
        0x48, 0x01, 0xC8,                    // add rax, rcx
        0xC3};
    CHECK_EQ(run_code(code, 1, 2, 3, 4).value, uint64_t{4321});
    CHECK_EQ(run_code(code, 9, 0, 0, 1).value, uint64_t{1009});
    CHECK_EQ(run_code(code, 0, 7, 0, 0).value, uint64_t{70});
}

ORVYN_TEST(guest_is_entered_with_a_system_v_aligned_stack) {
    // At function entry (after CALL) rsp % 16 must be 8.
    auto o = run_code({0x48, 0x89, 0xE0, 0x48, 0x83, 0xE0, 0x0F, 0xC3});   // mov rax,rsp ; and rax,15 ; ret
    CHECK_EQ(o.value, uint64_t{8});
}

ORVYN_TEST(guest_runs_on_its_own_stack_not_the_host_stack) {
    CodePage page({0x48, 0x89, 0xE0, 0xC3});                               // mov rax, rsp ; ret
    auto stack = make_stack();
    uint64_t sp = stack.top() - 64;
    auto o = abi::call_guest(page.address(), stack, sp);
    CHECK(o.value < stack.top() && o.value > reinterpret_cast<uint64_t>(stack.base()));   // rsp inside the guest stack
    int local = 0;
    CHECK(o.value != reinterpret_cast<uint64_t>(&local));
}

ORVYN_TEST(red_zone_below_rsp_is_usable_by_guest_leaf_functions) {
    // mov qword [rsp-8], 123 ; mov rax, [rsp-8] ; ret   (System V leaf functions use the 128-byte red zone)
    auto o = run_code({0x48, 0xC7, 0x44, 0x24, 0xF8, 0x7B, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x44, 0x24, 0xF8, 0xC3});
    CHECK_EQ(o.value, uint64_t{123});
}

ORVYN_TEST(host_callee_saved_registers_survive_guest_clobbering) {
    // Guest clobbers every register Windows requires to be preserved: rbx rbp rdi rsi r12-r15 xmm6-xmm15.
    std::vector<uint8_t> code;
    auto movq = [&](std::vector<uint8_t> op, uint64_t v) { code.insert(code.end(), op.begin(), op.end()); put_u64(code, v); };
    movq({0x48, 0xBB}, 0xDEAD000000000001ull);   // rbx
    movq({0x48, 0xBD}, 0xDEAD000000000002ull);   // rbp
    movq({0x48, 0xBF}, 0xDEAD000000000003ull);   // rdi
    movq({0x48, 0xBE}, 0xDEAD000000000004ull);   // rsi
    movq({0x49, 0xBC}, 0xDEAD000000000005ull);   // r12
    movq({0x49, 0xBD}, 0xDEAD000000000006ull);   // r13
    movq({0x49, 0xBE}, 0xDEAD000000000007ull);   // r14
    movq({0x49, 0xBF}, 0xDEAD000000000008ull);   // r15
    code.insert(code.end(), {0x66, 0x0F, 0x76, 0xF6, 0x66, 0x0F, 0x76, 0xFF});                 // pcmpeqd xmm6,xmm6 ; xmm7
    for (int modrm : {0xC0, 0xC9, 0xD2, 0xDB, 0xE4, 0xED, 0xF6, 0xFF})                           // xmm8..xmm15 = all ones
        code.insert(code.end(), {uint8_t{0x66}, uint8_t{0x45}, uint8_t{0x0F}, uint8_t{0x76}, static_cast<uint8_t>(modrm)});
    code.insert(code.end(), {0x31, 0xC0, 0xC3});                                                // xor eax,eax ; ret

    CodePage page(code);
    auto stack = make_stack();
    abi::GuestCall call;
    call.fn = reinterpret_cast<void*>(page.address());
    call.stack_top = stack.top() - 64;

    RegSnapshot snap{};
    for (int i = 0; i < 8; ++i) snap.gpr[i] = 0x1111111100000000ull + static_cast<uint64_t>(i) * 0x0101010101ull;
    for (int i = 0; i < 10; ++i) for (int b = 0; b < 16; ++b) snap.xmm[i][b] = static_cast<uint8_t>(0x30 + i * 16 + b);
    const RegSnapshot before = snap;

    orvyn_abi_selftest(&call, &snap);

    for (int i = 0; i < 8; ++i) CHECK_EQ(snap.gpr[i], before.gpr[i]);
    for (int i = 0; i < 10; ++i) CHECK(std::memcmp(snap.xmm[i], before.xmm[i], 16) == 0);
}

ORVYN_TEST(guest_calls_a_windows_function_through_a_thunk) {
    auto table = take(abi::ThunkTable::build({reinterpret_cast<void*>(&host_add)}));
    std::vector<uint8_t> body;
    mov_rdi(body, 20);
    mov_rsi(body, 22);
    call_abs(body, table.address(0));
    auto o = run_code(framed(body));
    CHECK_EQ(static_cast<uint32_t>(o.value), 42u);
    // negative values: 32-bit int semantics survive the register shuffle
    body.clear(); mov_rdi(body, static_cast<uint64_t>(-5)); mov_rsi(body, 2); call_abs(body, table.address(0));
    CHECK_EQ(static_cast<int32_t>(run_code(framed(body)).value), -3);
}

ORVYN_TEST(fifth_and_sixth_integer_arguments_reach_windows_stack_slots) {
    auto table = take(abi::ThunkTable::build({reinterpret_cast<void*>(&host_six)}));
    std::vector<uint8_t> body;
    mov_rdi(body, 1); mov_rsi(body, 2); mov_rdx(body, 3); mov_rcx(body, 4); mov_r8(body, 5); mov_r9(body, 6);
    call_abs(body, table.address(0));
    CHECK_EQ(run_code(framed(body)).value, uint64_t{1 + 4 + 9 + 16 + 25 + 36});
}

ORVYN_TEST(double_arguments_and_results_travel_in_xmm0_xmm1) {
    auto table = take(abi::ThunkTable::build({reinterpret_cast<void*>(&host_half), reinterpret_cast<void*>(&host_hypot2)}));
    auto bits = [](double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; };
    // mov rax, bits ; movq xmm0, rax ; call half ; movq rax, xmm0 ; (framed adds ret)
    std::vector<uint8_t> body = {0x48, 0xB8};
    put_u64(body, bits(9.0));
    body.insert(body.end(), {0x66, 0x48, 0x0F, 0x6E, 0xC0});
    call_abs(body, table.address(0));
    body.insert(body.end(), {0x66, 0x48, 0x0F, 0x7E, 0xC0});
    uint64_t r = run_code(framed(body)).value;
    double d; std::memcpy(&d, &r, 8);
    CHECK_EQ(d, 4.5);

    // two doubles: xmm0 = 3, xmm1 = 4  ->  25
    body = {0x48, 0xB8}; put_u64(body, bits(3.0));
    body.insert(body.end(), {0x66, 0x48, 0x0F, 0x6E, 0xC0});                       // movq xmm0, rax
    body.insert(body.end(), {0x48, 0xB8}); put_u64(body, bits(4.0));
    body.insert(body.end(), {0x66, 0x48, 0x0F, 0x6E, 0xC8});                       // movq xmm1, rax
    call_abs(body, table.address(1));
    body.insert(body.end(), {0x66, 0x48, 0x0F, 0x7E, 0xC0});
    r = run_code(framed(body)).value;
    std::memcpy(&d, &r, 8);
    CHECK_EQ(d, 25.0);
}

ORVYN_TEST(host_function_runs_on_the_host_stack_and_guest_stack_is_restored) {
    // guest: push a marker on its stack, call a host function, pop the marker, return it
    auto table = take(abi::ThunkTable::build({reinterpret_cast<void*>(&host_count)}));
    g_calls = 0;
    std::vector<uint8_t> body = {0x48, 0xC7, 0x04, 0x24, 0x39, 0x05, 0x00, 0x00};      // mov qword [rsp], 1337
    call_abs(body, table.address(0));
    body.insert(body.end(), {0x48, 0x8B, 0x04, 0x24});                                  // mov rax, [rsp]
    auto o = run_code(framed(body));
    CHECK_EQ(g_calls, 1);
    CHECK_EQ(o.value, uint64_t{1337});
}

ORVYN_TEST(exit_stops_the_guest_immediately) {
    auto table = take(abi::ThunkTable::build({reinterpret_cast<void*>(&host_exit77)}));
    std::vector<uint8_t> body;
    call_abs(body, table.address(0));
    body.insert(body.end(), {0xB8, 0x05, 0x00, 0x00, 0x00});                            // mov eax,5 : must NOT run
    auto o = run_code(framed(body));
    CHECK(o.exited);
    CHECK_EQ(o.value, uint64_t{77});
}

ORVYN_TEST(guest_can_receive_a_host_pointer_result) {
    auto table = take(abi::ThunkTable::build({reinterpret_cast<void*>(&host_text)}));
    std::vector<uint8_t> body;
    call_abs(body, table.address(0));
    uint64_t p = run_code(framed(body)).value;
    CHECK_EQ(std::string(reinterpret_cast<const char*>(p)), std::string("from-host"));
}

ORVYN_TEST(thunk_pages_are_executable_and_not_writable) {
    auto table = take(abi::ThunkTable::build({reinterpret_cast<void*>(&host_add), reinterpret_cast<void*>(&host_half)}));
    CHECK_EQ(table.size(), size_t{2});
    CHECK(table.address(1) - table.address(0) == abi::ThunkTable::kStride);
    auto info = memory::query_address(reinterpret_cast<void*>(table.address(0)));
    CHECK(info.executable && !info.writable);
    CHECK(abi::ThunkTable::build({}).ok());   // an empty table is valid
}

ORVYN_TEST(guest_call_struct_layout_matches_the_assembly) {
    // offsets are hard-coded in entry_trampoline.asm; abi.hpp static_asserts them, this documents the numbers
    CHECK_EQ(offsetof(abi::GuestCall, host_rsp), size_t{48});
    CHECK_EQ(offsetof(abi::GuestCall, exited), size_t{72});
    CHECK_EQ(sizeof(abi::GuestCall), size_t{200});
}
