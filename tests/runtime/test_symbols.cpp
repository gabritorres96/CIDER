// Fixups (rebase / bind / lazy bind opcode streams) and symbol resolution.
#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

#include "cider/fixups.hpp"
#include "cider/libsystem.hpp"
#include "guest_code.hpp"
#include "test_framework.hpp"

using namespace cider;
using namespace cider::dyld;
using namespace cider::testing;

namespace {

std::vector<macho::Segment> segs4() {
    std::vector<macho::Segment> v(4);
    v[0].name = "__PAGEZERO"; v[0].vmsize = 0x100000000ull;
    v[1].name = "__TEXT"; v[1].vmaddr = 0x100000000ull; v[1].vmsize = 0x1000;
    v[2].name = "__DATA"; v[2].vmaddr = 0x100001000ull; v[2].vmsize = 0x1000;
    v[3].name = "__LINKEDIT"; v[3].vmaddr = 0x100002000ull; v[3].vmsize = 0x1000;
    return v;
}

ByteView bv(const std::vector<uint8_t>& v) { return ByteView(v); }

void name(std::vector<uint8_t>& s, const char* n) { s.insert(s.end(), n, n + std::strlen(n) + 1); }

}  // namespace

// ---- rebase streams -----------------------------------------------------------------------------------
CIDER_TEST(rebase_opcodes_decode_to_the_documented_addresses) {
    std::vector<uint8_t> s = {0x11,              // SET_TYPE_IMM pointer
                              0x22, 0x10,        // SET_SEGMENT_AND_OFFSET_ULEB seg 2, offset 0x10
                              0x53,              // DO_REBASE_IMM_TIMES 3        -> 0x10 0x18 0x20
                              0x42,              // ADD_ADDR_IMM_SCALED 2*8      -> addr 0x38
                              0x60, 0x02,        // DO_REBASE_ULEB_TIMES 2       -> 0x38 0x40
                              0x00};
    auto r = decode_rebase_stream(bv(s), segs4());
    REQUIRE(r.ok());
    std::vector<uint64_t> offs;
    for (auto& f : r.value()) { CHECK(f.kind == Fixup::Kind::Rebase); CHECK_EQ(f.segment, 2u); offs.push_back(f.offset); }
    CHECK((offs == std::vector<uint64_t>{0x10, 0x18, 0x20, 0x38, 0x40}));
}

CIDER_TEST(rebase_add_and_skip_opcodes) {
    // DO_REBASE_ADD_ADDR_ULEB: rebase at 0, then +0x10+8; DO_REBASE_ULEB_TIMES_SKIPPING_ULEB: 3 times, skip 8 -> stride 16
    std::vector<uint8_t> s = {0x11, 0x22, 0x00, 0x70, 0x10, 0x80, 0x03, 0x08, 0x30, 0x08, 0x00};
    auto r = decode_rebase_stream(bv(s), segs4());
    REQUIRE(r.ok());
    std::vector<uint64_t> offs;
    for (auto& f : r.value()) offs.push_back(f.offset);
    CHECK((offs == std::vector<uint64_t>{0x0, 0x18, 0x28, 0x38}));
}

CIDER_TEST(rebase_malformed_streams_are_rejected) {
    auto segs = segs4();
    CHECK_ERR(decode_rebase_stream(bv({0x11, 0x22}), segs), ErrorCode::Malformed);                        // truncated ULEB
    CHECK_ERR(decode_rebase_stream(bv({0x11, 0x51}), segs), ErrorCode::Malformed);                        // rebase before segment
    CHECK_ERR(decode_rebase_stream(bv({0x12, 0x22, 0x00, 0x51}), segs), ErrorCode::Unsupported);          // type 2 (abs32)
    CHECK_ERR(decode_rebase_stream(bv({0x13, 0x22, 0x00, 0x51}), segs), ErrorCode::Unsupported);          // type 3 (pcrel32)
    CHECK_ERR(decode_rebase_stream(bv({0x11, 0x2F, 0x00, 0x51}), segs), ErrorCode::Malformed);            // segment index 15
    CHECK_ERR(decode_rebase_stream(bv({0x11, 0x22, 0xF9, 0x1F, 0x51}), segs), ErrorCode::Malformed);      // offset 0xFF9+8 > vmsize
    CHECK_ERR(decode_rebase_stream(bv({0x11, 0x22, 0x00, 0xE0}), segs), ErrorCode::Unsupported);          // unknown opcode
    std::vector<uint8_t> huge = {0x11, 0x22, 0x00, 0x60};
    for (int i = 0; i < 5; ++i) huge.push_back(0xFF);
    huge.push_back(0x7F);                                                                                  // 2^41-ish repeat count
    CHECK_ERR(decode_rebase_stream(bv(huge), segs), ErrorCode::LimitExceeded);
    std::vector<uint8_t> ovf = {0x11, 0x22};
    for (int i = 0; i < 11; ++i) ovf.push_back(0xFF);
    CHECK_ERR(decode_rebase_stream(bv(ovf), segs), ErrorCode::Malformed);                                  // ULEB128 > 64 bits
    FixupLimits lim; lim.max_fixups = 2;
    CHECK_ERR(decode_rebase_stream(bv({0x11, 0x22, 0x00, 0x54, 0x00}), segs, lim), ErrorCode::LimitExceeded);
}

// ---- bind streams ---------------------------------------------------------------------------------------------
CIDER_TEST(bind_opcodes_decode_symbols_ordinals_addends_and_weak_flags) {
    std::vector<uint8_t> s = {0x11};                  // ordinal 1
    s.push_back(0x40); name(s, "_puts");              // SET_SYMBOL_TRAILING_FLAGS_IMM 0
    s.insert(s.end(), {0x51, 0x72, 0x00, 0x90});      // type pointer, segment 2 offset 0, DO_BIND            -> bind at 0
    s.insert(s.end(), {0x60, 0x7F});                  // SET_ADDEND_SLEB -1
    s.insert(s.end(), {0xA0, 0x08});                  // DO_BIND_ADD_ADDR_ULEB 8      -> bind at 8, next 24
    s.insert(s.end(), {0xB1});                        // DO_BIND_ADD_ADDR_IMM_SCALED 1 -> bind at 24, next 40
    s.push_back(0x41); name(s, "_w");                 // weak import
    s.push_back(0x3E);                                // SET_DYLIB_SPECIAL_IMM: 0xE|0xF0 = -2 (flat lookup)
    s.insert(s.end(), {0x90, 0x00});
    auto r = decode_bind_stream(bv(s), segs4(), false);
    REQUIRE(r.ok());
    const auto& v = r.value();
    REQUIRE_EQ(v.size(), size_t{4});
    CHECK_EQ(v[0].symbol, std::string("_puts")); CHECK_EQ(v[0].offset, uint64_t{0}); CHECK_EQ(v[0].library_ordinal, int64_t{1}); CHECK_EQ(v[0].addend, int64_t{0});
    CHECK_EQ(v[1].offset, uint64_t{8}); CHECK_EQ(v[1].addend, int64_t{-1});
    CHECK_EQ(v[2].offset, uint64_t{24});
    CHECK_EQ(v[3].symbol, std::string("_w")); CHECK_EQ(v[3].offset, uint64_t{40}); CHECK(v[3].weak_import); CHECK_EQ(v[3].library_ordinal, int64_t{-2});
    CHECK(!v[0].weak_import);
    CHECK(!v[0].lazy);
}

CIDER_TEST(bind_times_skipping_and_uleb_ordinals) {
    std::vector<uint8_t> s = {0x20, 0x81, 0x01};      // ordinal ULEB 129
    s.push_back(0x40); name(s, "_x");
    s.insert(s.end(), {0x51, 0x72, 0x00, 0xC0, 0x03, 0x08, 0x00});   // 3 binds, skipping 8 bytes each -> 0, 16, 32
    auto r = decode_bind_stream(bv(s), segs4(), false);
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().size(), size_t{3});
    CHECK_EQ(r.value()[2].offset, uint64_t{32});
    CHECK_EQ(r.value()[0].library_ordinal, int64_t{129});
}

CIDER_TEST(lazy_bind_streams_separate_entries_with_done) {
    std::vector<uint8_t> s;
    for (int i = 0; i < 3; ++i) {
        s.insert(s.end(), {0x72, static_cast<uint8_t>(i * 8), 0x11});   // segment 2, offset, ordinal 1
        s.push_back(0x40); name(s, i == 0 ? "_a" : i == 1 ? "_b" : "_c");
        s.insert(s.end(), {0x90, 0x00});                                  // DO_BIND, DONE
    }
    auto r = decode_bind_stream(bv(s), segs4(), true);
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().size(), size_t{3});
    CHECK(r.value()[1].lazy);
    CHECK_EQ(r.value()[2].symbol, std::string("_c"));
    CHECK_EQ(r.value()[2].offset, uint64_t{16});
}

CIDER_TEST(bind_malformed_streams_are_rejected) {
    auto segs = segs4();
    CHECK_ERR(decode_bind_stream(bv({0x11, 0x90}), segs, false), ErrorCode::Malformed);                    // bind before symbol/segment
    std::vector<uint8_t> a = {0x40}; name(a, "_x"); a.push_back(0x90);
    CHECK_ERR(decode_bind_stream(bv(a), segs, false), ErrorCode::Malformed);                               // bind before segment
    std::vector<uint8_t> unterminated = {0x40, '_', 'x'};
    CHECK_ERR(decode_bind_stream(bv(unterminated), segs, false), ErrorCode::Malformed);
    std::vector<uint8_t> t = {0x40}; name(t, "_x"); t.insert(t.end(), {0x52, 0x72, 0x00, 0x90});
    CHECK_ERR(decode_bind_stream(bv(t), segs, false), ErrorCode::Unsupported);                             // bind type 2
    CHECK_ERR(decode_bind_stream(bv({0xD0, 0x00}), segs, false), ErrorCode::Unsupported);                  // threaded
    CHECK_ERR(decode_bind_stream(bv({0xE0}), segs, false), ErrorCode::Unsupported);                        // unknown opcode
    std::vector<uint8_t> oob = {0x40}; name(oob, "_x"); oob.insert(oob.end(), {0x73, 0x00, 0x90});      // segment 3 is fine, 0x7F is not
    CHECK(decode_bind_stream(bv(oob), segs, false).ok());
    std::vector<uint8_t> bad_seg = {0x40}; name(bad_seg, "_x"); bad_seg.insert(bad_seg.end(), {0x7F, 0x00, 0x90});
    CHECK_ERR(decode_bind_stream(bv(bad_seg), segs, false), ErrorCode::Malformed);
    std::vector<uint8_t> bad_off = {0x40}; name(bad_off, "_x"); bad_off.insert(bad_off.end(), {0x72, 0xF9, 0x1F, 0x90});
    CHECK_ERR(decode_bind_stream(bv(bad_off), segs, false), ErrorCode::Malformed);
    CHECK_ERR(decode_bind_stream(bv({0x60}), segs, false), ErrorCode::Malformed);                          // truncated SLEB
}

// ---- whole-image decoding vs llvm-objdump ------------------------------------------------------------------------------
namespace {
struct Row { uint64_t addr; std::string sym; };

// Parses the golden `llvm-objdump --macho --rebase --bind --lazy-bind` output.
struct Golden {
    std::vector<uint64_t> rebase;
    std::vector<Row> bind, lazy;
};

Golden read_golden(const std::string& name) {
    std::ifstream f(kExecFixtures / "golden" / (name + ".fixups.txt"));
    if (!f) { report_failure(__FILE__, __LINE__, "golden missing for " + name); throw AbortTest{}; }
    Golden g;
    std::string line, section;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "Rebase table:") { section = "r"; continue; }
        if (line == "Bind table:") { section = "b"; continue; }
        if (line == "Lazy bind table:") { section = "l"; continue; }
        if (line.empty() || line.rfind("segment", 0) == 0 || section.empty()) continue;
        std::istringstream ls(line);
        std::vector<std::string> t;
        for (std::string w; ls >> w;) t.push_back(w);
        if (section == "r" && t.size() >= 3) g.rebase.push_back(std::stoull(t[2], nullptr, 16));
        else if (section == "b" && t.size() >= 7) g.bind.push_back({std::stoull(t[2], nullptr, 16), t.back()});
        else if (section == "l" && t.size() >= 5) g.lazy.push_back({std::stoull(t[2], nullptr, 16), t.back()});
    }
    return g;
}
}  // namespace

CIDER_TEST(decoded_fixups_equal_llvm_objdump_for_every_execution_fixture) {
    for (const char* n : {"ret42", "ret7", "ret99", "math", "stack", "calls", "globals", "bss", "libcall", "hello", "args", "missing", "exitcall",
                          "crash", "spin", "malloc", "fmath"}) {
        auto bytes = exec_fixture(n);
        auto file = macho::parse_file(ByteView(bytes)).value();
        const auto& img = *file.slices[0].image;
        auto fx = decode_fixups(img, ByteView(bytes));
        if (!fx.ok()) { report_failure(__FILE__, __LINE__, std::string(n) + ": " + fx.error().message); continue; }
        Golden g = read_golden(n);

        std::vector<uint64_t> rebase;
        std::vector<Row> bind, lazy;
        for (auto& f : fx.value()) {
            uint64_t addr = img.segments[f.segment].vmaddr + f.offset;
            if (f.kind == Fixup::Kind::Rebase) rebase.push_back(addr);
            else (f.lazy ? lazy : bind).push_back({addr, f.symbol});
        }
        auto by_addr = [](const Row& a, const Row& b) { return a.addr < b.addr; };
        std::sort(rebase.begin(), rebase.end());
        std::sort(g.rebase.begin(), g.rebase.end());
        std::sort(bind.begin(), bind.end(), by_addr); std::sort(g.bind.begin(), g.bind.end(), by_addr);
        std::sort(lazy.begin(), lazy.end(), by_addr); std::sort(g.lazy.begin(), g.lazy.end(), by_addr);
        if (!(rebase == g.rebase)) report_failure(__FILE__, __LINE__, std::string(n) + ": rebase addresses differ from llvm-objdump");
        REQUIRE_EQ(bind.size(), g.bind.size());
        REQUIRE_EQ(lazy.size(), g.lazy.size());
        for (size_t i = 0; i < bind.size(); ++i) { CHECK_EQ(bind[i].addr, g.bind[i].addr); CHECK_EQ(bind[i].sym, g.bind[i].sym); }
        for (size_t i = 0; i < lazy.size(); ++i) { CHECK_EQ(lazy[i].addr, g.lazy[i].addr); CHECK_EQ(lazy[i].sym, g.lazy[i].sym); }
    }
}

CIDER_TEST(chained_fixups_are_decoded_since_0_3_5) {
    auto bytes = read_file(std::filesystem::path(CIDER_REAL_FIXTURES_DIR) / "bin" / "hello_chained_x86_64", 1 << 20).value();
    auto file = macho::parse_file(ByteView(bytes)).value();
    auto fx = take(decode_fixups(*file.slices[0].image, ByteView(bytes)));
    std::vector<std::string> names;
    for (auto& f : fx) if (f.kind == Fixup::Kind::Bind) names.push_back(f.symbol);
    std::sort(names.begin(), names.end());
    CHECK_EQ(names.size(), size_t{2});
    CHECK_EQ(names[0], std::string("_exit"));
    CHECK_EQ(names[1], std::string("_puts"));
}

CIDER_TEST(weak_bind_streams_are_refused_by_the_decoder) {
    auto bytes = exec_fixture("hello");
    auto file = macho::parse_file(ByteView(bytes)).value();
    macho::Image img = *file.slices[0].image;
    img.dyld_info->weak_bind.size = 8;
    CHECK_ERR(decode_fixups(img, ByteView(bytes)), ErrorCode::Unsupported);
    img = *file.slices[0].image;
    img.dyld_info->bind.offset = 0x7fffffff;                       // stream outside the file
    img.dyld_info->bind.size = 16;
    CHECK_ERR(decode_fixups(img, ByteView(bytes)), ErrorCode::OutOfBounds);
}

// ---- resolution ---------------------------------------------------------------------------------------------------------------
namespace {
class FakeProvider final : public SymbolProvider {
public:
    std::optional<ResolvedSymbol> lookup(std::string_view lib, std::string_view sym) override {
        calls.emplace_back(std::string(lib), std::string(sym));
        if (!lib.empty() && lib != "/lib/x.dylib") return std::nullopt;
        if (sym == "_a") return ResolvedSymbol{0x1000, true};
        if (sym == "_b") return ResolvedSymbol{0x2000, true};
        return std::nullopt;
    }
    bool provides_library(std::string_view n) const override { return n == "/lib/x.dylib"; }
    std::vector<std::pair<std::string, std::string>> calls;
};

Fixup bind(const char* sym, int64_t ord, bool weak = false) {
    Fixup f;
    f.kind = Fixup::Kind::Bind;
    f.symbol = sym;
    f.library_ordinal = ord;
    f.weak_import = weak;
    return f;
}
}  // namespace

CIDER_TEST(resolver_collects_every_problem_in_one_pass) {
    macho::Image img;
    macho::Dylib x, y;
    x.path = "/lib/x.dylib";
    y.path = "/lib/other.dylib";
    img.dylibs = {x, y};
    FakeProvider p;
    std::vector<Fixup> fx = {bind("_a", 1), bind("_b", 1), bind("_missing", 1), bind("_c", 2), bind("_w", 1, true), bind("_z", 9),
                             bind("_self", 0), bind("_main", -1), bind("_a", -2), bind("_a", 1)};
    auto rep = resolve_symbols(img, fx, p);
    CHECK_EQ(rep.addresses.at("_a"), uint64_t{0x1000});
    CHECK_EQ(rep.addresses.at("_b"), uint64_t{0x2000});
    CHECK_EQ(rep.addresses.at("_w"), uint64_t{0});                      // weak import: bound to null, not an error
    REQUIRE_EQ(rep.weak_missing.size(), size_t{1});
    std::set<std::string> bad;
    for (auto& u : rep.unresolved) bad.insert(u.symbol);
    CHECK((bad == std::set<std::string>{"_missing", "_c", "_z", "_self", "_main"}));
    for (auto& u : rep.unresolved) {
        if (u.symbol == "_missing") CHECK(u.reason.find("not implemented") != std::string::npos);
        if (u.symbol == "_c") CHECK(u.reason.find("library not available") != std::string::npos);
        if (u.symbol == "_z") CHECK(u.reason.find("ordinal") != std::string::npos);
    }
    CHECK_EQ(rep.libraries.size(), size_t{2});
    // (symbol, ordinal) pairs are looked up once: "_a" via ordinal 1 appears twice in the input
    size_t a_calls = 0;
    for (auto& c : p.calls) a_calls += c.second == "_a" && c.first == "/lib/x.dylib";
    CHECK_EQ(a_calls, size_t{1});
}

CIDER_TEST(libsystem_provider_resolves_known_symbols_to_executable_thunks) {
    auto p = take(libsystem::LibSystemProvider::create());
    CHECK(p->provides_library("/usr/lib/libSystem.B.dylib"));
    CHECK(!p->provides_library("/usr/lib/libc++.1.dylib"));
    auto puts = p->lookup("/usr/lib/libSystem.B.dylib", "_puts");
    REQUIRE(puts.has_value());
    CHECK(memory::query_address(reinterpret_cast<void*>(puts->address)).executable);
    CHECK(p->lookup("", "_puts").has_value());                                       // flat lookup
    CHECK(!p->lookup("/usr/lib/libSystem.B.dylib", "_fork").has_value());           // not implemented
    CHECK(!p->lookup("/usr/lib/libSystem.B.dylib", "_sscanf").has_value());
    auto out = p->lookup("/usr/lib/libSystem.B.dylib", "___stdoutp");              // a data symbol: readable, not executable
    REQUIRE(out.has_value());
    CHECK(!out->is_function);
    CHECK(!memory::query_address(reinterpret_cast<void*>(out->address)).executable);
    CHECK(!p->lookup("/usr/lib/libz.1.dylib", "_puts").has_value());                // wrong library
    auto a = p->lookup("", "_strlen").value().address, b = p->lookup("", "_memcpy").value().address;
    CHECK(a != b);
}

CIDER_TEST(every_execution_fixture_but_the_missing_symbol_one_resolves_completely) {
    auto p = take(libsystem::LibSystemProvider::create());
    for (const char* n : {"ret42", "math", "stack", "calls", "globals", "bss", "libcall", "hello", "args", "exitcall", "crash", "spin", "malloc", "fmath"}) {
        auto bytes = exec_fixture(n);
        auto file = macho::parse_file(ByteView(bytes)).value();
        auto fx = decode_fixups(*file.slices[0].image, ByteView(bytes)).value();
        auto rep = resolve_symbols(*file.slices[0].image, fx, *p);
        if (!rep.unresolved.empty()) report_failure(__FILE__, __LINE__, std::string(n) + ": unresolved " + rep.unresolved[0].symbol);
    }
    auto bytes = exec_fixture("missing");
    auto file = macho::parse_file(ByteView(bytes)).value();
    auto fx = decode_fixups(*file.slices[0].image, ByteView(bytes)).value();
    auto rep = resolve_symbols(*file.slices[0].image, fx, *p);
    REQUIRE_EQ(rep.unresolved.size(), size_t{1});
    CHECK_EQ(rep.unresolved[0].symbol, std::string("_fork"));
}
