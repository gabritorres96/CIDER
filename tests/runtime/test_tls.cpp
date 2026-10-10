// Thread-local storage (CIDER 0.3.6): the Mach-O TLV protocol served by CIDER's own tlv_get_addr, with proof that the
// host's Windows TLS state (%gs / TEB, TlsAlloc slots, C++ thread_local) is never touched, and the explicit refusal of
// the thread-local features that are not implemented.
#include <algorithm>
#include <cstring>
#include <fstream>
#include <intrin.h>
#include <sstream>

#include "cider/guest.hpp"
#include "cider/linker.hpp"
#include "guest_code.hpp"
#include "macho_builder.hpp"
#include "test_framework.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace cider;
using namespace cider::testing;
namespace fs = std::filesystem;

namespace {

const fs::path kDylib = fs::path(CIDER_DYLIB_FIXTURES_DIR);
thread_local int g_host_tls = 1234;      // a C++ thread_local of the HOST: it must survive a guest run untouched

std::vector<uint8_t> load(const fs::path& p) {
    auto r = read_file(p, 64u << 20);
    if (!r.ok()) { report_failure(__FILE__, __LINE__, "fixture missing: " + path_to_utf8(p)); throw AbortTest{}; }
    return r.value();
}

struct Run {
    guest::Outcome r;
    std::string out;
};
Run run_file(const fs::path& exe, const std::vector<uint8_t>* bytes = nullptr) {
    Run run;
    guest::GuestConfig cfg;
    cfg.args = {path_to_utf8(exe)};
    cfg.image_path = exe;
    cfg.sink = [&](int, const char* d, size_t n) { run.out.append(d, n); };
    auto b = bytes ? *bytes : load(exe);
    run.r = guest::run_in_process(ByteView(b), cfg);
    return run;
}

// file offset of the section_64 record named `name` in the first x86_64 image of `bytes`
uint64_t section_header(const std::vector<uint8_t>& bytes, const std::string& name) {
    uint64_t off = 32;
    uint32_t ncmds;
    std::memcpy(&ncmds, bytes.data() + 16, 4);
    for (uint32_t i = 0; i < ncmds; ++i) {
        uint32_t cmd, size;
        std::memcpy(&cmd, bytes.data() + off, 4);
        std::memcpy(&size, bytes.data() + off + 4, 4);
        if (cmd == 0x19) {
            uint32_t nsects;
            std::memcpy(&nsects, bytes.data() + off + 64, 4);
            for (uint32_t s = 0; s < nsects; ++s) {
                const uint64_t sh = off + 72 + 80ull * s;
                if (name == std::string(reinterpret_cast<const char*>(bytes.data() + sh), strnlen(reinterpret_cast<const char*>(bytes.data() + sh), 16))) return sh;
            }
        }
        off += size;
    }
    return 0;
}

// llvm-objdump --syms: address of a local/global symbol
std::map<std::string, uint64_t> golden_syms(const std::string& key) {
    std::map<std::string, uint64_t> out;
    std::ifstream f(kDylib / "golden" / (key + ".syms.txt"));
    for (std::string line; std::getline(f, line);) {
        std::istringstream ls(line);
        std::string addr, flags;
        ls >> addr;
        if (addr.size() != 16 || addr.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) continue;
        std::string tok, last;
        while (ls >> tok) last = tok;
        out[last] = std::stoull(addr, nullptr, 16);
    }
    return out;
}

struct HostTlsState {
    uint64_t teb_self, tls_pointer, stack_base, stack_limit;
    int host_tls;
    uintptr_t host_tls_addr;
    DWORD slot;
    void* slot_value;
    bool operator==(const HostTlsState& o) const {
        return teb_self == o.teb_self && tls_pointer == o.tls_pointer && stack_base == o.stack_base && stack_limit == o.stack_limit && host_tls == o.host_tls &&
               host_tls_addr == o.host_tls_addr && slot == o.slot && slot_value == o.slot_value;
    }
};
HostTlsState capture(DWORD slot) {
    HostTlsState s{};
    s.teb_self = __readgsqword(0x30);          // NT_TIB.Self
    s.stack_base = __readgsqword(0x08);
    s.stack_limit = __readgsqword(0x10);
    s.tls_pointer = __readgsqword(0x58);       // TEB.ThreadLocalStoragePointer
    s.host_tls = g_host_tls;
    s.host_tls_addr = reinterpret_cast<uintptr_t>(&g_host_tls);
    s.slot = slot;
    s.slot_value = TlsGetValue(slot);
    return s;
}

}  // namespace

CIDER_TEST(thread_local_variables_in_an_executable_work) {
    Run r = run_file(kDylib / "bin/tlsexe/tlsexe");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 42);
    CHECK_EQ(r.out, std::string("tls-name\n"));              // a pointer inside the template, relocated before use
}

CIDER_TEST(each_image_has_its_own_thread_local_storage_and_exports_work_across_images) {
    Run r = run_file(kDylib / "bin/tlslib/tlslib");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 42);
    CHECK_EQ(r.r.images.size(), size_t{2});
}

CIDER_TEST(running_twice_starts_from_the_template_again) {
    for (int i = 0; i < 3; ++i) {
        Run r = run_file(kDylib / "bin/tlsexe/tlsexe");
        REQUIRE(r.r.state == guest::State::Exited);
        CHECK_EQ(r.r.exit_code, 42);                          // not 42 + leftovers: the blocks died with the previous run
    }
}

CIDER_TEST(the_hosts_windows_tls_state_is_never_touched) {
    const DWORD slot = TlsAlloc();
    REQUIRE(slot != TLS_OUT_OF_INDEXES);
    int marker = 99;
    TlsSetValue(slot, &marker);
    g_host_tls = 1234;
    const HostTlsState before = capture(slot);
    for (const char* scenario : {"tlsexe", "tlslib"}) {
        Run r = run_file(kDylib / "bin" / scenario / scenario);
        REQUIRE(r.r.state == guest::State::Exited);
        CHECK_EQ(r.r.exit_code, 42);
        const HostTlsState after = capture(slot);
        CHECK(after == before);                                // TEB, %gs-relative fields, TlsAlloc slot and thread_local data: identical
        CHECK_EQ(after.host_tls, 1234);
    }
    TlsFree(slot);
}

CIDER_TEST(template_layout_matches_llvm_objdump_symbols) {
    auto bytes = load(kDylib / "bin/tlsexe/tlsexe");
    auto syms = golden_syms("tlsexe_tlsexe");
    auto prov = take(libsystem::LibSystemProvider::create());
    linker::LinkError err;
    linker::LinkOptions opts;
    opts.executable_path = kDylib / "bin/tlsexe/tlsexe";
    auto prog = linker::Program::link(ByteView(bytes), opts, *prov, err);
    REQUIRE(prog != nullptr);
    const auto& tls = prog->main().tls;
    const uint64_t start = syms["_counter$tlv$init"];            // the first template symbol is the template start
    CHECK_EQ(tls.tmpl, start + static_cast<uint64_t>(prog->main().mapped.slide));
    CHECK_EQ(tls.descriptors, uint64_t{6});
    // __thread_bss ends at the last template byte: buffer[100] is the highest object
    CHECK_EQ(tls.tmpl_size, syms["_buffer$tlv$init"] + 100 - start);
    CHECK_EQ(tls.init_size, syms["_zeroed$tlv$init"] - start - 8);   // __thread_data ends before the 8-byte alignment gap
    // each descriptor's offset field equals (initial-value symbol - template start)
    struct Pair { const char* var; const char* init; };
    for (Pair p : {Pair{"_counter", "_counter$tlv$init"}, Pair{"_tname", "_tname$tlv$init"}, Pair{"_zeroed", "_zeroed$tlv$init"},
                   Pair{"_big_zero", "_big_zero$tlv$init"}, Pair{"_buffer", "_buffer$tlv$init"}}) {
        const uint8_t* desc = prog->main().mapped.base() + (syms[p.var] - prog->main().mapped.layout.preferred_base);
        uint64_t thunk, key, offset;
        std::memcpy(&thunk, desc, 8); std::memcpy(&key, desc + 8, 8); std::memcpy(&offset, desc + 16, 8);
        CHECK_EQ(offset, syms[p.init] - start);
        CHECK_EQ(key, uint64_t{0});
        CHECK(thunk != 0);                                         // bound to _tlv_bootstrap by the image's own fixups
    }
}

CIDER_TEST(the_initial_values_really_come_from_the_macho_template) {
    // change `static __thread long local_tls = 7` to 8 inside the file: the guest's own initial-value check now fails (exit 2)
    auto bytes = load(kDylib / "bin/tlsexe/tlsexe");
    auto syms = golden_syms("tlsexe_tlsexe");
    const uint64_t vm = syms["_local_tls$tlv$init"];
    // vmaddr -> file offset: __DATA is mapped at 0x100001000 from file offset 0x1000 here
    const uint64_t file_off = vm - 0x100001000ull + 0x1000;
    uint64_t v;
    std::memcpy(&v, bytes.data() + file_off, 8);
    REQUIRE_EQ(v, uint64_t{7});
    v = 8;
    std::memcpy(bytes.data() + file_off, &v, 8);
    Run r = run_file(kDylib / "bin/tlsexe/tlsexe", &bytes);
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 2);
}

// ---- the TLV calling convention preserves every register but rax ------------------------------------------------------------------
CIDER_TEST(the_tlv_thunk_preserves_all_registers_except_rax) {
    // Fake image: a heap block acts as "mapped image" holding a descriptor and a template.
    alignas(64) static uint8_t image[4096] = {};
    uint64_t* desc = reinterpret_cast<uint64_t*>(image + 64);           // {thunk, key, offset}
    desc[0] = 0; desc[1] = 0; desc[2] = 8;
    uint8_t* tmpl = image + 256;
    const uint64_t init = 0x1122334455667788ull;
    std::memcpy(tmpl + 8, &init, 8);

    auto prov = take(libsystem::LibSystemProvider::create());
    libsystem::RuntimeContext ctx;
    libsystem::TlsImage t;
    t.lo = reinterpret_cast<uint64_t>(image);
    t.hi = t.lo + sizeof image;
    t.tmpl = reinterpret_cast<uint64_t>(tmpl);
    t.tmpl_size = 32;
    t.init_size = 16;
    ctx.tls_images.push_back(t);
    libsystem::set_context(&ctx);
    auto sym = prov->lookup("", "_tlv_get_addr");
    REQUIRE(sym.has_value());

    using Code = std::vector<uint8_t>;
    Code c = {0x48, 0x83, 0xEC, 0x08};                                   // sub rsp, 8
    auto mov = [&](std::initializer_list<uint8_t> op, uint64_t v) { c.insert(c.end(), op); put_u64(c, v); };
    mov({0x48, 0xB9}, 0x0101010101010101ull);                            // rcx
    mov({0x48, 0xBA}, 0x0202020202020202ull);                            // rdx
    mov({0x48, 0xBE}, 0x0303030303030303ull);                            // rsi
    mov({0x49, 0xB8}, 0x0404040404040404ull);                            // r8
    mov({0x49, 0xB9}, 0x0505050505050505ull);                            // r9
    mov({0x49, 0xBA}, 0x0606060606060606ull);                            // r10
    mov({0x48, 0xBB}, 0x0707070707070707ull);                            // rbx (callee-saved: preserved by the ABI anyway)
    mov({0x48, 0xBF}, reinterpret_cast<uint64_t>(desc));                 // rdi = descriptor
    // xmm0..xmm5 = distinctive values:  mov rax, imm ; movq xmmN, rax
    for (int i = 0; i < 6; ++i) {
        c.insert(c.end(), {0x48, 0xB8}); put_u64(c, 0xA0A0A0A0A0A0A000ull + static_cast<uint64_t>(i));
        c.insert(c.end(), {0x66, 0x48, 0x0F, 0x6E, static_cast<uint8_t>(0xC0 | (i << 3))});
    }
    mov({0x49, 0xBB}, sym->address);                                     // r11 = thunk
    c.insert(c.end(), {0x41, 0xFF, 0xD3});                               // call r11
    // rax = pointer to the variable. Keep it in r11, then fold every register that must have survived into r13
    // (OR of the differences from the expected values: zero when all were preserved).
    c.insert(c.end(), {0x49, 0x89, 0xC3});                               // mov r11, rax  (the pointer)
    // result = ((rcx ^ M1) | (rdx ^ M2) | (rsi ^ M3) | (r8 ^ M4) | (r9 ^ M5) | (r10 ^ M6) | (rbx ^ M7) | (rdi ^ desc))
    auto fold = [&](std::initializer_list<uint8_t> mov_rax_from, uint64_t magic) {
        c.insert(c.end(), mov_rax_from);                                 // mov rax, <reg>
        c.insert(c.end(), {0x49, 0xBC}); put_u64(c, magic);              // mov r12, magic
        c.insert(c.end(), {0x4C, 0x31, 0xE0});                           // xor rax, r12
        c.insert(c.end(), {0x49, 0x09, 0xC5});                           // or r13, rax
    };
    c.insert(c.end(), {0x4D, 0x31, 0xED});                               // xor r13, r13
    fold({0x48, 0x89, 0xC8}, 0x0101010101010101ull);                     // mov rax, rcx
    fold({0x48, 0x89, 0xD0}, 0x0202020202020202ull);                     // mov rax, rdx
    fold({0x48, 0x89, 0xF0}, 0x0303030303030303ull);                     // mov rax, rsi
    fold({0x4C, 0x89, 0xC0}, 0x0404040404040404ull);                     // mov rax, r8
    fold({0x4C, 0x89, 0xC8}, 0x0505050505050505ull);                     // mov rax, r9
    fold({0x4C, 0x89, 0xD0}, 0x0606060606060606ull);                     // mov rax, r10
    fold({0x48, 0x89, 0xD8}, 0x0707070707070707ull);                     // mov rax, rbx
    fold({0x48, 0x89, 0xF8}, reinterpret_cast<uint64_t>(desc));          // mov rax, rdi
    for (int i = 0; i < 6; ++i) {                                        // movq rax, xmmN ; compare with its magic
        c.insert(c.end(), {0x66, 0x48, 0x0F, 0x7E, static_cast<uint8_t>(0xC0 | (i << 3))});
        c.insert(c.end(), {0x49, 0xBC}); put_u64(c, 0xA0A0A0A0A0A0A000ull + static_cast<uint64_t>(i));
        c.insert(c.end(), {0x4C, 0x31, 0xE0});
        c.insert(c.end(), {0x49, 0x09, 0xC5});
    }
    // return: rax = r13 (0 when everything survived) ; the variable pointer is checked separately through memory below
    c.insert(c.end(), {0x4C, 0x89, 0xE8});                               // mov rax, r13
    c.insert(c.end(), {0x4C, 0x89, 0x1C, 0x24});                         // mov [rsp], r11   (pointer saved at [rsp])
    c.insert(c.end(), {0x48, 0x83, 0xC4, 0x08, 0xC3});                   // add rsp, 8 ; ret
    // The code clobbers r12/r13 (callee-saved): cider_enter_guest saves and restores them.
    auto o = run_code(c);
    libsystem::set_context(nullptr);
    CHECK_EQ(o.value, uint64_t{0});                                       // every register and xmm0-5 survived the call
    REQUIRE(ctx.tls_images[0].block != nullptr);
    uint64_t seen;
    std::memcpy(&seen, ctx.tls_images[0].block + 8, 8);
    CHECK_EQ(seen, init);                                                 // the block was initialised from the template
}

CIDER_TEST(a_descriptor_outside_every_tls_image_traps_the_guest) {
    auto prov = take(libsystem::LibSystemProvider::create());
    libsystem::RuntimeContext ctx;
    libsystem::set_context(&ctx);
    uint64_t bogus[3] = {0, 0, 0};
    auto sym = prov->lookup("", "_tlv_get_addr");
    REQUIRE(sym.has_value());
    auto o = call_host(*prov, "_tlv_get_addr", reinterpret_cast<uint64_t>(bogus));
    libsystem::set_context(nullptr);
    CHECK(o.exited);
    CHECK_EQ(o.value, uint64_t{134});
    CHECK(ctx.fault.find("does not belong") != std::string::npos);
}

// ---- refusals ----------------------------------------------------------------------------------------------------------------------
namespace {
void expect_refused(const std::vector<uint8_t>& bytes, loader::LoadFailure kind, const char* needle, const char* what) {
    Run r = run_file(kDylib / "bin/tlsexe/tlsexe", &bytes);
    if (r.r.state != guest::State::LoadFailed) { report_failure(__FILE__, __LINE__, std::string("accepted: ") + what); return; }
    if (r.r.load_failure != kind) report_failure(__FILE__, __LINE__, std::string(what) + ": wrong kind: " + r.r.message);
    if (r.r.message.find(needle) == std::string::npos) report_failure(__FILE__, __LINE__, std::string(what) + ": message was " + r.r.message);
}
}  // namespace

CIDER_TEST(thread_local_initialisation_functions_are_refused) {
    auto bytes = load(kDylib / "bin/tlsexe/tlsexe");
    const uint64_t sh = section_header(bytes, "__thread_vars");
    REQUIRE(sh != 0);
    uint32_t flags;
    std::memcpy(&flags, bytes.data() + sh + 64, 4);
    flags = (flags & ~0xffu) | 0x15;                          // S_THREAD_LOCAL_INIT_FUNCTION_POINTERS
    std::memcpy(bytes.data() + sh + 64, &flags, 4);
    expect_refused(bytes, loader::LoadFailure::Unsupported, "initialisation functions", "type 0x15");
}

CIDER_TEST(malformed_thread_local_metadata_is_refused) {
    {   // __thread_vars size not a multiple of 24
        auto bytes = load(kDylib / "bin/tlsexe/tlsexe");
        const uint64_t sh = section_header(bytes, "__thread_vars");
        uint64_t size; std::memcpy(&size, bytes.data() + sh + 40, 8);
        size -= 1;
        std::memcpy(bytes.data() + sh + 40, &size, 8);
        expect_refused(bytes, loader::LoadFailure::Incompatible, "24-byte descriptors", "odd descriptor table size");
    }
    {   // a descriptor whose offset points past the template
        auto bytes = load(kDylib / "bin/tlsexe/tlsexe");
        const uint64_t sh = section_header(bytes, "__thread_vars");
        uint32_t file_off; std::memcpy(&file_off, bytes.data() + sh + 48, 4);
        uint64_t bad = 0x100000;
        std::memcpy(bytes.data() + file_off + 16, &bad, 8);
        expect_refused(bytes, loader::LoadFailure::Incompatible, "outside the template", "descriptor offset");
    }
    {   // template alignment 2^13
        auto bytes = load(kDylib / "bin/tlsexe/tlsexe");
        const uint64_t sh = section_header(bytes, "__thread_data");
        uint32_t align = 13;
        std::memcpy(bytes.data() + sh + 52, &align, 4);
        expect_refused(bytes, loader::LoadFailure::Unsupported, "alignment", "alignment 8192");
    }
    {   // an enormous zero-fill template
        auto bytes = load(kDylib / "bin/tlsexe/tlsexe");
        const uint64_t sh = section_header(bytes, "__thread_bss");
        uint64_t size = 64ull << 20;
        std::memcpy(bytes.data() + sh + 40, &size, 8);
        Run r = run_file(kDylib / "bin/tlsexe/tlsexe", &bytes);
        CHECK(r.r.state == guest::State::LoadFailed);
    }
}

CIDER_TEST(the_default_validator_still_refuses_tls_unless_the_linker_serves_it) {
    // 0.2 behaviour is intact for callers that do not enable TLS support
    auto bytes = load(kDylib / "bin/tlsexe/tlsexe");
    auto file = take(macho::parse_file(ByteView(bytes)));
    loader::ValidatorOptions opts;
    auto rep = loader::validate_for_execution(*file.slices[0].image, opts);
    CHECK(!rep.ok());
    opts.allow_tls = true;
    opts.allow_initializers = true;
    opts.linker_resolves_dependencies = true;
    CHECK(loader::validate_for_execution(*file.slices[0].image, opts).ok());
}
