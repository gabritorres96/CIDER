// Mach-O loader: pre-execution validation, layout planning, segment mapping, memory permissions and
// cleanup. These tests LOAD images; they never transfer control to guest code (see test_execution.cpp).
#include <algorithm>
#include <cstring>
#include <fstream>

#include "cider/libsystem.hpp"
#include "cider/loader.hpp"
#include "guest_code.hpp"
#include "macho_builder.hpp"
#include "objdump_compare.hpp"
#include "test_framework.hpp"

using namespace cider;
using namespace cider::loader;
using namespace cider::testing;

namespace {

macho::Segment seg(const char* name, uint64_t vmaddr, uint64_t vmsize, uint64_t filesize, int32_t prot, uint64_t fileoff = 0) {
    macho::Segment s;
    s.name = name;
    s.vmaddr = vmaddr; s.vmsize = vmsize; s.fileoff = fileoff; s.filesize = filesize;
    s.initprot = prot; s.maxprot = prot;
    return s;
}

// A valid minimal x86_64 executable image description (no file behind it).
macho::Image synthetic() {
    macho::Image i;
    i.cputype = macho::kCpuX86_64;
    i.filetype = macho::kFileExecute;
    i.segments = {seg("__PAGEZERO", 0, 0x100000000ull, 0, 0), seg("__TEXT", 0x100000000ull, 0x1000, 0x1000, 5),
                  seg("__DATA", 0x100001000ull, 0x1000, 0x1000, 3, 0x1000), seg("__LINKEDIT", 0x100002000ull, 0x1000, 0x100, 1, 0x2000)};
    macho::EntryPoint e;
    e.kind = macho::EntryPoint::Kind::Main;
    e.vmaddr = 0x100000f00ull;
    e.file_offset = 0xf00;
    i.entry = e;
    macho::Dylib d;
    d.path = "/usr/lib/libSystem.B.dylib";
    i.dylibs.push_back(d);
    return i;
}

struct Fixture {
    std::vector<uint8_t> bytes;
    std::unique_ptr<libsystem::LibSystemProvider> provider;
};

Fixture fixture(const std::string& name) {
    Fixture f;
    f.bytes = exec_fixture(name);
    f.provider = take(libsystem::LibSystemProvider::create());
    return f;
}

LoadResult load(Fixture& f) { return load_macho(ByteView(f.bytes), *f.provider); }

bool range_free(uint64_t addr) { return memory::query_address(reinterpret_cast<void*>(addr)).state == memory::PageInfo::State::Free; }

}  // namespace

// ---- layout planning ---------------------------------------------------------------------------------
CIDER_TEST(layout_of_a_valid_image) {
    auto lay = plan_layout(synthetic());
    REQUIRE(lay.ok());
    const auto& l = lay.value();
    CHECK_EQ(l.preferred_base, uint64_t{0x100000000});
    CHECK_EQ(l.span, uint64_t{0x2000});                       // __PAGEZERO and __LINKEDIT are not mapped
    REQUIRE_EQ(l.segments.size(), size_t{2});
    CHECK_EQ(l.segments[0].name, std::string("__TEXT"));
    CHECK(l.segments[0].final_prot == memory::Prot::ReadExecute);
    CHECK(l.segments[1].final_prot == memory::Prot::ReadWrite);
    CHECK_EQ(l.entry_vmaddr, uint64_t{0x100000f00});
    CHECK_EQ(l.stack_size, kDefaultStackSize);
}

CIDER_TEST(layout_uses_lc_main_stacksize_within_limits) {
    auto i = synthetic();
    i.entry->stack_size = 1 << 20;
    CHECK_EQ(plan_layout(i).value().stack_size, uint64_t{1 << 20});
    i.entry->stack_size = 100;                                  // tiny requests are raised to 64 KiB
    CHECK_EQ(plan_layout(i).value().stack_size, uint64_t{64 * 1024});
    i.entry->stack_size = 1ull << 40;
    CHECK_ERR(plan_layout(i), ErrorCode::LimitExceeded);
}

CIDER_TEST(layout_data_const_becomes_read_only_after_fixups) {
    auto i = synthetic();
    i.segments.insert(i.segments.begin() + 2, seg("__DATA_CONST", 0x100001000ull, 0x1000, 0x1000, 3));
    i.segments[3].vmaddr = 0x100002000ull;                       // __DATA moves up
    i.segments[4].vmaddr = 0x100003000ull;
    auto lay = plan_layout(i).value();
    const SegmentPlan* dc = nullptr;
    for (auto& s : lay.segments) if (s.name == "__DATA_CONST") dc = &s;
    REQUIRE(dc != nullptr);
    CHECK(dc->read_only_after_fixups);
    CHECK(dc->final_prot == memory::Prot::Read);
}

CIDER_TEST(layout_rejects_rwx_and_unrepresentable_permissions) {
    auto i = synthetic();
    i.segments[1].initprot = 7;
    CHECK_ERR(plan_layout(i), ErrorCode::Unsupported);          // W^X
    i = synthetic(); i.segments[2].initprot = 6;
    CHECK_ERR(plan_layout(i), ErrorCode::Unsupported);
    i = synthetic(); i.segments[1].initprot = 0x10;
    CHECK_ERR(plan_layout(i), ErrorCode::Malformed);
    i = synthetic(); i.segments[0].initprot = 1;                // __PAGEZERO must be inaccessible
    CHECK_ERR(plan_layout(i), ErrorCode::Malformed);
}

CIDER_TEST(layout_rejects_bad_geometry) {
    auto i = synthetic();
    i.segments[1].vmaddr = 0x100000010ull;                      // not page aligned
    CHECK_ERR(plan_layout(i), ErrorCode::Malformed);
    i = synthetic(); i.segments[1].vmaddr = UINT64_MAX - 0xfff; i.segments[1].vmsize = 0x2000;
    CHECK_ERR(plan_layout(i), ErrorCode::Overflow);             // address space wraps
    i = synthetic(); i.segments[1].vmsize = UINT64_MAX - 5;
    CHECK_ERR(plan_layout(i), ErrorCode::Overflow);             // rounding up overflows
    i = synthetic(); i.segments[1].filesize = 0x2000;
    CHECK_ERR(plan_layout(i), ErrorCode::Malformed);            // more file data than memory
    i = synthetic(); i.segments[1].vmsize = 0x1800;             // rounds to 0x2000 and then overlaps __DATA
    CHECK_ERR(plan_layout(i), ErrorCode::Overlap);
    i = synthetic(); i.segments[2].vmsize = 4ull << 30;
    CHECK_ERR(plan_layout(i), ErrorCode::LimitExceeded);        // span limit
    i = synthetic();
    i.segments[1].vmaddr = 0x100010000ull; i.segments[2].vmaddr = 0x100011000ull; i.segments[3].vmaddr = 0x100012000ull; i.entry->vmaddr = 0x100010f00ull;
    CHECK(plan_layout(i).ok());                                  // 64 KiB aligned base: fine
    i.segments[1].vmaddr = 0x100001000ull; i.segments[2].vmaddr = 0x100002000ull; i.segments[3].vmaddr = 0x100003000ull; i.entry->vmaddr = 0x100001f00ull;
    CHECK_ERR(plan_layout(i), ErrorCode::Unsupported);          // base not 64 KiB aligned
}

CIDER_TEST(layout_rejects_bad_entry_points) {
    auto i = synthetic();
    i.entry->vmaddr = 0x200000000ull;
    CHECK_ERR(plan_layout(i), ErrorCode::Malformed);            // outside every segment
    i = synthetic(); i.entry->vmaddr = 0x100001100ull;
    CHECK_ERR(plan_layout(i), ErrorCode::Malformed);            // inside __DATA: not executable
    i = synthetic(); i.entry->vmaddr = 0x100002100ull;
    CHECK_ERR(plan_layout(i), ErrorCode::Malformed);            // __LINKEDIT is not mapped
    i = synthetic(); i.entry->kind = macho::EntryPoint::Kind::UnixThread;
    CHECK_ERR(plan_layout(i), ErrorCode::Unsupported);
    i = synthetic(); i.entry.reset();
    CHECK_ERR(plan_layout(i), ErrorCode::Unsupported);
    i = synthetic(); i.segments = {seg("__PAGEZERO", 0, 0x100000000ull, 0, 0)};
    CHECK_ERR(plan_layout(i), ErrorCode::Malformed);            // nothing to map
}

// ---- validation ---------------------------------------------------------------------------------------
CIDER_TEST(validator_accepts_the_real_fixtures) {
    for (const char* n : {"ret42", "math", "calls", "globals", "bss", "libcall", "hello", "args", "missing"}) {
        auto bytes = exec_fixture(n);
        auto file = macho::parse_file(ByteView(bytes)).value();
        auto rep = validate_for_execution(*file.slices[0].image);
        if (!rep.ok()) report_failure(__FILE__, __LINE__, std::string(n) + ": " + rep.issues[0].message);
        CHECK_EQ(rep.required_libs.size(), size_t{1});
    }
}

CIDER_TEST(validator_rejects_wrong_architecture_type_platform_and_encryption) {
    auto i = synthetic();
    i.cputype = macho::kCpuArm64;
    CHECK(!validate_for_execution(i).ok());
    i = synthetic(); i.filetype = macho::kFileDylib;
    CHECK(!validate_for_execution(i).ok());
    i = synthetic(); i.build_versions.push_back({2, 0, 0, {}, false});         // iOS
    CHECK(!validate_for_execution(i).ok());
    i = synthetic(); i.build_versions.push_back({1, 0, 0, {}, false});         // macOS: fine
    CHECK(validate_for_execution(i).ok());
    i = synthetic(); i.encrypted = true;
    CHECK(!validate_for_execution(i).ok());
    i = synthetic(); i.entry.reset();
    CHECK(!validate_for_execution(i).ok());
}

CIDER_TEST(validator_reports_missing_dependencies_precisely) {
    auto i = synthetic();
    macho::Dylib d;
    d.path = "@rpath/libfoo.dylib";
    i.dylibs.push_back(d);
    auto rep = validate_for_execution(i);
    REQUIRE(!rep.ok());
    CHECK(rep.issues[0].kind == Issue::Kind::MissingDependency);
    CHECK(rep.issues[0].message.find("libfoo.dylib") != std::string::npos);
    CHECK_EQ(rep.required_libs.size(), size_t{2});
    i = synthetic(); d.kind = macho::DylibKind::Weak; i.dylibs.push_back(d);      // weak: tolerated (symbols become null)
    CHECK(validate_for_execution(i).ok());
}

CIDER_TEST(validator_rejects_features_the_runtime_cannot_honour) {
    auto i = synthetic();
    i.chained_fixups = macho::LinkeditRange{0x1000, 16};
    auto rep = validate_for_execution(i);
    REQUIRE(!rep.ok());
    CHECK(rep.issues[0].kind == Issue::Kind::Unsupported);
    i = synthetic();
    macho::Section tls;
    tls.name = "__thread_vars"; tls.segment = "__DATA"; tls.flags = 0x13; tls.size = 24;
    i.segments[2].sections.push_back(tls);
    CHECK(!validate_for_execution(i).ok());
    i = synthetic();
    macho::Section init;
    init.name = "__mod_init_func"; init.segment = "__DATA"; init.flags = 0x9; init.size = 8;
    i.segments[2].sections.push_back(init);
    CHECK(!validate_for_execution(i).ok());
    i.segments[2].sections[0].size = 0;                                            // empty initializer section is harmless
    CHECK(validate_for_execution(i).ok());
    i = synthetic();
    i.dyld_info = macho::DyldInfo{};
    i.dyld_info->weak_bind.size = 8;
    CHECK(!validate_for_execution(i).ok());
}

CIDER_TEST(real_chained_fixups_binary_is_refused_with_a_clear_message) {
    auto bytes = read_file(std::filesystem::path(CIDER_REAL_FIXTURES_DIR) / "bin" / "hello_chained_x86_64", 1 << 20).value();
    auto provider = take(libsystem::LibSystemProvider::create());
    auto r = load_macho(ByteView(bytes), *provider);
    REQUIRE(!r.ok());
    CHECK(r.error->kind == LoadFailure::Unsupported);
    CHECK(r.error->message.find("chained fixups") != std::string::npos);
}

// ---- mapping ------------------------------------------------------------------------------------------------
CIDER_TEST(mapped_segments_contain_the_bytes_of_the_file) {
    auto f = fixture("globals");
    auto r = load(f);
    REQUIRE(r.ok());
    LoadedImage& img = *r.image;
    // the Mach-O header sits at the start of the mapped __TEXT segment
    uint32_t magic;
    std::memcpy(&magic, img.base(), 4);
    CHECK_EQ(magic, 0xfeedfacfu);
    // the entry point's machine code in memory is byte-identical to the file contents at LC_MAIN's offset
    auto parsed = macho::parse_file(ByteView(f.bytes)).value();
    const auto& e = *parsed.slices[0].image->entry;
    CHECK(std::memcmp(img.host_address(img.layout.entry_vmaddr), f.bytes.data() + e.file_offset, 64) == 0);
    CHECK_EQ(img.entry_address(), reinterpret_cast<uint64_t>(img.host_address(img.layout.entry_vmaddr)));
    // initialised data is copied from the file (g_counter = 5 lives in __DATA)
    bool found5 = false;
    for (auto& s : img.layout.segments)
        if (s.name == "__DATA")
            for (uint64_t o = 0; o + 4 <= s.filesize; o += 4) {
                int32_t v; std::memcpy(&v, img.host_address(s.vmaddr) + o, 4);
                found5 |= v == 5;
            }
    CHECK(found5);
}

CIDER_TEST(zero_fill_memory_is_zero_and_writable) {
    auto f = fixture("bss");
    auto r = load(f);
    REQUIRE(r.ok());
    LoadedImage& img = *r.image;
    const SegmentPlan* data = nullptr;
    for (auto& s : img.layout.segments) if (s.name == "__DATA") data = &s;
    REQUIRE(data != nullptr);
    CHECK_EQ(data->filesize, uint64_t{0});                      // pure zero-fill segment
    const uint8_t* p = img.host_address(data->vmaddr);
    bool all_zero = true;
    for (uint64_t i = 0; i < data->vmsize; ++i) all_zero &= p[i] == 0;
    CHECK(all_zero);
    auto info = memory::query_address(p);
    CHECK(info.writable && !info.executable);
    const_cast<uint8_t*>(p)[100] = 0x7f;                       // writable: must not fault
}

CIDER_TEST(final_permissions_follow_w_xor_x_for_every_page) {
    for (const char* name : {"calls", "libcall", "hello", "bss"}) {
        auto f = fixture(name);
        auto r = load(f);
        REQUIRE(r.ok());
        LoadedImage& img = *r.image;
        for (auto& s : img.layout.segments) {
            for (uint64_t off = 0; off < s.vmsize; off += 4096) {
                auto pi = memory::query_address(img.host_address(s.vmaddr) + off);
                CHECK(pi.state == memory::PageInfo::State::Committed);
                CHECK(!(pi.writable && pi.executable));               // never RWX
                if (s.name == "__TEXT") CHECK(pi.executable && !pi.writable);
                else if (s.name == "__DATA_CONST") CHECK(!pi.writable && !pi.executable);
                else if (s.name == "__DATA") CHECK(pi.writable && !pi.executable);
            }
        }
        // gaps between segments (and the span as a whole) are reserved, not committed RWX
        for (uint64_t off = 0; off < img.layout.span; off += 4096) {
            auto pi = memory::query_address(img.base() + off);
            CHECK(!(pi.writable && pi.executable));
        }
    }
}

CIDER_TEST(fixups_are_applied_rebases_and_binds) {
    auto f = fixture("calls");                                   // g_fn = &twice lives in __DATA: one rebase
    auto r = load(f);
    REQUIRE(r.ok());
    CHECK_EQ(r.report.rebases, size_t{1});
    LoadedImage& img = *r.image;
    // find the pointer in __DATA: it must point into __TEXT of THIS mapping
    const SegmentPlan* data = nullptr;
    for (auto& s : img.layout.segments) if (s.name == "__DATA") data = &s;
    uint64_t v;
    std::memcpy(&v, img.host_address(data->vmaddr), 8);
    CHECK(v >= img.actual_base() && v < img.actual_base() + 0x1000);

    auto f2 = fixture("libcall");                                // imports: bound to System V thunks, not to host functions
    auto r2 = load(f2);
    REQUIRE(r2.ok());
    CHECK_EQ(r2.report.binds, size_t{5});
    CHECK_EQ(r2.report.required_libs.size(), size_t{1});
}

CIDER_TEST(lazy_binding_stubs_are_resolved_eagerly) {
    auto f = fixture("hello");
    auto r = load(f);
    REQUIRE(r.ok());
    LoadedImage& img = *r.image;
    const SegmentPlan* data = nullptr;
    for (auto& s : img.layout.segments) if (s.name == "__DATA") data = &s;
    REQUIRE(data != nullptr);
    uint64_t slot0;
    std::memcpy(&slot0, img.host_address(data->vmaddr), 8);      // first __la_symbol_ptr entry
    // not an address inside the image (that would be the stub helper): it points at an executable thunk
    CHECK(!(slot0 >= img.actual_base() && slot0 < img.actual_base() + img.layout.span));
    CHECK(memory::query_address(reinterpret_cast<void*>(slot0)).executable);
}

// ---- failures and cleanup -----------------------------------------------------------------------------------------
CIDER_TEST(garbage_and_wrong_files_fail_before_any_memory_is_reserved) {
    auto provider = take(libsystem::LibSystemProvider::create());
    std::vector<uint8_t> junk(1000, 0x41);
    auto r = load_macho(ByteView(junk), *provider);
    REQUIRE(!r.ok());
    CHECK(r.error->kind == LoadFailure::Incompatible);
    CHECK(range_free(0x100000000ull));
    CHECK(!load_macho(ByteView(), *provider).ok());

    auto arm = read_file(std::filesystem::path(CIDER_REAL_FIXTURES_DIR) / "bin" / "hello_arm64", 1 << 20).value();
    auto r2 = load_macho(ByteView(arm), *provider);
    REQUIRE(!r2.ok());
    CHECK(r2.error->message.find("no x86_64 slice") != std::string::npos);

    auto dylib = read_file(std::filesystem::path(CIDER_REAL_FIXTURES_DIR) / "bin" / "libfoo.dylib", 1 << 20).value();
    auto r3 = load_macho(ByteView(dylib), *provider);
    REQUIRE(!r3.ok());
    CHECK(r3.error->kind == LoadFailure::Incompatible);
    CHECK(range_free(0x100000000ull));
}

CIDER_TEST(unresolved_symbols_are_reported_and_nothing_stays_mapped) {
    auto f = fixture("missing");
    auto r = load(f);
    REQUIRE(!r.ok());
    CHECK(r.error->kind == LoadFailure::UnresolvedSymbol);
    REQUIRE_EQ(r.error->report.unresolved.size(), size_t{1});
    CHECK_EQ(r.error->report.unresolved[0].symbol, std::string("_fork"));
    CHECK_EQ(r.error->report.unresolved[0].library, std::string("/usr/lib/libSystem.B.dylib"));
    CHECK(range_free(0x100000000ull));          // resolution happens before mapping
}

CIDER_TEST(memory_is_released_when_a_loaded_image_goes_out_of_scope) {
    uint64_t base;
    {
        auto f = fixture("hello");
        auto r = load(f);
        REQUIRE(r.ok());
        base = r.image->actual_base();
        CHECK(!range_free(base));
    }
    CHECK(range_free(base));
}

CIDER_TEST(synthetic_builder_image_loads_and_validates_end_to_end) {
    auto built = build_macho({});
    auto provider = take(libsystem::LibSystemProvider::create());
    auto r = load_macho(ByteView(built.bytes), *provider);
    REQUIRE(r.ok());
    CHECK_EQ(r.image->layout.segments.size(), size_t{1});         // only __TEXT: no data segment in this builder
    // patching the segment protection to RWX makes the same file unloadable
    auto b2 = built.bytes;
    patch32(b2, built.cmd["text"] + 60, 7);
    auto r2 = load_macho(ByteView(b2), *provider);
    REQUIRE(!r2.ok());
    CHECK(r2.error->kind == LoadFailure::Unsupported);
    CHECK(r2.error->message.find("RWX") != std::string::npos);
}

CIDER_TEST(mutated_executables_either_load_cleanly_or_fail_cleanly) {
    // Loading (not executing) mutated copies of real fixtures: no crash, no leaked reservation.
    auto provider = take(libsystem::LibSystemProvider::create());
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    auto rnd = [&] { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
    size_t loaded = 0, rejected = 0;
    for (const char* name : {"hello", "calls", "bss", "libcall"}) {
        auto good = exec_fixture(name);
        for (int iter = 0; iter < 400; ++iter) {
            auto b = good;
            for (int e = 0, n = 1 + static_cast<int>(rnd() % 3); e < n; ++e) b[rnd() % std::min<size_t>(b.size(), 2048)] = static_cast<uint8_t>(rnd());
            auto r = load_macho(ByteView(b), *provider);
            (r.ok() ? loaded : rejected)++;
            // execution is NOT attempted: a mutated binary may legitimately contain bad code
        }
    }
    CHECK(rejected > 0);
    CHECK(range_free(0x100000000ull));
    (void)loaded;
}

// ---- the fixtures themselves are real Mach-O: CIDER's parse equals llvm-objdump's ----------------------------------------------------------
CIDER_TEST(execution_fixtures_metadata_matches_llvm_objdump_snapshots) {
    auto slurp = [](const std::filesystem::path& p) {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    for (const char* n : {"ret42", "ret7", "ret99", "math", "stack", "calls", "globals", "bss", "libcall", "hello", "args", "missing", "exitcall",
                          "crash", "spin", "malloc", "fmath", "recurse", "hog", "pid"}) {
        auto bytes = exec_fixture(n);
        macho::ParseOptions po;
        po.parse_symbols = true;
        auto file = macho::parse_file(ByteView(bytes), po);
        if (!file.ok() || !file.value().slices[0].image) { report_failure(__FILE__, __LINE__, std::string(n) + ": parse failed"); continue; }
        const auto& img = *file.value().slices[0].image;
        objdump::compare_image(n, objdump::parse_private_headers(slurp(kExecFixtures / "golden" / (std::string(n) + ".private-headers.txt"))), img);
        objdump::compare_symbols(n, slurp(kExecFixtures / "golden" / (std::string(n) + ".syms.txt")), img);
        CHECK(img.filetype == macho::kFileExecute);
        CHECK(img.cputype == macho::kCpuX86_64);
    }
}

CIDER_TEST(execution_fixtures_machine_code_matches_the_llvm_objdump_disassembly) {
    // ret42's main, as disassembled by llvm-objdump when the fixture was built, is exactly the code CIDER maps and runs.
    std::ifstream f(kExecFixtures / "golden" / "ret42.disasm.txt");
    std::stringstream ss;
    ss << f.rdbuf();
    std::string d = ss.str();
    CHECK(d.find("b8 2a 00 00 00") != std::string::npos);      // movl $0x2a, %eax
    CHECK(d.find("movl\t$0x2a, %eax") != std::string::npos);
    auto fx = fixture("ret42");
    auto r = load(fx);
    REQUIRE(r.ok());
    const uint8_t* main_code = r.image->host_address(r.image->layout.entry_vmaddr);
    const uint8_t expect[] = {0x55, 0x48, 0x89, 0xE5, 0xB8, 0x2A, 0x00, 0x00, 0x00, 0x5D, 0xC3};    // push rbp; mov rbp,rsp; mov eax,42; pop rbp; ret
    CHECK(std::memcmp(main_code, expect, sizeof expect) == 0);
}
