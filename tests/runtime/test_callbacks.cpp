// Host -> guest callbacks (ORVYN 0.4.0). ABI-level tests use hand-assembled System V code and test host functions, so the
// bridge is verified without any compiler; the libSystem-level tests (qsort, bsearch, atexit) run real Mach-O fixtures whose
// comparators and handlers are machine code of the guest program.
#include <cmath>
#include <cstring>
#include <functional>

#include "orvyn/abi.hpp"
#include "orvyn/guest.hpp"
#include "guest_code.hpp"
#include "test_framework.hpp"

using namespace orvyn;
using namespace orvyn::testing;

namespace {

// ---- callbacks at ABI level -----------------------------------------------------------------------------------------
std::vector<std::pair<uint64_t, uint64_t>> g_ranges;
bool valid(uint64_t fn) {
    for (auto& r : g_ranges)
        if (fn >= r.first && fn < r.second) return true;
    return false;
}
struct Validator {
    Validator() { g_ranges.clear(); abi::set_callback_validator(&valid); }
    ~Validator() { abi::set_callback_validator(nullptr); g_ranges.clear(); }
    void add(const CodePage& p) { g_ranges.emplace_back(p.address(), p.address() + p.size()); }
};

int g_refused = 0;
// int host_apply(fn, a, b): calls the guest function fn(a, b) and returns its integer result (-1 when refused)
int64_t host_apply(uint64_t fn, uint64_t a, uint64_t b) {
    abi::GuestCallArgs args;
    args.push_int(a).push_int(b);
    auto r = abi::call_guest_nested(fn, args);
    if (r.refused) { ++g_refused; return -1; }
    return static_cast<int32_t>(r.value);
}
// double host_apply_d(fn, double x, double y): fn(double, double) -> double ; raw because it mixes classes
void raw_apply_double(abi::GuestRegs* r) {
    abi::RegsReader a(*r);
    const uint64_t fn = a.next_int();
    const double x = a.next_double(), y = a.next_double();
    abi::GuestCallArgs args;
    args.push_double(x).push_double(y);
    auto o = abi::call_guest_nested(fn, args);
    r->ret_xmm0 = o.xmm0;
}
// mixed(fn): calls fn(int 5, double 2.5, int 7) -> double
void raw_apply_mixed(abi::GuestRegs* r) {
    abi::RegsReader a(*r);
    const uint64_t fn = a.next_int();
    abi::GuestCallArgs args;
    args.push_int(5).push_double(2.5).push_int(7);
    auto o = abi::call_guest_nested(fn, args);
    r->ret_xmm0 = o.xmm0;
}
// stack(fn): fn(1..8) -> the callee returns arg7 + arg8
void raw_apply_stack(abi::GuestRegs* r) {
    abi::RegsReader a(*r);
    const uint64_t fn = a.next_int();
    abi::GuestCallArgs args;
    for (uint64_t i = 1; i <= 8; ++i) args.push_int(i * 10);
    auto o = abi::call_guest_nested(fn, args);
    r->ret_rax = o.value;
}
// pair(fn): fn() returns a struct {long, long} in rax:rdx
void raw_apply_pair(abi::GuestRegs* r) {
    abi::RegsReader a(*r);
    const uint64_t fn = a.next_int();
    auto o = abi::call_guest_nested(fn, abi::GuestCallArgs{});
    r->ret_rax = o.value + o.rdx;
}
void raw_exit(abi::GuestRegs*) { abi::request_guest_exit(66); }
int64_t host_depth() { return abi::callback_depth(); }

using Code = std::vector<uint8_t>;
void mov_rax(Code& c, uint64_t v) { c.insert(c.end(), {0x48, 0xB8}); put_u64(c, v); }
void mov_rdi(Code& c, uint64_t v) { c.insert(c.end(), {0x48, 0xBF}); put_u64(c, v); }
void mov_rsi(Code& c, uint64_t v) { c.insert(c.end(), {0x48, 0xBE}); put_u64(c, v); }
void mov_rdx(Code& c, uint64_t v) { c.insert(c.end(), {0x48, 0xBA}); put_u64(c, v); }
void call_rax(Code& c) { c.insert(c.end(), {0xFF, 0xD0}); }
Code framed(const Code& body) {
    Code c = {0x48, 0x83, 0xEC, 0x08};
    c.insert(c.end(), body.begin(), body.end());
    c.insert(c.end(), {0x48, 0x83, 0xC4, 0x08, 0xC3});
    return c;
}
double from_bits(uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }
uint64_t bits(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }

struct Fixture {
    abi::ThunkTable table;
    Fixture() {
        std::vector<abi::ThunkSpec> specs = {{reinterpret_cast<void*>(&host_apply), false},
                                             {reinterpret_cast<void*>(&raw_apply_double), true},
                                             {reinterpret_cast<void*>(&raw_apply_mixed), true},
                                             {reinterpret_cast<void*>(&raw_apply_stack), true},
                                             {reinterpret_cast<void*>(&raw_apply_pair), true},
                                             {reinterpret_cast<void*>(&raw_exit), true},
                                             {reinterpret_cast<void*>(&host_depth), false}};
        table = take(abi::ThunkTable::build_specs(specs));
    }
    uint64_t apply() const { return table.address(0); }
    uint64_t apply_d() const { return table.address(1); }
    uint64_t apply_mixed() const { return table.address(2); }
    uint64_t apply_stack() const { return table.address(3); }
    uint64_t apply_pair() const { return table.address(4); }
    uint64_t do_exit() const { return table.address(5); }
    uint64_t depth() const { return table.address(6); }
};

// guest function bodies
const Code kAdd = {0x8D, 0x04, 0x37, 0xC3};                                  // lea eax,[rdi+rsi] ; ret
const Code kMul = {0x89, 0xF8, 0x0F, 0xAF, 0xC6, 0xC3};                      // mov eax,edi ; imul eax,esi ; ret
const Code kAddD = {0xF2, 0x0F, 0x58, 0xC1, 0xC3};                           // addsd xmm0,xmm1 ; ret
const Code kMixed = {0xF2, 0x0F, 0x2A, 0xD7,                                 // cvtsi2sd xmm2, edi   (a = 5)
                     0xF2, 0x0F, 0x58, 0xC2,                                 // addsd xmm0, xmm2     (b + a)
                     0xF2, 0x0F, 0x2A, 0xD6,                                 // cvtsi2sd xmm2, esi   (c = 7)
                     0xF2, 0x0F, 0x58, 0xC2, 0xC3};                          // addsd xmm0, xmm2 ; ret      -> 2.5 + 5 + 7
const Code kStackSum = {0x48, 0x8B, 0x44, 0x24, 0x08,                        // mov rax,[rsp+8]   (7th integer argument)
                        0x48, 0x03, 0x44, 0x24, 0x10, 0xC3};                 // add rax,[rsp+16]  (8th) ; ret
const Code kPair = {0xB8, 0x14, 0, 0, 0, 0xBA, 0x16, 0, 0, 0, 0xC3};         // mov eax,20 ; mov edx,22 ; ret

}  // namespace

ORVYN_TEST(callback_returns_an_integer_computed_by_guest_code) {
    Fixture f;
    CodePage add(kAdd);
    Validator v;
    v.add(add);
    Code body;
    mov_rdi(body, add.address()); mov_rsi(body, 20); mov_rdx(body, 22);
    mov_rax(body, f.apply()); call_rax(body);
    CHECK_EQ(static_cast<int32_t>(run_code(framed(body)).value), 42);
    // different guest code => different result (the value is not computed by the host)
    CodePage mul(kMul);
    v.add(mul);
    body.clear();
    mov_rdi(body, mul.address()); mov_rsi(body, 6); mov_rdx(body, 7);
    mov_rax(body, f.apply()); call_rax(body);
    CHECK_EQ(static_cast<int32_t>(run_code(framed(body)).value), 42);
}

ORVYN_TEST(callback_with_double_arguments_and_result) {
    Fixture f;
    CodePage add(kAddD);
    Validator v;
    v.add(add);
    Code body;
    mov_rdi(body, add.address());
    body.insert(body.end(), {0x48, 0xB8}); put_u64(body, bits(1.25));
    body.insert(body.end(), {0x66, 0x48, 0x0F, 0x6E, 0xC0});                    // movq xmm0, rax
    body.insert(body.end(), {0x48, 0xB8}); put_u64(body, bits(40.5));
    body.insert(body.end(), {0x66, 0x48, 0x0F, 0x6E, 0xC8});                    // movq xmm1, rax
    mov_rax(body, f.apply_d()); call_rax(body);
    body.insert(body.end(), {0x66, 0x48, 0x0F, 0x7E, 0xC0});                    // movq rax, xmm0
    CHECK_EQ(from_bits(run_code(framed(body)).value), 41.75);
}

ORVYN_TEST(callback_with_mixed_integer_and_double_arguments) {
    Fixture f;
    CodePage cb(kMixed);
    Validator v;
    v.add(cb);
    Code body;
    mov_rdi(body, cb.address());
    mov_rax(body, f.apply_mixed()); call_rax(body);
    body.insert(body.end(), {0x66, 0x48, 0x0F, 0x7E, 0xC0});
    CHECK_EQ(from_bits(run_code(framed(body)).value), 14.5);                   // 2.5 + 5 + 7
}

ORVYN_TEST(callback_with_eight_integer_arguments_uses_the_guest_stack) {
    Fixture f;
    CodePage cb(kStackSum);
    Validator v;
    v.add(cb);
    Code body;
    mov_rdi(body, cb.address());
    mov_rax(body, f.apply_stack()); call_rax(body);
    CHECK_EQ(run_code(framed(body)).value, uint64_t{70 + 80});                  // the 7th and 8th arguments
}

ORVYN_TEST(callback_returns_a_two_register_aggregate) {
    Fixture f;
    CodePage cb(kPair);
    Validator v;
    v.add(cb);
    Code body;
    mov_rdi(body, cb.address());
    mov_rax(body, f.apply_pair()); call_rax(body);
    CHECK_EQ(run_code(framed(body)).value, uint64_t{42});                      // rax (20) + rdx (22)
}

ORVYN_TEST(nested_callbacks_guest_to_host_to_guest_to_host_to_guest) {
    Fixture f;
    CodePage add(kAdd);
    // outer(a, b) = host_apply(add, a, b) + 1     -- the outer function is itself a callback target
    Code outer;
    outer.insert(outer.end(), {0x48, 0x83, 0xEC, 0x08});                         // sub rsp,8
    outer.insert(outer.end(), {0x48, 0x89, 0xF2});                               // mov rdx, rsi
    outer.insert(outer.end(), {0x48, 0x89, 0xFE});                               // mov rsi, rdi
    mov_rdi(outer, add.address());
    mov_rax(outer, f.apply()); call_rax(outer);
    outer.insert(outer.end(), {0x83, 0xC0, 0x01});                               // add eax,1
    outer.insert(outer.end(), {0x48, 0x83, 0xC4, 0x08, 0xC3});
    CodePage outer_page(outer);
    Validator v;
    v.add(add);
    v.add(outer_page);
    Code body;
    mov_rdi(body, outer_page.address()); mov_rsi(body, 20); mov_rdx(body, 21);
    mov_rax(body, f.apply()); call_rax(body);
    CHECK_EQ(static_cast<int32_t>(run_code(framed(body)).value), 42);           // 20 + 21 + 1
}

ORVYN_TEST(callback_depth_is_limited_and_the_guest_survives_the_refusal) {
    Fixture f;
    // rec(x): if x == 0 return 0 ; return host_apply(rec, x-1, 0) + 1.  It finds its own address with a RIP-relative lea.
    Code rec = {0x85, 0xFF,                                  // 0:  test edi, edi
                0x74, 0x25,                                  // 2:  jz ret0 (offset 41)
                0x48, 0x83, 0xEC, 0x08,                      // 4:  sub rsp, 8
                0x89, 0xFE, 0xFF, 0xCE,                      // 8:  mov esi, edi ; dec esi
                0x31, 0xD2,                                  // 12: xor edx, edx
                0x48, 0x8D, 0x3D, 0xEB, 0xFF, 0xFF, 0xFF};   // 14: lea rdi, [rip - 21]  -> the function itself
    mov_rax(rec, f.apply());                                 // 21
    call_rax(rec);                                           // 31
    rec.insert(rec.end(), {0x83, 0xC0, 0x01});               // 33: add eax, 1
    rec.insert(rec.end(), {0x48, 0x83, 0xC4, 0x08, 0xC3});   // 36: add rsp, 8 ; ret
    rec.insert(rec.end(), {0x31, 0xC0, 0xC3});               // 41: ret0: xor eax, eax ; ret
    CodePage page(rec);
    Validator v;
    v.add(page);
    g_refused = 0;
    // call rec(1000) directly from the guest entry (a top-level call)
    auto stack = make_stack(4 << 20);
    auto o = abi::call_guest(page.address(), stack, stack.top() - 64, 1000);
    CHECK(!o.exited);
    CHECK(g_refused >= 1);                                                        // the depth limit refused the 65th nested callback
    CHECK(static_cast<int32_t>(o.value) >= 60 && static_cast<int32_t>(o.value) <= static_cast<int32_t>(abi::kMaxCallbackDepth) + 1);
    CHECK_EQ(abi::callback_depth(), 0u);                                          // the counter unwound completely
}

ORVYN_TEST(invalid_callback_targets_are_refused_and_nothing_runs) {
    Fixture f;
    CodePage add(kAdd);
    Validator v;
    v.add(add);
    g_refused = 0;
    for (uint64_t bad : {uint64_t{0}, uint64_t{0x1234}, add.address() + add.size(), reinterpret_cast<uint64_t>(&g_refused)}) {
        Code body;
        mov_rdi(body, bad); mov_rsi(body, 1); mov_rdx(body, 2);
        mov_rax(body, f.apply()); call_rax(body);
        CHECK_EQ(static_cast<int32_t>(run_code(framed(body)).value), -1);
    }
    CHECK_EQ(g_refused, 4);
    // outside any guest call there is nothing to call back into
    abi::GuestCallArgs none;
    auto r = abi::call_guest_nested(add.address(), none);
    CHECK(r.refused);
}

ORVYN_TEST(exit_inside_a_nested_callback_unwinds_every_host_frame) {
    Fixture f;
    // callee: calls the raw exit function. outer: host_apply(callee) ; mov eax, 5 (must not run)
    Code callee;
    callee.insert(callee.end(), {0x48, 0x83, 0xEC, 0x08});
    mov_rax(callee, f.do_exit()); call_rax(callee);
    callee.insert(callee.end(), {0x48, 0x83, 0xC4, 0x08, 0xC3});
    CodePage cb(callee);
    Validator v;
    v.add(cb);
    Code body;
    mov_rdi(body, cb.address());
    mov_rax(body, f.apply()); call_rax(body);
    body.insert(body.end(), {0xB8, 0x05, 0, 0, 0});                              // mov eax,5 : must not run
    auto o = run_code(framed(body));
    CHECK(o.exited);
    CHECK_EQ(o.value, uint64_t{66});
    CHECK_EQ(abi::callback_depth(), 0u);
}

ORVYN_TEST(the_interrupted_frame_red_zone_survives_a_callback_that_uses_a_lot_of_stack) {
    Fixture f;
    // callee: sub rsp,4096 ; write 0xEE over the whole area ; add rsp,4096 ; xor eax,eax ; ret
    Code callee = {0x48, 0x81, 0xEC, 0x00, 0x10, 0x00, 0x00,                     // sub rsp,4096
                   0x48, 0x89, 0xE7,                                              // mov rdi,rsp
                   0xB9, 0x00, 0x02, 0x00, 0x00,                                  // mov ecx,512
                   0x48, 0xB8, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE,   // mov rax,0xEEEE...
                   0xF3, 0x48, 0xAB,                                              // rep stosq
                   0x48, 0x81, 0xC4, 0x00, 0x10, 0x00, 0x00,                     // add rsp,4096
                   0x31, 0xC0, 0xC3};
    CodePage cb(callee);
    Validator v;
    v.add(cb);
    const uint64_t m = 0x1122334455667788ull;
    Code body;
    mov_rax(body, m); body.insert(body.end(), {0x48, 0x89, 0x44, 0x24, 0x90});    // mov [rsp-112], rax   (inside the red zone)
    mov_rdi(body, cb.address());
    mov_rax(body, f.apply()); call_rax(body);
    body.insert(body.end(), {0x48, 0x8B, 0x44, 0x24, 0x90});                      // mov rax,[rsp-112]
    CHECK_EQ(run_code(framed(body)).value, m);
}

// ---- libSystem level: real Mach-O fixtures ----------------------------------------------------------------------------------
namespace {
struct Run {
    guest::Outcome r;
    std::string out;
};
Run run_fixture(const std::string& name, std::vector<std::string> args = {}) {
    Run run;
    guest::GuestConfig cfg;
    cfg.args = {name};
    cfg.args.insert(cfg.args.end(), args.begin(), args.end());
    cfg.sink = [&](int, const char* d, size_t n) { run.out.append(d, n); };
    auto bytes = exec_fixture(name);
    run.r = guest::run_in_process(ByteView(bytes), cfg);
    return run;
}
}  // namespace

ORVYN_TEST(qsort_calls_a_mach_o_comparator_and_sorts_the_array) {
    Run r = run_fixture("qsort_ints");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 42);               // 42 only if the comparator ran (calls > 0) and the array is sorted
    CHECK_EQ(r.out, std::string("0 1 2 3 4 5 6 7 8 9 \ncomparisons>0: 1\n"));
}

ORVYN_TEST(qsort_of_strings_records_and_bsearch) {
    Run r = run_fixture("qsort_records");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 42);
    CHECK_EQ(r.out, std::string("apple banana cherry fig pear \n1:1 1:4 2:3 3:0 3:2 3:5 \n"));   // strcmp from libSystem; stable records
}

ORVYN_TEST(a_comparator_may_call_qsort_itself) {
    Run r = run_fixture("qsort_nested");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 42);
}

ORVYN_TEST(atexit_handlers_run_in_reverse_order_after_main) {
    Run r = run_fixture("atexit_order");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 5);                                                   // main's value is kept
    CHECK_EQ(r.out, std::string("main\nhandler 3\nhandler 1\nhandler 2\nhandler 1\n"));
}

ORVYN_TEST(exit_in_an_atexit_handler_replaces_the_exit_code) {
    Run r = run_fixture("atexit_exit");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 9);
    CHECK_EQ(r.out, std::string("quitter\n"));                                    // "first" never runs
}

ORVYN_TEST(underscore_exit_skips_atexit_handlers) {
    Run r = run_fixture("atexit_skipped");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 3);
    CHECK_EQ(r.out, std::string("main\n"));
}

ORVYN_TEST(exit_in_a_qsort_comparator_stops_the_sort_and_runs_atexit) {
    Run r = run_fixture("qsort_exit");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 11);
    CHECK_EQ(r.out, std::string("comparing\nbye\n"));
}

ORVYN_TEST(qsort_with_an_invalid_comparator_traps_the_guest) {
    for (const char* arg : {"n", "d", "l", "h"}) {
        Run r = run_fixture("qsort_bad", {arg});
        if (r.r.state != guest::State::Faulted) { report_failure(__FILE__, __LINE__, std::string("not faulted for ") + arg); continue; }
        CHECK(r.r.message.find("not executable guest code") != std::string::npos);
        CHECK_EQ(r.out, std::string("before\n"));
    }
}
