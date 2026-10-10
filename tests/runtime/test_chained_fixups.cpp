// LC_DYLD_CHAINED_FIXUPS (CIDER 0.3.5): decoding checked against llvm-objdump --dyld-info / --chained-fixups on real
// ld64.lld output, execution of chained executables and dylibs, a patched PTR_64_OFFSET variant, and the refusal
// of every format and malformation CIDER does not implement.
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>

#include "cider/fixups.hpp"
#include "cider/guest.hpp"
#include "cider/linker.hpp"
#include "cider/loader.hpp"
#include "guest_code.hpp"
#include "macho_builder.hpp"
#include "test_framework.hpp"

using namespace cider;
using namespace cider::testing;
namespace fs = std::filesystem;

namespace {

const fs::path kDylib = fs::path(CIDER_DYLIB_FIXTURES_DIR);
const fs::path kReal = fs::path(CIDER_REAL_FIXTURES_DIR);

std::vector<uint8_t> load(const fs::path& p) {
    auto r = read_file(p, 64u << 20);
    if (!r.ok()) { report_failure(__FILE__, __LINE__, "fixture missing: " + path_to_utf8(p)); throw AbortTest{}; }
    return r.value();
}

struct Parsed {
    macho::File file;
    const macho::Image* img = nullptr;
    ByteView slice;
};
Parsed parse(const std::vector<uint8_t>& bytes) {
    Parsed p;
    p.file = take(macho::parse_file(ByteView(bytes)));
    const auto* sl = p.file.find_slice(macho::kCpuX86_64);
    if (!sl || !sl->image) { report_failure(__FILE__, __LINE__, "no x86_64 slice"); throw AbortTest{}; }
    p.img = &*sl->image;
    p.slice = take(ByteView(bytes).slice(sl->offset, sl->size));
    return p;
}

// Offset (inside the file) of the chained-fixups blob, and of the first segment's starts record.
struct Blob {
    uint64_t base = 0;     // blob start
    uint64_t starts = 0;   // starts_in_image
    uint64_t imports = 0, symbols = 0;
    uint64_t seg_record(const std::vector<uint8_t>& b, int seg_index) const {
        uint32_t off;
        std::memcpy(&off, b.data() + starts + 4 + 4 * seg_index, 4);
        return off ? starts + off : 0;
    }
};
Blob blob_of(const std::vector<uint8_t>& bytes) {
    Parsed p = parse(bytes);
    Blob b;
    b.base = p.img->chained_fixups->offset;
    uint32_t v;
    std::memcpy(&v, bytes.data() + b.base + 4, 4);  b.starts = b.base + v;
    std::memcpy(&v, bytes.data() + b.base + 8, 4);  b.imports = b.base + v;
    std::memcpy(&v, bytes.data() + b.base + 12, 4); b.symbols = b.base + v;
    return b;
}
void put32(std::vector<uint8_t>& b, uint64_t off, uint32_t v) { std::memcpy(b.data() + off, &v, 4); }
void put16(std::vector<uint8_t>& b, uint64_t off, uint16_t v) { std::memcpy(b.data() + off, &v, 2); }
uint64_t get64(const std::vector<uint8_t>& b, uint64_t off) { uint64_t v; std::memcpy(&v, b.data() + off, 8); return v; }
void put64(std::vector<uint8_t>& b, uint64_t off, uint64_t v) { std::memcpy(b.data() + off, &v, 8); }

// llvm-objdump --dyld-info lines for chained images:
//   segment section address pointer type addend dylib symbol/vm address
struct Expected {
    std::string kind;       // "rebase" | "bind"
    uint64_t pointer = 0;
    std::string symbol;     // bind
    std::string dylib;      // bind
    uint64_t addend = 0;    // bind
    uint64_t target = 0;    // rebase
};
std::map<uint64_t, Expected> golden_dyld_info(const std::string& key) {
    std::map<uint64_t, Expected> out;
    std::ifstream f(kDylib / "golden" / (key + ".dyldinfo.txt"));
    if (!f) { f.open(kReal / "golden" / (key + ".dyldinfo.txt")); }
    for (std::string line; std::getline(f, line);) {
        std::istringstream ls(line);
        std::vector<std::string> tok;
        for (std::string t; ls >> t;) tok.push_back(t);
        // data rows have a segment name starting with "__" and an address starting with 0x
        if (tok.size() < 6 || tok[0].rfind("__", 0) != 0 || tok[2].rfind("0x", 0) != 0) continue;
        Expected e;
        const uint64_t addr = std::stoull(tok[2], nullptr, 16);
        e.pointer = std::stoull(tok[3], nullptr, 16);
        e.kind = tok[4];
        if (e.kind == "rebase") {
            e.target = std::stoull(tok.back(), nullptr, 16);
        } else if (e.kind == "bind") {
            e.addend = std::stoull(tok[5], nullptr, 16);
            e.dylib = tok[6];
            e.symbol = tok[7];
        }
        out[addr] = e;
    }
    return out;
}

std::string leaf_stem(const std::string& path) {   // "@rpath/libchain.dylib" -> "libchain"; "/usr/lib/libSystem.B.dylib" -> "libSystem"
    std::string n = path.substr(path.find_last_of('/') + 1);
    auto dot = n.find('.');
    return dot == std::string::npos ? n : n.substr(0, dot);
}

void compare_with_objdump(const std::vector<uint8_t>& bytes, const std::string& key) {
    Parsed p = parse(bytes);
    auto fixups = take(dyld::decode_fixups(*p.img, p.slice));
    auto gold = golden_dyld_info(key);
    if (fixups.size() != gold.size()) { report_failure(__FILE__, __LINE__, key + ": " + std::to_string(fixups.size()) + " fixups, llvm-objdump lists " + std::to_string(gold.size())); return; }
    for (const auto& f : fixups) {
        const uint64_t addr = p.img->segments[f.segment].vmaddr + f.offset;
        auto it = gold.find(addr);
        if (it == gold.end()) { report_failure(__FILE__, __LINE__, key + ": unexpected fixup at " + hex(addr)); continue; }
        const Expected& e = it->second;
        if (f.kind == dyld::Fixup::Kind::ChainedRebase) {
            if (e.kind != "rebase" || e.target != f.target) report_failure(__FILE__, __LINE__, key + ": rebase mismatch at " + hex(addr));
        } else if (f.kind == dyld::Fixup::Kind::Bind) {
            const std::string lib = f.library_ordinal >= 1 ? leaf_stem(p.img->dylibs[static_cast<size_t>(f.library_ordinal) - 1].path) : "?";
            if (e.kind != "bind" || e.symbol != f.symbol || e.dylib != lib || e.addend != static_cast<uint64_t>(f.addend))
                report_failure(__FILE__, __LINE__, key + ": bind mismatch at " + hex(addr) + " (" + f.symbol + " vs " + e.symbol + ")");
        } else {
            report_failure(__FILE__, __LINE__, key + ": a chained image produced a classic rebase");
        }
    }
}

struct Run {
    guest::Outcome r;
    std::string out;
};
Run run_file(const fs::path& exe, const std::vector<uint8_t>* patched = nullptr) {
    Run run;
    guest::GuestConfig cfg;
    cfg.args = {path_to_utf8(exe)};
    cfg.image_path = exe;
    cfg.sink = [&](int, const char* d, size_t n) { run.out.append(d, n); };
    auto bytes = patched ? *patched : load(exe);
    run.r = guest::run_in_process(ByteView(bytes), cfg);
    return run;
}

guest::Outcome run_patched(const std::vector<uint8_t>& bytes) {
    guest::GuestConfig cfg;
    cfg.args = {"x"};
    cfg.sink = [](int, const char*, size_t) {};
    return guest::run_in_process(ByteView(bytes), cfg);
}

}  // namespace

// ============================== decoding against llvm-objdump ====================================================================
CIDER_TEST(decoded_fixups_equal_llvm_objdump_dyld_info) {
    compare_with_objdump(load(kDylib / "bin/chained/chained"), "chained_chained");
    compare_with_objdump(load(kDylib / "bin/chained/libchain.dylib"), "chained_libchain.dylib");
    compare_with_objdump(load(kDylib / "bin/chainexe/chainexe"), "chainexe_chainexe");
}

CIDER_TEST(chained_header_fields_equal_llvm_objdump) {
    for (const char* key : {"chained_chained", "chainexe_chainexe"}) {
        std::string path = std::string(key) == "chained_chained" ? "bin/chained/chained" : "bin/chainexe/chainexe";
        auto bytes = load(kDylib / path);
        Blob b = blob_of(bytes);
        std::ifstream f(kDylib / "golden" / (std::string(key) + ".chained.txt"));
        std::map<std::string, uint64_t> kv;
        for (std::string line; std::getline(f, line);) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::istringstream l(line.substr(eq + 1));
            uint64_t v;
            std::string name = line.substr(0, eq);
            while (!name.empty() && name.front() == ' ') name.erase(name.begin());
            while (!name.empty() && name.back() == ' ') name.pop_back();
            if (l >> v) kv[name] = v;
        }
        uint32_t starts_offset, imports_count, imports_format;
        std::memcpy(&starts_offset, bytes.data() + b.base + 4, 4);
        std::memcpy(&imports_count, bytes.data() + b.base + 16, 4);
        std::memcpy(&imports_format, bytes.data() + b.base + 20, 4);
        CHECK_EQ(kv["starts_offset"], uint64_t{starts_offset});
        CHECK_EQ(kv["imports_count"], uint64_t{imports_count});
        CHECK_EQ(kv["imports_format"], uint64_t{imports_format});
        CHECK_EQ(kv["pointer_format"], uint64_t{2});
    }
}

CIDER_TEST(an_image_without_chained_fixups_decodes_to_nothing) {
    auto bytes = load(kDylib / "bin/one/one");
    Parsed p = parse(bytes);
    CHECK(take(dyld::decode_chained_fixups(*p.img, p.slice)).empty());
}

// ============================== execution ===========================================================================================
CIDER_TEST(chained_executable_with_rebases_and_binds_runs) {
    Run r = run_file(kDylib / "bin/chainexe/chainexe");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 42);
    CHECK_EQ(r.out, std::string("alpha gamma 80\n"));
}

CIDER_TEST(chained_executable_and_chained_dylib_link_together) {
    Run r = run_file(kDylib / "bin/chained/chained");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 42);
    CHECK_EQ(r.out, std::string("chain-lib\nbc def\n"));
    CHECK_EQ(r.r.images.size(), size_t{2});
}

CIDER_TEST(the_hello_chained_fixture_of_0_1_1_now_runs) {
    Run r = run_file(kReal / "bin/hello_chained_x86_64");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 0);
    CHECK_EQ(r.out, std::string("hello from a real Mach-O\n"));
}

CIDER_TEST(chained_images_are_relocatable_by_the_slide) {
    // Occupy the preferred base so the executable must be relocated: every chained rebase must add the slide.
    auto blocker = memory::Region::reserve(0x100000000ull, 0x10000, false);
    if (!blocker.ok()) SKIP("cannot reserve the preferred base");
    Run r = run_file(kDylib / "bin/chainexe/chainexe");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 42);
    CHECK(r.r.image_base != 0x100000000ull);
}

CIDER_TEST(pointer_format_64_offset_is_decoded_like_pointer_format_64) {
    // Rewrite a PTR_64 image as PTR_64_OFFSET: format 6, rebase targets stored as offsets from the header.
    auto bytes = load(kDylib / "bin/chainexe/chainexe");
    Parsed p = parse(bytes);
    auto before = take(dyld::decode_fixups(*p.img, p.slice));
    Blob b = blob_of(bytes);
    uint64_t header_vmaddr = 0;
    for (auto& s : p.img->segments) if (s.fileoff == 0 && s.filesize > 0 && s.name != "__PAGEZERO") header_vmaddr = s.vmaddr;
    for (size_t si = 0; si < p.img->segments.size(); ++si) {
        uint64_t rec = b.seg_record(bytes, static_cast<int>(si));
        if (rec) put16(bytes, rec + 6, 6);
    }
    for (auto& f : before) {
        if (f.kind != dyld::Fixup::Kind::ChainedRebase) continue;
        const auto& seg = p.img->segments[f.segment];
        const uint64_t at = seg.fileoff + f.offset;
        uint64_t v = get64(bytes, at);
        v = (v & ~0xFFFFFFFFFull) | ((f.target - header_vmaddr) & 0xFFFFFFFFFull);
        put64(bytes, at, v);
    }
    Parsed q = parse(bytes);
    auto after = take(dyld::decode_fixups(*q.img, q.slice));
    REQUIRE_EQ(after.size(), before.size());
    for (size_t i = 0; i < after.size(); ++i) {
        CHECK(after[i].kind == before[i].kind);
        CHECK_EQ(after[i].target, before[i].target);                       // the same unslid virtual address
        CHECK_EQ(after[i].offset, before[i].offset);
    }
    auto o = run_patched(bytes);
    REQUIRE(o.state == guest::State::Exited);
    CHECK_EQ(o.exit_code, 42);
}

CIDER_TEST(the_high8_byte_of_a_rebase_lands_in_bits_56_to_63) {
    auto bytes = load(kDylib / "bin/chainexe/chainexe");
    Parsed p = parse(bytes);
    auto fixups = take(dyld::decode_fixups(*p.img, p.slice));
    const dyld::Fixup* rb = nullptr;
    for (auto& f : fixups) if (f.kind == dyld::Fixup::Kind::ChainedRebase) { rb = &f; break; }
    REQUIRE(rb != nullptr);
    const auto& seg = p.img->segments[rb->segment];
    uint64_t v = get64(bytes, seg.fileoff + rb->offset);
    put64(bytes, seg.fileoff + rb->offset, v | (0x5Aull << 36));          // high8 = 0x5A
    Parsed q = parse(bytes);
    auto again = take(dyld::decode_fixups(*q.img, q.slice));
    const dyld::Fixup* rb2 = nullptr;
    for (auto& f : again) if (f.kind == dyld::Fixup::Kind::ChainedRebase && f.offset == rb->offset && f.segment == rb->segment) rb2 = &f;
    REQUIRE(rb2 != nullptr);
    CHECK_EQ(rb2->addend, int64_t{0x5A});
    CHECK_EQ(rb2->target, rb->target);
    // applying it: top byte 0x5A, low bits target + slide
    auto layout = take(loader::plan_layout(*q.img));
    auto image = take(loader::map_segments(layout, q.slice));
    std::vector<uint64_t> values(again.size(), 0);
    REQUIRE(loader::apply_fixups_resolved(image, *q.img, again, values).ok());
    const uint8_t* slot = image.host_address(q.img->segments[rb2->segment].vmaddr) + rb2->offset;
    uint64_t got;
    std::memcpy(&got, slot, 8);
    CHECK_EQ(got >> 56, uint64_t{0x5A});
    CHECK_EQ(got & 0x00FFFFFFFFFFFFFFull, static_cast<uint64_t>(static_cast<int64_t>(rb2->target) + image.slide));
}

// ============================== refusals ============================================================================================
namespace {
guest::Outcome load_result(const std::vector<uint8_t>& bytes) { return run_patched(bytes); }
void expect_refused(const std::vector<uint8_t>& bytes, loader::LoadFailure kind, const char* needle, const char* what) {
    guest::Outcome o = load_result(bytes);
    if (o.state != guest::State::LoadFailed) { report_failure(__FILE__, __LINE__, std::string("accepted: ") + what); return; }
    if (o.load_failure != kind) report_failure(__FILE__, __LINE__, std::string(what) + ": wrong failure kind, message " + o.message);
    if (o.message.find(needle) == std::string::npos) report_failure(__FILE__, __LINE__, std::string(what) + ": message was " + o.message);
}
}  // namespace

CIDER_TEST(unsupported_pointer_formats_are_refused_by_name) {
    const struct { uint16_t fmt; const char* name; } cases[] = {
        {1, "DYLD_CHAINED_PTR_ARM64E"}, {3, "DYLD_CHAINED_PTR_32"}, {4, "DYLD_CHAINED_PTR_32_CACHE"}, {7, "DYLD_CHAINED_PTR_ARM64E_KERNEL"},
        {8, "DYLD_CHAINED_PTR_64_KERNEL_CACHE"}, {9, "DYLD_CHAINED_PTR_ARM64E_USERLAND"}, {11, "DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE"},
        {12, "DYLD_CHAINED_PTR_ARM64E_USERLAND24"}, {0, "unknown"}, {99, "unknown"}};
    for (auto& c : cases) {
        auto bytes = load(kDylib / "bin/chainexe/chainexe");
        Blob b = blob_of(bytes);
        for (int si = 0; si < 4; ++si)
            if (uint64_t rec = b.seg_record(bytes, si)) put16(bytes, rec + 6, c.fmt);
        expect_refused(bytes, loader::LoadFailure::Unsupported, c.name, "pointer format");
    }
}

CIDER_TEST(unsupported_header_versions_and_import_formats_are_refused) {
    auto fresh = [] { return load(kDylib / "bin/chainexe/chainexe"); };
    {
        auto b = fresh(); Blob bl = blob_of(b); put32(b, bl.base + 0, 1);
        expect_refused(b, loader::LoadFailure::Unsupported, "header version 1", "fixups_version 1");
    }
    {
        auto b = fresh(); Blob bl = blob_of(b); put32(b, bl.base + 20, 4);
        expect_refused(b, loader::LoadFailure::Unsupported, "imports format 4", "imports_format 4");
    }
    {
        auto b = fresh(); Blob bl = blob_of(b); put32(b, bl.base + 24, 1);
        expect_refused(b, loader::LoadFailure::Unsupported, "symbol pool format 1", "symbols_format 1");
    }
    {   // page size 0x2000 is not one of the two real page sizes
        auto b = fresh(); Blob bl = blob_of(b);
        for (int si = 0; si < 4; ++si) if (uint64_t rec = bl.seg_record(b, si)) put16(b, rec + 4, 0x2000);
        expect_refused(b, loader::LoadFailure::Unsupported, "page size", "page size 0x2000");
    }
    {   // a page flagged DYLD_CHAINED_PTR_START_MULTI
        auto b = fresh(); Blob bl = blob_of(b);
        for (int si = 0; si < 4; ++si) if (uint64_t rec = bl.seg_record(b, si)) put16(b, rec + 22, 0x8000);
        expect_refused(b, loader::LoadFailure::Unsupported, "START_MULTI", "multi start");
    }
}

CIDER_TEST(malformed_chains_are_refused_without_touching_memory_outside_the_image) {
    auto fresh = [] { return load(kDylib / "bin/chainexe/chainexe"); };
    auto first_chain_segment = [](const std::vector<uint8_t>& b, const Blob& bl) { for (int si = 0; si < 4; ++si) if (bl.seg_record(b, si)) return si; return -1; };
    {   // bind with an import index past the table: patch the first bind's ordinal to 99
        auto b = fresh();
        Parsed p = parse(b);
        auto fx = take(dyld::decode_fixups(*p.img, p.slice));
        bool done = false;
        for (auto& f : fx) if (f.kind == dyld::Fixup::Kind::Bind) {
            const uint64_t at = p.img->segments[f.segment].fileoff + f.offset;
            put64(b, at, (get64(b, at) & ~0xFFFFFFull) | 99);
            done = true;
            break;
        }
        REQUIRE(done);
        expect_refused(b, loader::LoadFailure::Incompatible, "import 99", "ordinal 99");
    }
    {   // a chain that jumps out of its page: next = 0xFFF (16380 bytes)
        auto b = fresh();
        Parsed p = parse(b);
        auto fx = take(dyld::decode_fixups(*p.img, p.slice));
        REQUIRE(!fx.empty());
        const auto& f = fx.front();
        const uint64_t at = p.img->segments[f.segment].fileoff + f.offset;
        put64(b, at, (get64(b, at) & ~(0xFFFull << 51)) | (0xFFFull << 51));
        expect_refused(b, loader::LoadFailure::Incompatible, "leaves its page", "next = 0xFFF");
    }
    {   // chain start beyond the file-backed part of the segment
        auto b = fresh(); Blob bl = blob_of(b);
        const int si = first_chain_segment(b, bl);
        REQUIRE(si >= 0);
        put16(b, bl.seg_record(b, si) + 22, 0x0FFC);
        guest::Outcome o = load_result(b);
        CHECK(o.state == guest::State::LoadFailed);
    }
    {   // segment_offset that does not match the segment address
        auto b = fresh(); Blob bl = blob_of(b);
        const int si = first_chain_segment(b, bl);
        REQUIRE(si >= 0);
        put64(b, bl.seg_record(b, si) + 8, 0x7000);
        expect_refused(b, loader::LoadFailure::Incompatible, "does not match its address", "segment_offset");
    }
    {   // more pages than the segment has
        auto b = fresh(); Blob bl = blob_of(b);
        const int si = first_chain_segment(b, bl);
        put16(b, bl.seg_record(b, si) + 20, 200);
        guest::Outcome o = load_result(b);
        CHECK(o.state == guest::State::LoadFailed);
    }
    {   // starts_offset past the end of the data
        auto b = fresh(); Blob bl = blob_of(b); put32(b, bl.base + 4, 0x7fffffff);
        expect_refused(b, loader::LoadFailure::Incompatible, "starts_in_image", "starts_offset");
    }
    {   // seg_count larger than the image
        auto b = fresh(); Blob bl = blob_of(b); put32(b, bl.starts, 500);
        expect_refused(b, loader::LoadFailure::Incompatible, "lists 500 segments", "seg_count");
    }
    {   // imports table running off the data
        auto b = fresh(); Blob bl = blob_of(b); put32(b, bl.base + 16, 0x00ffffff);
        guest::Outcome o = load_result(b);
        CHECK(o.state == guest::State::LoadFailed);
    }
    {   // symbol name offset outside the pool
        auto b = fresh(); Blob bl = blob_of(b);
        uint32_t imp; std::memcpy(&imp, b.data() + bl.imports, 4);
        put32(b, bl.imports, (imp & 0x1FF) | (0x7fffffu << 9));
        expect_refused(b, loader::LoadFailure::Incompatible, "unterminated or out-of-range name", "name offset");
    }
    {   // truncated header
        auto b = fresh(); Parsed p = parse(b);
        const uint64_t cmd = [&] {   // find the LC_DYLD_CHAINED_FIXUPS load command and shrink its datasize to 8
            uint64_t off = 32;
            uint32_t ncmds; std::memcpy(&ncmds, b.data() + 16, 4);
            for (uint32_t i = 0; i < ncmds; ++i) {
                uint32_t cmdid, size; std::memcpy(&cmdid, b.data() + off, 4); std::memcpy(&size, b.data() + off + 4, 4);
                if (cmdid == 0x80000034) return off;
                off += size;
            }
            return uint64_t{0};
        }();
        REQUIRE(cmd != 0);
        put32(b, cmd + 12, 8);
        guest::Outcome o = load_result(b);
        CHECK(o.state == guest::State::LoadFailed);
        (void)p;
    }
}

CIDER_TEST(an_image_with_classic_and_chained_fixups_is_refused) {
    // The hello_chained fixture plus a classic LC_DYLD_INFO_ONLY would be ambiguous; the decoder refuses it.
    auto bytes = load(kReal / "bin/hello_chained_x86_64");
    Parsed p = parse(bytes);
    macho::Image img = *p.img;
    img.dyld_info = macho::DyldInfo{};
    auto r = dyld::decode_fixups(img, p.slice);
    CHECK(!r.ok());
}

CIDER_TEST(mutated_chained_images_never_crash_the_decoder_or_the_linker) {
    const auto exe = load(kDylib / "bin/chained/chained");
    const auto lib = load(kDylib / "bin/chained/libchain.dylib");
    Blob eb = blob_of(exe);
    std::mt19937 rng(0xC4A1D);
    size_t refused = 0;
    for (int i = 0; i < 1500; ++i) {
        auto m = (i % 2) ? exe : lib;
        const Blob b = (i % 2) ? eb : blob_of(lib);
        const uint64_t lo = b.base, hi = std::min<uint64_t>(m.size(), b.base + 0x180);   // concentrate on the chained-fixups blob and the data pages
        for (int k = 0, n = 1 + static_cast<int>(rng() % 4); k < n; ++k) {
            const uint64_t pos = (rng() % 3 == 0) ? (4096 + rng() % 0x100) : lo + rng() % (hi - lo);
            if (pos < m.size()) m[pos] = static_cast<uint8_t>(rng());
        }
        // link-only: never executes guest code
        auto prov = take(libsystem::LibSystemProvider::create());
        linker::LinkError err;
        TempDir t;
        t.write("chained", (i % 2) ? m : exe);
        t.write("libchain.dylib", (i % 2) ? lib : m);
        std::vector<uint8_t> mainbytes = (i % 2) ? m : exe;
        linker::LinkOptions opts;
        opts.executable_path = t.path() / "chained";
        auto prog = linker::Program::link(ByteView(mainbytes), opts, *prov, err);
        if (!prog) ++refused;
    }
    CHECK(refused > 0);
}
