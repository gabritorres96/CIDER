// Entry protocol: LC_MAIN, the guest stack and the argv/envp/apple block (main(argc, argv, envp, apple)).
#include <fstream>
#include <sstream>

#include "orvyn/guest.hpp"
#include "guest_code.hpp"
#include "test_framework.hpp"

using namespace orvyn;
using namespace orvyn::testing;

namespace {

// value of "_main" in the golden `llvm-objdump --syms` output
uint64_t golden_main(const std::string& fixture) {
    std::ifstream f(kExecFixtures / "golden" / (fixture + ".syms.txt"));
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::vector<std::string> tok;
        for (std::string w; ls >> w;) tok.push_back(w);
        if (tok.size() >= 4 && tok.back() == "_main") return std::stoull(tok[0], nullptr, 16);
    }
    return 0;
}

guest::Outcome run_fixture(const std::string& name, std::vector<std::string> args, std::string* out = nullptr, std::vector<std::string> env = {}) {
    auto bytes = exec_fixture(name);
    guest::GuestConfig cfg;
    cfg.args = std::move(args);
    cfg.env = std::move(env);
    std::string captured;
    cfg.sink = [&](int, const char* d, size_t n) { captured.append(d, n); };
    auto r = guest::run_in_process(ByteView(bytes), cfg);
    if (out) *out = captured;
    return r;
}

const uint8_t* at(const abi::GuestStack& st, uint64_t addr) {
    CHECK(addr >= reinterpret_cast<uint64_t>(st.base()) && addr < st.top());
    return reinterpret_cast<const uint8_t*>(addr);
}

}  // namespace

// ---- LC_MAIN contract ---------------------------------------------------------------------------------------
ORVYN_TEST(lc_main_entry_is_the_address_of_main_in_every_fixture) {
    for (const char* n : {"ret42", "math", "stack", "calls", "globals", "bss", "libcall", "hello", "args", "missing", "exitcall", "crash", "spin"}) {
        auto bytes = exec_fixture(n);
        auto file = macho::parse_file(ByteView(bytes)).value();
        const auto& e = *file.slices[0].image->entry;
        CHECK(e.kind == macho::EntryPoint::Kind::Main);
        uint64_t main_addr = golden_main(n);
        REQUIRE(main_addr != 0);
        CHECK_EQ(e.vmaddr, main_addr);                       // LC_MAIN entryoff + __TEXT vmaddr == _main
        CHECK_EQ(e.stack_size, uint64_t{0});                 // linker default: the runtime picks 8 MiB
        CHECK_EQ(e.segment, std::string("__TEXT"));
    }
}

ORVYN_TEST(entry_contract_is_main_called_with_argc_argv_envp_apple_and_its_value_goes_to_exit) {
    // Documented contract (docs/EXECUTION.md): the guest function at LC_MAIN is main(); when it returns,
    // its eax is passed to exit(), exactly like dyld does. ret42 shows the returned value becomes the exit code
    // with returned_from_main == true, exitcall shows an explicit exit() with returned_from_main == false.
    auto a = run_fixture("ret42", {"ret42"});
    CHECK(a.state == guest::State::Exited);
    CHECK_EQ(a.exit_code, 42);
    CHECK(a.returned_from_main);
    auto b = run_fixture("exitcall", {"exitcall"});
    CHECK(b.state == guest::State::Exited);
    CHECK_EQ(b.exit_code, 37);
    CHECK(!b.returned_from_main);
}

// ---- stack builder ------------------------------------------------------------------------------------------
ORVYN_TEST(guest_args_block_layout_and_alignment) {
    auto st = make_stack();
    auto g = abi::build_guest_args(st, {"prog", "a b", "caf\xC3\xA9"}, {"A=1", "B=2"}, {"executable_path=prog"});
    REQUIRE(g.ok());
    const auto& a = g.value();
    CHECK_EQ(a.argc, uint64_t{3});
    CHECK_EQ(a.initial_sp % 16, uint64_t{0});
    CHECK_EQ(a.argv % 8, uint64_t{0});
    CHECK(a.initial_sp <= a.argv && a.initial_sp > reinterpret_cast<uint64_t>(st.base()) + 4096 + 4096);   // above the guard page with room to grow down

    auto strv = [&](uint64_t arr, size_t i) { return std::string(reinterpret_cast<const char*>(at(st, reinterpret_cast<const uint64_t*>(arr)[i]))); };
    CHECK_EQ(strv(a.argv, 0), std::string("prog"));
    CHECK_EQ(strv(a.argv, 1), std::string("a b"));
    CHECK_EQ(strv(a.argv, 2), std::string("caf\xC3\xA9"));
    CHECK_EQ(reinterpret_cast<const uint64_t*>(a.argv)[3], uint64_t{0});                   // argv[argc] == NULL
    CHECK_EQ(strv(a.envp, 0), std::string("A=1"));
    CHECK_EQ(strv(a.envp, 1), std::string("B=2"));
    CHECK_EQ(reinterpret_cast<const uint64_t*>(a.envp)[2], uint64_t{0});
    CHECK_EQ(strv(a.apple, 0), std::string("executable_path=prog"));
    CHECK_EQ(reinterpret_cast<const uint64_t*>(a.apple)[1], uint64_t{0});
    // arrays are consecutive: argv, then envp, then apple (kernel layout)
    CHECK_EQ(a.envp, a.argv + 4 * 8);
    CHECK_EQ(a.apple, a.envp + 3 * 8);
}

ORVYN_TEST(guest_args_accept_empty_vectors) {
    auto st = make_stack();
    auto g = abi::build_guest_args(st, {}, {}, {});
    REQUIRE(g.ok());
    CHECK_EQ(g.value().argc, uint64_t{0});
    CHECK_EQ(*reinterpret_cast<const uint64_t*>(g.value().argv), uint64_t{0});
    CHECK_EQ(g.value().initial_sp % 16, uint64_t{0});
}

ORVYN_TEST(guest_args_that_do_not_fit_are_rejected) {
    auto st = make_stack(64 * 1024);
    std::string huge(100 * 1024, 'x');
    CHECK_ERR(abi::build_guest_args(st, {"p", huge}, {}, {}), ErrorCode::LimitExceeded);
    std::vector<std::string> many(20000, "argument");
    CHECK_ERR(abi::build_guest_args(st, many, {}, {}), ErrorCode::LimitExceeded);
    // a failed attempt never writes below the guard page or past the top: the stack is still usable
    CHECK(abi::build_guest_args(st, {"ok"}, {}, {}).ok());
}

ORVYN_TEST(guest_function_can_use_stack_space_below_the_args_block) {
    auto st = make_stack();
    auto g = abi::build_guest_args(st, {"prog"}, {}, {}).value();
    // push/pop 200 values: mov ecx,200 ; L: push rcx ; loop L ; add rsp,1600 ; mov eax, 7 ; ret
    CodePage page({0xB9, 0xC8, 0x00, 0x00, 0x00, 0x51, 0xE2, 0xFD, 0x48, 0x81, 0xC4, 0x40, 0x06, 0x00, 0x00, 0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3});
    auto o = abi::call_guest(page.address(), st, g.initial_sp, g.argc, g.argv, g.envp, g.apple);
    CHECK_EQ(static_cast<uint32_t>(o.value), 7u);
}

ORVYN_TEST(guest_receives_argc_argv_envp_apple_in_the_system_v_registers) {
    auto st = make_stack();
    auto g = abi::build_guest_args(st, {"p", "q"}, {"E=1"}, {"A=2"}).value();
    // return argc*1000 + (argv!=0) + (envp!=0)*10 + (apple!=0)*100  using rdi,rsi,rdx,rcx
    CodePage page({0x48, 0x69, 0xC7, 0xE8, 0x03, 0x00, 0x00,       // imul rax, rdi, 1000
                   0x48, 0x85, 0xF6, 0x0F, 0x95, 0xC3, 0x0F, 0xB6, 0xDB, 0x48, 0x01, 0xD8,           // test rsi,rsi ; setne bl ; movzx ebx,bl ; add rax,rbx
                   0x48, 0x85, 0xD2, 0x0F, 0x95, 0xC3, 0x0F, 0xB6, 0xDB, 0x48, 0x6B, 0xDB, 0x0A, 0x48, 0x01, 0xD8,   // envp * 10
                   0x48, 0x85, 0xC9, 0x0F, 0x95, 0xC3, 0x0F, 0xB6, 0xDB, 0x48, 0x6B, 0xDB, 0x64, 0x48, 0x01, 0xD8,   // apple * 100
                   0xC3});
    auto o = abi::call_guest(page.address(), st, g.initial_sp, g.argc, g.argv, g.envp, g.apple);
    CHECK_EQ(o.value, uint64_t{2111});
}

// ---- real Mach-O programs using the protocol -----------------------------------------------------------------------------------
ORVYN_TEST(fixture_args_sees_the_command_line) {
    std::string out;
    auto r = run_fixture("args", {"prog", "first", "second"}, &out);
    CHECK(r.state == guest::State::Exited);
    CHECK_EQ(out, std::string("first\n"));                  // puts(argv[1]) from the Mach-O code
    CHECK_EQ(r.exit_code, 31);                              // argc*10 + ok  (3 args, argv[argc]==NULL, envp, apple non-null)
    out.clear();
    r = run_fixture("args", {"prog"}, &out);
    CHECK_EQ(out, std::string(""));
    CHECK_EQ(r.exit_code, 11);
    r = run_fixture("args", {"a", "b", "c", "d", "e"}, &out);
    CHECK_EQ(r.exit_code, 51);
    CHECK_EQ(out, std::string("b\n"));
}

ORVYN_TEST(unicode_and_spaces_in_arguments_reach_the_guest_unchanged) {
    std::string out;
    run_fixture("args", {"prog", "h\xC3\xA9llo w\xC3\xB6rld"}, &out);
    CHECK_EQ(out, std::string("h\xC3\xA9llo w\xC3\xB6rld\n"));
}
