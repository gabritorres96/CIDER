// System V AMD64 <-> Windows x64, second half (ORVYN 0.3): the raw thunk and the argument readers. The guest code is
// hand-assembled machine code, so the bridge is verified independently of any compiler. The cases are the ones a
// positional register remapping cannot express: mixed integer/double signatures, more than six integer arguments,
// stack arguments, variadic calls (AL), aggregate returns, preservation of callee-saved registers and the red zone.
#include <cmath>
#include <cstring>

#include "orvyn/abi.hpp"
#include "guest_code.hpp"
#include "test_framework.hpp"

using namespace orvyn;
using namespace orvyn::testing;

namespace {

uint64_t bits(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }
double from_bits(uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }

// ---- assembler helpers ------------------------------------------------------------------------------------------
using Code = std::vector<uint8_t>;
void mov_rax(Code& c, uint64_t v) { c.insert(c.end(), {0x48, 0xB8}); put_u64(c, v); }
void mov_reg(Code& c, std::initializer_list<uint8_t> op, uint64_t v) { c.insert(c.end(), op); put_u64(c, v); }
void mov_rdi(Code& c, uint64_t v) { mov_reg(c, {0x48, 0xBF}, v); }
void mov_rsi(Code& c, uint64_t v) { mov_reg(c, {0x48, 0xBE}, v); }
void mov_rdx(Code& c, uint64_t v) { mov_reg(c, {0x48, 0xBA}, v); }
void mov_rcx(Code& c, uint64_t v) { mov_reg(c, {0x48, 0xB9}, v); }
void mov_r8(Code& c, uint64_t v) { mov_reg(c, {0x49, 0xB8}, v); }
void mov_r9(Code& c, uint64_t v) { mov_reg(c, {0x49, 0xB9}, v); }
void movq_xmm(Code& c, int n, double d) {                     // mov rax, imm64 ; movq xmmN, rax
    mov_rax(c, bits(d));
    c.insert(c.end(), {0x66, 0x48, 0x0F, 0x6E, static_cast<uint8_t>(0xC0 | (n << 3))});
}
void push_u64(Code& c, uint64_t v) { mov_rax(c, v); c.push_back(0x50); }                // mov rax,v ; push rax
void call_r11(Code& c, uint64_t target) { mov_reg(c, {0x49, 0xBB}, target); c.insert(c.end(), {0x41, 0xFF, 0xD3}); }   // keeps rax (AL) intact
void add_rsp(Code& c, uint8_t n) { c.insert(c.end(), {0x48, 0x83, 0xC4, n}); }
void ret_xmm0(Code& c) { c.insert(c.end(), {0x66, 0x48, 0x0F, 0x7E, 0xC0}); }          // movq rax, xmm0
// sub rsp,8 ; body ; add rsp,8 ; ret      (rsp is 16-byte aligned inside the body, as at a System V call site)
Code framed(const Code& body) {
    Code c = {0x48, 0x83, 0xEC, 0x08};
    c.insert(c.end(), body.begin(), body.end());
    c.insert(c.end(), {0x48, 0x83, 0xC4, 0x08, 0xC3});
    return c;
}

// ---- raw host functions ------------------------------------------------------------------------------------------
void raw_weighted_sum(abi::GuestRegs* r) {              // eight integers: 1..6 in registers, 7 and 8 on the stack
    abi::RegsReader a(*r);
    uint64_t s = 0;
    for (uint64_t w = 1; w <= 8; ++w) s += w * a.next_int();
    r->ret_rax = s;
}
void raw_mixed(abi::GuestRegs* r) {                     // int a, double b, int c, double d  ->  a + 2b + 3c + 4d (double)
    abi::RegsReader a(*r);
    const uint64_t ia = a.next_int();
    const double b = a.next_double();
    const uint64_t ic = a.next_int();
    const double d = a.next_double();
    r->ret_xmm0 = bits(static_cast<double>(ia) + 2 * b + 3 * static_cast<double>(ic) + 4 * d);
}
void raw_ten_doubles(abi::GuestRegs* r) {               // ten doubles: eight in xmm0-7, two on the stack; weight i+1
    abi::RegsReader a(*r);
    double s = 0;
    for (int i = 0; i < 10; ++i) s += (i + 1) * a.next_double();
    r->ret_xmm0 = bits(s);
}
void raw_interleaved_stack(abi::GuestRegs* r) {         // 8 ints then 10 doubles: int slots and double slots share the stack
    abi::RegsReader a(*r);
    uint64_t si = 0;
    for (int i = 0; i < 8; ++i) si = si * 10 + a.next_int();     // digits
    double sd = 0;
    for (int i = 0; i < 10; ++i) sd += a.next_double();
    r->ret_rax = si;
    r->ret_xmm0 = bits(sd);
}
void raw_al(abi::GuestRegs* r) { r->ret_rax = r->rax & 0xff; }          // number of vector registers a variadic caller announced
void raw_aggregate(abi::GuestRegs* r) {                 // struct { long a; long b; } in rax:rdx, struct { double; double; } in xmm0:xmm1
    abi::RegsReader a(*r);
    const uint64_t x = a.next_int();
    r->ret_rax = x + 1;
    r->ret_rdx = x + 2;
    r->ret_xmm0 = bits(1.25);
    r->ret_xmm1 = bits(2.5);
}
void raw_alignment(abi::GuestRegs* r) {                 // System V: (stack_args) is 16-byte aligned at a call site; the record is host memory
    r->ret_rax = (r->stack_args % 16 == 0 ? 1u : 0u) | (reinterpret_cast<uintptr_t>(r) % 8 == 0 ? 2u : 0u);
}
void raw_exit(abi::GuestRegs*) { abi::request_guest_exit(66); }
void raw_clobber(abi::GuestRegs* r) {                   // do real work with the host stack and registers
    volatile uint64_t big[256];
    for (int i = 0; i < 256; ++i) big[i] = static_cast<uint64_t>(i) * 0x9e3779b97f4a7c15ull;
    uint64_t x = 0;
    for (int i = 0; i < 256; ++i) x ^= big[i];
    r->ret_rax = x;
}
int64_t positional_int_double(int a, double b) { return static_cast<int64_t>(a + b * 10); }

uint64_t raw_addr(const abi::ThunkTable& t, size_t i) { return t.address(i); }

abi::ThunkTable raw_table(std::initializer_list<abi::RawHostFn> fns) {
    std::vector<abi::ThunkSpec> specs;
    for (auto f : fns) specs.push_back({reinterpret_cast<void*>(f), true});
    return take(abi::ThunkTable::build_specs(specs));
}

}  // namespace

ORVYN_TEST(raw_thunk_receives_six_register_and_two_stack_integers) {
    auto t = raw_table({raw_weighted_sum});
    Code body;
    push_u64(body, 8);                                   // pushed first: highest address
    push_u64(body, 7);                                   // first stack argument: lowest address
    mov_rdi(body, 1); mov_rsi(body, 2); mov_rdx(body, 3); mov_rcx(body, 4); mov_r8(body, 5); mov_r9(body, 6);
    call_r11(body, raw_addr(t, 0));
    add_rsp(body, 16);
    uint64_t expect = 0;
    for (uint64_t i = 1; i <= 8; ++i) expect += i * i;
    CHECK_EQ(run_code(framed(body)).value, expect);       // 204
}

ORVYN_TEST(raw_thunk_handles_mixed_integer_and_double_arguments) {
    auto t = raw_table({raw_mixed});
    Code body;
    movq_xmm(body, 0, 2.5);                              // b
    movq_xmm(body, 1, 4.5);                              // d
    mov_rdi(body, 10);                                   // a
    mov_rsi(body, 20);                                   // c
    call_r11(body, raw_addr(t, 0));
    ret_xmm0(body);
    // 10 + 2*2.5 + 3*20 + 4*4.5 = 93
    CHECK_EQ(from_bits(run_code(framed(body)).value), 93.0);
}

ORVYN_TEST(a_positional_thunk_gets_mixed_signatures_wrong_which_is_why_raw_thunks_exist) {
    // Windows x64 positions the double of f(int, double) in xmm1; System V puts it in xmm0. The positional thunk cannot know.
    auto t = take(abi::ThunkTable::build({reinterpret_cast<void*>(&positional_int_double)}));
    Code body;
    movq_xmm(body, 0, 2.5);
    movq_xmm(body, 1, 0.0);
    mov_rdi(body, 7);
    call_r11(body, t.address(0));
    const int64_t wrong = static_cast<int64_t>(run_code(framed(body)).value);
    CHECK(wrong != 7 + 25);                               // the correct answer is 32; the positional bridge returns 7
    CHECK_EQ(wrong, int64_t{7});
}

ORVYN_TEST(ten_doubles_use_eight_xmm_registers_and_two_stack_slots) {
    auto t = raw_table({raw_ten_doubles});
    Code body;
    push_u64(body, bits(10.0));
    push_u64(body, bits(9.0));
    for (int i = 0; i < 8; ++i) movq_xmm(body, i, i + 1.0);
    call_r11(body, raw_addr(t, 0));
    add_rsp(body, 16);
    ret_xmm0(body);
    double expect = 0;
    for (int i = 1; i <= 10; ++i) expect += i * static_cast<double>(i);   // 385
    CHECK_EQ(from_bits(run_code(framed(body)).value), expect);
}

ORVYN_TEST(integer_and_double_stack_arguments_keep_call_order) {
    // 8 ints (6 regs + 2 stack) and 10 doubles (8 regs + 2 stack): the four stack slots hold int7 int8 dbl9 dbl10 in call order
    auto t = raw_table({raw_interleaved_stack});
    Code body;
    push_u64(body, bits(10.0));
    push_u64(body, bits(9.0));
    push_u64(body, 8);
    push_u64(body, 7);
    mov_rdi(body, 1); mov_rsi(body, 2); mov_rdx(body, 3); mov_rcx(body, 4); mov_r8(body, 5); mov_r9(body, 6);
    for (int i = 0; i < 8; ++i) movq_xmm(body, i, i + 1.0);
    // The reader consumes the two integer stack slots first (ints requested first), then the double slots: this is the
    // layout a caller creates when it passes (int x8, double x10) in that textual order, which is what we are modelling.
    call_r11(body, raw_addr(t, 0));
    add_rsp(body, 32);
    // result: integer digits 12345678 ; double sum 55 (reported through rax only for the integer part)
    CHECK_EQ(run_code(framed(body)).value, uint64_t{12345678});
}

ORVYN_TEST(al_announces_the_number_of_vector_registers_to_a_variadic_callee) {
    auto t = raw_table({raw_al});
    for (int n : {0, 1, 3, 8}) {
        Code body;
        call_r11(body, raw_addr(t, 0));
        // `mov eax, n` must come AFTER loading r11 (which does not touch rax) and just before the call: patch it in
        Code c;
        mov_reg(c, {0x49, 0xBB}, raw_addr(t, 0));
        c.insert(c.end(), {0xB8, static_cast<uint8_t>(n), 0, 0, 0});            // mov eax, n
        c.insert(c.end(), {0x41, 0xFF, 0xD3});                                     // call r11
        CHECK_EQ(run_code(framed(c)).value, static_cast<uint64_t>(n));
    }
}

ORVYN_TEST(aggregate_results_come_back_in_rax_rdx_and_xmm0_xmm1) {
    auto t = raw_table({raw_aggregate});
    Code a;
    mov_rdi(a, 100);
    call_r11(a, raw_addr(t, 0));                                                 // rax = 101
    CHECK_EQ(run_code(framed(a)).value, uint64_t{101});
    Code b;
    mov_rdi(b, 100);
    call_r11(b, raw_addr(t, 0));
    b.insert(b.end(), {0x48, 0x89, 0xD0});                                       // mov rax, rdx   (= 102)
    CHECK_EQ(run_code(framed(b)).value, uint64_t{102});
    Code c;
    call_r11(c, raw_addr(t, 0));
    ret_xmm0(c);
    CHECK_EQ(from_bits(run_code(framed(c)).value), 1.25);
    // xmm1: movapd xmm0, xmm1 ; movq rax, xmm0
    Code d;
    call_r11(d, raw_addr(t, 0));
    d.insert(d.end(), {0x66, 0x0F, 0x28, 0xC1});
    ret_xmm0(d);
    CHECK_EQ(from_bits(run_code(framed(d)).value), 2.5);
}

ORVYN_TEST(stack_arguments_start_16_byte_aligned_like_a_system_v_call_site) {
    auto t = raw_table({raw_alignment});
    Code body;
    call_r11(body, raw_addr(t, 0));
    CHECK_EQ(run_code(framed(body)).value, uint64_t{3});
}

ORVYN_TEST(raw_thunk_preserves_callee_saved_registers_across_a_noisy_host_function) {
    auto t = raw_table({raw_clobber});
    const uint64_t mb = 0x1111111111111111ull, m12 = 0x2222222222222222ull, m13 = 0x3333333333333333ull, m14 = 0x4444444444444444ull,
                   m15 = 0x5555555555555555ull, mbp = 0x6666666666666666ull;
    Code body;
    mov_reg(body, {0x48, 0xBB}, mb);          // rbx
    mov_reg(body, {0x49, 0xBC}, m12);         // r12
    mov_reg(body, {0x49, 0xBD}, m13);         // r13
    mov_reg(body, {0x49, 0xBE}, m14);         // r14
    mov_reg(body, {0x49, 0xBF}, m15);         // r15
    mov_reg(body, {0x48, 0xBD}, mbp);         // rbp
    call_r11(body, raw_addr(t, 0));
    body.insert(body.end(), {0x48, 0x89, 0xD8});               // mov rax, rbx
    body.insert(body.end(), {0x4C, 0x31, 0xE0});               // xor rax, r12
    body.insert(body.end(), {0x4C, 0x31, 0xE8});               // xor rax, r13
    body.insert(body.end(), {0x4C, 0x31, 0xF0});               // xor rax, r14
    body.insert(body.end(), {0x4C, 0x31, 0xF8});               // xor rax, r15
    body.insert(body.end(), {0x48, 0x31, 0xE8});               // xor rax, rbp
    CHECK_EQ(run_code(framed(body)).value, mb ^ m12 ^ m13 ^ m14 ^ m15 ^ mbp);
}

ORVYN_TEST(raw_thunk_never_writes_below_the_guest_stack_pointer) {
    // Data in the 128 bytes below rsp (the red zone of a leaf function) survives a call that runs on the host stack.
    auto t = raw_table({raw_clobber});
    const uint64_t m1 = 0xA1A1A1A1A1A1A1A1ull, m2 = 0xB2B2B2B2B2B2B2B2ull, m3 = 0xC3C3C3C3C3C3C3C3ull;
    Code body;
    mov_rax(body, m1); body.insert(body.end(), {0x48, 0x89, 0x44, 0x24, 0xF0});    // mov [rsp-16], rax
    mov_rax(body, m2); body.insert(body.end(), {0x48, 0x89, 0x44, 0x24, 0xC0});    // mov [rsp-64], rax
    mov_rax(body, m3); body.insert(body.end(), {0x48, 0x89, 0x44, 0x24, 0x80});    // mov [rsp-128], rax
    call_r11(body, raw_addr(t, 0));
    body.insert(body.end(), {0x48, 0x8B, 0x44, 0x24, 0xF0});                       // mov rax, [rsp-16]
    body.insert(body.end(), {0x48, 0x8B, 0x4C, 0x24, 0xC0});                       // mov rcx, [rsp-64]
    body.insert(body.end(), {0x48, 0x31, 0xC8});                                   // xor rax, rcx
    body.insert(body.end(), {0x48, 0x8B, 0x4C, 0x24, 0x80});                       // mov rcx, [rsp-128]
    body.insert(body.end(), {0x48, 0x31, 0xC8});                                   // xor rax, rcx
    CHECK_EQ(run_code(framed(body)).value, m1 ^ m2 ^ m3);
}

ORVYN_TEST(raw_functions_can_end_the_guest_with_exit) {
    auto t = raw_table({raw_exit});
    Code body;
    call_r11(body, raw_addr(t, 0));
    body.insert(body.end(), {0xB8, 0x05, 0x00, 0x00, 0x00});                       // mov eax, 5   -- must not run
    auto o = run_code(framed(body));
    CHECK(o.exited);
    CHECK_EQ(o.value, uint64_t{66});
}

ORVYN_TEST(positional_and_raw_thunks_coexist_in_one_table) {
    auto add = +[](int a, int b) -> int { return a + b; };
    std::vector<abi::ThunkSpec> specs = {{reinterpret_cast<void*>(add), false}, {reinterpret_cast<void*>(&raw_weighted_sum), true}};
    auto t = take(abi::ThunkTable::build_specs(specs));
    Code body;
    mov_rdi(body, 20); mov_rsi(body, 22);
    call_r11(body, t.address(0));
    CHECK_EQ(static_cast<uint32_t>(run_code(framed(body)).value), 42u);
    CHECK(t.address(1) - t.address(0) == abi::ThunkTable::kStride);
}

// ---- readers -------------------------------------------------------------------------------------------------------------------
ORVYN_TEST(regs_reader_walks_registers_then_stack_in_order) {
    abi::GuestRegs r;
    for (int i = 0; i < 6; ++i) r.gp[i] = 100 + i;
    for (int i = 0; i < 8; ++i) r.xmm[i] = bits(0.5 + i);
    uint64_t stack[4] = {1000, bits(2000.5), 3000, bits(4000.5)};
    r.stack_args = reinterpret_cast<uint64_t>(stack);
    abi::RegsReader a(r);
    for (int i = 0; i < 6; ++i) CHECK_EQ(a.next_int(), uint64_t{100} + i);
    CHECK_EQ(a.next_int(), uint64_t{1000});           // 7th integer: first stack slot
    for (int i = 0; i < 8; ++i) CHECK_EQ(a.next_double(), 0.5 + i);
    CHECK_EQ(a.next_double(), 2000.5);                // 9th double: next stack slot
    CHECK_EQ(a.next_int(), uint64_t{3000});
    CHECK_EQ(a.next_double(), 4000.5);
}

ORVYN_TEST(va_list_reader_follows_the_system_v_register_save_area_layout) {
    // reg_save_area: 6 integer registers (48 bytes) then 8 xmm registers (16 bytes each, up to offset 176)
    alignas(16) uint8_t save[176] = {};
    for (int i = 0; i < 6; ++i) { uint64_t v = 10 + i; std::memcpy(save + 8 * i, &v, 8); }
    for (int i = 0; i < 8; ++i) { uint64_t v = bits(0.25 * (i + 1)); std::memcpy(save + 48 + 16 * i, &v, 8); }
    uint64_t overflow[3] = {777, bits(8.5), 999};
    abi::VaListReader::VaList va{8, 64, reinterpret_cast<uint64_t>(overflow), reinterpret_cast<uint64_t>(save)};   // 1 int and 1 double already consumed
    abi::VaListReader r(reinterpret_cast<uint64_t>(&va));
    CHECK_EQ(r.next_int(), uint64_t{11});             // gp_offset 8 -> second register
    CHECK_EQ(va.gp_offset, 16u);                      // va_arg advanced the structure
    CHECK_EQ(r.next_double(), 0.5);                   // fp_offset 64 -> xmm3? (48 + 16)=xmm1 -> 0.5
    CHECK_EQ(va.fp_offset, 80u);
    for (int i = 0; i < 4; ++i) CHECK_EQ(r.next_int(), uint64_t{12} + i);      // registers 2..5
    CHECK_EQ(va.gp_offset, 48u);
    CHECK_EQ(r.next_int(), uint64_t{777});            // registers exhausted: overflow area
    CHECK_EQ(va.overflow_arg_area, reinterpret_cast<uint64_t>(overflow + 1));
    CHECK_EQ(r.next_double(), 0.75);                  // xmm still has registers (offset 80 -> index 2)
    while (va.fp_offset < 176) r.next_double();       // drain the vector registers
    CHECK_EQ(r.next_double(), 8.5);                   // then the same overflow area
    CHECK_EQ(r.next_int(), uint64_t{999});
}
