// CIDER 0.3 dynamic linker: export tries, dependency search, loading several real Mach-O images, symbol
// resolution between them, re-exports, cycles, initializers, failures and unloading.
// Fixtures: tests/fixtures/dylib (clang + ld64.lld; gen-dylib-fixtures.cmd).
#include <algorithm>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

#include "cider/dyld.hpp"
#include "cider/exports.hpp"
#include "cider/guest.hpp"
#include "cider/linker.hpp"
#include "guest_code.hpp"
#include "macho_builder.hpp"
#include "test_framework.hpp"

using namespace cider;
using namespace cider::testing;
namespace fs = std::filesystem;

namespace {

const fs::path kDylibFixtures = fs::path(CIDER_DYLIB_FIXTURES_DIR);

fs::path dy(const std::string& scenario, const std::string& rel = {}) {
    return kDylibFixtures / "bin" / scenario / (rel.empty() ? scenario : rel);
}

std::vector<uint8_t> file_bytes(const fs::path& p) {
    auto r = read_file(p, 64u << 20);
    if (!r.ok()) { report_failure(__FILE__, __LINE__, "fixture missing: " + path_to_utf8(p)); throw AbortTest{}; }
    return r.value();
}

struct Linked {
    std::unique_ptr<libsystem::LibSystemProvider> provider;
    std::unique_ptr<linker::Program> program;
    linker::LinkError error;
    std::vector<uint8_t> main_bytes;
};

Linked link_path(const fs::path& exe, const std::function<void(linker::LinkOptions&)>& tweak = {}) {
    Linked l;
    l.provider = take(libsystem::LibSystemProvider::create());
    l.main_bytes = file_bytes(exe);
    linker::LinkOptions opts;
    opts.executable_path = exe;
    if (tweak) tweak(opts);
    l.program = linker::Program::link(ByteView(l.main_bytes), opts, *l.provider, l.error);
    return l;
}

Linked link_scenario(const std::string& s, const std::function<void(linker::LinkOptions&)>& tweak = {}) { return link_path(dy(s), tweak); }

struct Run {
    guest::Outcome r;
    std::string out;
};

Run run_path(const fs::path& exe, bool reject_cycles = false) {
    Run run;
    guest::GuestConfig cfg;
    cfg.args = {path_to_utf8(exe)};
    cfg.image_path = exe;
    cfg.reject_cycles = reject_cycles;
    cfg.sink = [&](int, const char* d, size_t n) { run.out.append(d, n); };
    auto bytes = file_bytes(exe);
    run.r = guest::run_in_process(ByteView(bytes), cfg);
    return run;
}

Run run_scenario(const std::string& s) { return run_path(dy(s)); }

void expect_exit(const std::string& scenario, int code) {
    Run r = run_scenario(scenario);
    if (r.r.state != guest::State::Exited) { report_failure(__FILE__, __LINE__, scenario + ": state " + guest::to_string(r.r.state) + " " + r.r.message); return; }
    if (r.r.exit_code != code) report_failure(__FILE__, __LINE__, scenario + ": expected exit code " + std::to_string(code) + ", got " + std::to_string(r.r.exit_code));
}

bool is_free(uint64_t a) { return memory::query_address(reinterpret_cast<void*>(a)).state == memory::PageInfo::State::Free; }

// ULEB128 helper for hand-made tries.
void uleb(std::vector<uint8_t>& v, uint64_t x) {
    do { uint8_t b = x & 0x7f; x >>= 7; if (x) b |= 0x80; v.push_back(b); } while (x);
}

// "0x00001000  _name" lines of llvm-objdump --exports-trie
std::map<std::string, uint64_t> golden_exports(const std::string& key) {
    std::map<std::string, uint64_t> out;
    std::ifstream f(kDylibFixtures / "golden" / (key + ".exports.txt"));
    for (std::string line; std::getline(f, line);) {
        if (line.rfind("0x", 0) != 0) continue;
        std::istringstream ls(line);
        std::string addr, name;
        ls >> addr >> name;
        out[name] = std::stoull(addr, nullptr, 16);
    }
    return out;
}

}  // namespace

// ============================== export trie ==============================================================================
CIDER_TEST(export_trie_matches_llvm_objdump_for_real_dylibs) {
    struct Case { const char* scenario; const char* file; const char* key; };
    const Case cases[] = {{"one", "libmath.dylib", "one_libmath.dylib"},       {"init", "libinitbase.dylib", "init_libinitbase.dylib"},
                          {"glob", "libglob.dylib", "glob_libglob.dylib"},     {"trans", "libtransa.dylib", "trans_libtransa.dylib"},
                          {"one", "one", "one_one"},                           {"reexport", "libfacade.dylib", "reexport_libfacade.dylib"}};
    for (auto& c : cases) {
        auto bytes = file_bytes(dy(c.scenario, c.file));
        auto parsed = take(macho::parse_file(ByteView(bytes)));
        const auto* sl = parsed.find_slice(macho::kCpuX86_64);
        REQUIRE(sl && sl->image);
        auto slice = take(ByteView(bytes).slice(sl->offset, sl->size));
        auto ex = take(dyld::decode_exports(*sl->image, slice));
        auto gold = golden_exports(c.key);
        // llvm-objdump prints header-relative offsets plus the preferred load address of the __TEXT segment.
        uint64_t text_base = 0;
        for (auto& seg : sl->image->segments) if (seg.name == "__TEXT") text_base = seg.vmaddr;
        std::map<std::string, uint64_t> mine;
        for (auto& e : ex) mine[e.name] = e.address + text_base;
        if (mine != gold) report_failure(__FILE__, __LINE__, std::string("export list differs from llvm-objdump for ") + c.key);
    }
}

CIDER_TEST(export_trie_hand_made_valid_trie) {
    std::vector<uint8_t> t = {0, 1, '_', 'a', 0, 6, 2, 0, 0x10, 0};
    auto ex = take(dyld::parse_export_trie(ByteView(t)));
    REQUIRE_EQ(ex.size(), size_t{1});
    CHECK_EQ(ex[0].name, std::string("_a"));
    CHECK_EQ(ex[0].address, uint64_t{0x10});
    CHECK(!ex[0].is_reexport());
    CHECK(take(dyld::parse_export_trie(ByteView())).empty());
}

CIDER_TEST(export_trie_reexport_entries_are_decoded) {
    // root -> "_r" -> terminal: flags REEXPORT, ordinal 2, import name "_real"
    std::vector<uint8_t> t = {0, 1, '_', 'r', 0, 6};
    std::vector<uint8_t> term;
    uleb(term, dyld::kExportReexport);
    uleb(term, 2);
    for (char c : std::string("_real")) term.push_back(static_cast<uint8_t>(c));
    term.push_back(0);
    t.push_back(static_cast<uint8_t>(term.size()));
    t.insert(t.end(), term.begin(), term.end());
    t.push_back(0);
    auto ex = take(dyld::parse_export_trie(ByteView(t)));
    REQUIRE_EQ(ex.size(), size_t{1});
    CHECK(ex[0].is_reexport());
    CHECK_EQ(ex[0].reexport_ordinal, uint64_t{2});
    CHECK_EQ(ex[0].reexport_name, std::string("_real"));
}

CIDER_TEST(export_trie_rejects_malformed_input) {
    struct Case { const char* what; std::vector<uint8_t> trie; };
    std::vector<Case> cases = {
        {"child offset outside the trie", {0, 1, '_', 'a', 0, 0x40}},
        {"child points back at the root (cycle)", {0, 1, '_', 'a', 0, 0}},
        {"two edges to the same node", {0, 2, 'a', 0, 9, 'b', 0, 9, 0, 1, 0, 0}},
        {"unterminated edge label", {0, 1, '_', 'a'}},
        {"missing child count", {0}},
        {"terminal size past the end", {0x7f, 0}},
        {"terminal larger than declared", {0, 1, 'a', 0, 6, 1, 0, 0x10, 0}},
        {"ULEB128 never ends", {0xff, 0xff, 0xff}},
        {"ULEB128 wider than 64 bits", {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f}},
        {"offset ULEB truncated", {0, 1, 'a', 0, 0x80}},
    };
    for (auto& c : cases) {
        auto r = dyld::parse_export_trie(ByteView(c.trie));
        if (r.ok()) report_failure(__FILE__, __LINE__, std::string("accepted: ") + c.what);
    }
}

CIDER_TEST(export_trie_limits_stop_hostile_tries) {
    // a chain of 600 single-edge nodes (each 6 bytes, offsets encoded with a redundant 2-byte ULEB): depth limit 512 refuses it
    std::vector<uint8_t> t;
    const size_t n = 600;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t next = 6 * (i + 1);
        t.push_back(0); t.push_back(1); t.push_back('a'); t.push_back(0);
        t.push_back(static_cast<uint8_t>((next & 0x7f) | 0x80));
        t.push_back(static_cast<uint8_t>(next >> 7));
    }
    t.push_back(0); t.push_back(0);           // last node: no terminal, no children
    auto r = dyld::parse_export_trie(ByteView(t));
    CHECK(!r.ok());
    dyld::ExportLimits lim;
    lim.max_depth = 1000;
    lim.max_name_bytes = 100000;
    CHECK(dyld::parse_export_trie(ByteView(t), lim).ok());
}

// ============================== dependency search ==============================================================================
CIDER_TEST(search_candidates_follow_dyld_rules) {
    const fs::path exe_dir = "C:/app/bin", loader_dir = "C:/app/lib/sub";
    auto c = dyld::search_candidates("@executable_path/../lib/x.dylib", exe_dir, loader_dir, {});
    REQUIRE_EQ(c.size(), size_t{1});
    CHECK_EQ(c[0].generic_string(), std::string("C:/app/lib/x.dylib"));
    c = dyld::search_candidates("@loader_path/inner/y.dylib", exe_dir, loader_dir, {});
    REQUIRE_EQ(c.size(), size_t{1});
    CHECK_EQ(c[0].generic_string(), std::string("C:/app/lib/sub/inner/y.dylib"));
    // @rpath: one candidate per rpath entry, each expanded relative to the image that OWNS the entry
    std::vector<dyld::RpathEntry> rp = {{"@loader_path/../a", "C:/app/lib/sub"}, {"@executable_path/b", "C:/app/bin"}, {"@rpath/bad", "C:/x"}, {"C:/abs", "C:/q"}};
    c = dyld::search_candidates("@rpath/z.dylib", exe_dir, loader_dir, rp);
    REQUIRE_EQ(c.size(), size_t{3});                                 // the @rpath-in-rpath entry is invalid and skipped
    CHECK_EQ(c[0].generic_string(), std::string("C:/app/lib/a/z.dylib"));
    CHECK_EQ(c[1].generic_string(), std::string("C:/app/bin/b/z.dylib"));
    CHECK_EQ(c[2].generic_string(), std::string("C:/abs/z.dylib"));
    CHECK(dyld::search_candidates("@rpath/z.dylib", exe_dir, loader_dir, {}).empty());
    c = dyld::search_candidates("/opt/lib/q.dylib", exe_dir, loader_dir, {});
    REQUIRE_EQ(c.size(), size_t{1});
    CHECK(dyld::search_candidates("relative/q.dylib", exe_dir, loader_dir, {}).empty());       // never searched in the working directory
    CHECK(dyld::search_candidates("@unknown/q.dylib", exe_dir, loader_dir, {}).empty());
}

// ============================== loading real dylibs ==============================================================================
CIDER_TEST(one_executable_and_one_dylib_return_42_from_the_dylib) {
    auto l = link_scenario("one");
    REQUIRE(l.program != nullptr);
    REQUIRE_EQ(l.program->images().size(), size_t{2});
    CHECK(l.program->images()[0]->is_main);
    CHECK_EQ(l.program->images()[1]->name, std::string("@rpath/libmath.dylib"));
    CHECK_EQ(l.program->images()[1]->leaf, std::string("libmath.dylib"));
    CHECK(l.program->images()[1]->info.filetype == macho::kFileDylib);
    auto add = l.program->find_symbol(1, "_cider_add");
    REQUIRE(add.has_value());
    // The address is inside the dylib's own executable mapping, not inside the executable and not a libSystem thunk.
    const auto& lib = *l.program->images()[1];
    CHECK(*add > lib.header && *add < lib.header + lib.mapped.layout.span);
    CHECK(!l.program->find_symbol(0, "_cider_add").has_value());
    expect_exit("one", 42);
}

CIDER_TEST(the_42_really_comes_from_the_dylib_machine_code) {
    // Patch `lea eax,[rdi+rsi]` (cider_add's body) in a COPY of the library: the executable's result changes.
    TempDir t;
    for (auto f : {"one", "libmath.dylib"}) t.write(f, file_bytes(dy("one", f)));
    auto lib = file_bytes(dy("one", "libmath.dylib"));
    const std::vector<uint8_t> lea = {0x8d, 0x04, 0x37};   // lea eax, [rdi+rsi]
    auto it = std::search(lib.begin(), lib.end(), lea.begin(), lea.end());
    REQUIRE(it != lib.end());
    // `lea eax,[rdi+rsi*1]` -> `mov eax,esi ; nop`   (result = second argument = 22)
    *it++ = 0x89; *it++ = 0xf0; *it = 0x90;
    t.write("libmath.dylib", lib);
    Run r = run_path(t.path() / "one");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 22);
}

CIDER_TEST(two_dylibs_are_loaded_and_both_contribute) {
    auto l = link_scenario("two");
    REQUIRE(l.program != nullptr);
    CHECK_EQ(l.program->images().size(), size_t{3});
    expect_exit("two", 42);
}

CIDER_TEST(transitive_dependency_is_found_through_the_first_dylib) {
    auto l = link_scenario("trans");
    REQUIRE(l.program != nullptr);
    REQUIRE_EQ(l.program->images().size(), size_t{3});
    // the executable never names libtransb: it was reached through libtransa
    for (auto& d : l.program->main().info.dylibs) CHECK(d.path.find("libtransb") == std::string::npos);
    CHECK_EQ(l.program->images()[2]->leaf, std::string("libtransb.dylib"));
    expect_exit("trans", 42);
}

CIDER_TEST(a_shared_dependency_is_loaded_exactly_once) {
    // init: the executable needs base and top, top needs base again.
    auto l = link_scenario("init");
    REQUIRE(l.program != nullptr);
    CHECK_EQ(l.program->images().size(), size_t{3});
    size_t base_count = 0;
    for (auto& r : l.program->images()) base_count += r->leaf == "libinitbase.dylib";
    CHECK_EQ(base_count, size_t{1});
}

CIDER_TEST(initializers_run_dependencies_first_exactly_once) {
    auto l = link_scenario("init");
    REQUIRE(l.program != nullptr);
    auto order = l.program->initialization_order();
    REQUIRE_EQ(order.size(), size_t{3});
    CHECK_EQ(l.program->images()[order[0]]->leaf, std::string("libinitbase.dylib"));
    CHECK_EQ(l.program->images()[order[1]]->leaf, std::string("libinittop.dylib"));
    CHECK(l.program->images()[order[2]]->is_main);
    for (auto& r : l.program->images()) CHECK_EQ(r->initializers.size(), size_t{1});
    // running them through a recording caller: three calls, in order, and a second pass does nothing (exactly once)
    std::vector<std::string> seen;
    auto caller = [&](uint64_t, const linker::ImageRecord& r) { seen.push_back(r.leaf); return linker::InitCall{}; };
    auto res = l.program->run_initializers(caller);
    CHECK(res.status == linker::InitResult::Status::Ok);
    CHECK_EQ(res.calls, size_t{3});
    CHECK_EQ(seen.size(), size_t{3});
    CHECK_EQ(seen[0], std::string("libinitbase.dylib"));
    seen.clear();
    res = l.program->run_initializers(caller);
    CHECK_EQ(res.calls, size_t{0});
    CHECK(seen.empty());
}

CIDER_TEST(an_initializer_modifies_a_global_and_the_executable_observes_it) {
    // init_main returns 1 if base_init did not run, 2 if top ran first, 3/4 on wrong count/order, 42 when all is right.
    Run r = run_scenario("init");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 42);
    CHECK_EQ(r.r.initializers_run, uint64_t{3});
}

CIDER_TEST(init_func_offsets_section_is_supported) {
    auto l = link_scenario("initoff");
    REQUIRE(l.program != nullptr);
    CHECK_EQ(l.program->images()[1]->initializers.size(), size_t{1});
    // __init_offsets entries are header-relative: the address is inside the dylib's code
    const auto& lib = *l.program->images()[1];
    CHECK(lib.initializers[0] > lib.header && lib.initializers[0] < lib.header + lib.mapped.layout.span);
    expect_exit("initoff", 42);
}

CIDER_TEST(exported_data_symbols_are_shared_in_both_directions) { expect_exit("glob", 42); }

CIDER_TEST(rpath_resolves_through_a_subdirectory) {
    auto l = link_scenario("rpath");
    REQUIRE(l.program != nullptr);
    CHECK(l.program->images()[1]->path.generic_string().find("/rpath/lib/librp.dylib") != std::string::npos);
    expect_exit("rpath", 42);
}

CIDER_TEST(loader_path_is_relative_to_the_loading_library) {
    auto l = link_scenario("loaderpath");
    REQUIRE(l.program != nullptr);
    REQUIRE_EQ(l.program->images().size(), size_t{3});
    CHECK(l.program->images()[1]->path.generic_string().find("/loaderpath/libs/liblpouter.dylib") != std::string::npos);
    CHECK(l.program->images()[2]->path.generic_string().find("/loaderpath/libs/inner/liblpinner.dylib") != std::string::npos);
    expect_exit("loaderpath", 42);
}

CIDER_TEST(executable_path_is_relative_to_the_executable_not_the_cwd) {
    // the same scenario copied elsewhere still resolves: nothing depends on the working directory or the original location
    TempDir t;
    for (auto f : {"one", "libmath.dylib"}) t.write(std::string("moved/") + f, file_bytes(dy("one", f)));
    Run r = run_path(t.path() / "moved" / "one");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 42);
}

CIDER_TEST(reexported_symbols_are_found_through_the_facade) {
    auto l = link_scenario("reexport");
    REQUIRE(l.program != nullptr);
    REQUIRE_EQ(l.program->images().size(), size_t{3});
    size_t facade = 0, core = 0;
    for (auto& r : l.program->images()) { if (r->leaf == "libfacade.dylib") facade = r->index; if (r->leaf == "libcore.dylib") core = r->index; }
    REQUIRE(facade != 0 && core != 0);
    auto via_facade = l.program->find_symbol(facade, "_core_fn");
    auto direct = l.program->find_symbol(core, "_core_fn");
    REQUIRE(via_facade.has_value());
    CHECK_EQ(*via_facade, *direct);
    CHECK(!l.program->find_symbol(facade, "_nonexistent").has_value());
    expect_exit("reexport", 42);
}

CIDER_TEST(a_missing_weak_library_binds_to_null) {
    Run r = run_scenario("weak");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 7);
}

// ============================== cycles ======================================================================================
CIDER_TEST(dependency_cycles_are_detected_and_tolerated_like_dyld) {
    auto l = link_scenario("cycle");
    REQUIRE(l.program != nullptr);
    CHECK_EQ(l.program->images().size(), size_t{3});           // each library is mapped once
    REQUIRE_EQ(l.program->cycles().size(), size_t{1});
    CHECK(l.program->cycles()[0].find("libcyca.dylib") != std::string::npos);
    CHECK(l.program->cycles()[0].find("libcycb.dylib") != std::string::npos);
    expect_exit("cycle", 42);
}

CIDER_TEST(dependency_cycles_can_be_refused) {
    auto l = link_scenario("cycle", [](linker::LinkOptions& o) { o.reject_cycles = true; });
    CHECK(l.program == nullptr);
    CHECK(l.error.kind == loader::LoadFailure::Incompatible);
    CHECK(l.error.message.find("dependency cycle") != std::string::npos);
    Run r = run_path(dy("cycle"), true);
    CHECK(r.r.state == guest::State::LoadFailed);
    CHECK(is_free(0x100000000ull));                              // the partial load left nothing behind
}

CIDER_TEST(initialization_order_ignores_back_edges_of_a_cycle) {
    auto l = link_scenario("cycle");
    REQUIRE(l.program != nullptr);
    auto order = l.program->initialization_order();
    CHECK_EQ(order.size(), size_t{3});                            // every image exactly once
    std::vector<size_t> sorted = order;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    CHECK_EQ(order.back(), size_t{0});                            // the executable is last
}

// ============================== failures ====================================================================================
CIDER_TEST(missing_library_is_reported_by_install_name_and_nothing_stays_mapped) {
    auto l = link_scenario("missing");
    CHECK(l.program == nullptr);
    CHECK(l.error.kind == loader::LoadFailure::MissingDependency);
    CHECK_EQ(l.error.missing_library, std::string("@rpath/libgone.dylib"));
    CHECK(l.error.message.find("libgone.dylib") != std::string::npos);
    CHECK(l.error.message.find("searched:") != std::string::npos);
    CHECK(is_free(0x100000000ull));
    Run r = run_scenario("missing");
    CHECK(r.r.state == guest::State::LoadFailed);
    CHECK(r.r.load_failure == loader::LoadFailure::MissingDependency);
    CHECK_EQ(r.r.entry_address, uint64_t{0});                      // control was never transferred
}

CIDER_TEST(incompatible_library_version_is_refused) {
    auto l = link_scenario("badver");
    CHECK(l.program == nullptr);
    CHECK(l.error.kind == loader::LoadFailure::Incompatible);
    CHECK(l.error.message.find("compatibility version 5.0.0") != std::string::npos);
    CHECK(l.error.message.find("1.0.0") != std::string::npos);
}

CIDER_TEST(system_libraries_are_never_loaded_or_substituted) {
    // The executable asks for libz; CIDER must not look for it anywhere, least of all among Windows DLLs.
    MachoSpec s;
    s.dylibs = {"/usr/lib/libSystem.B.dylib", "/usr/lib/libz.1.dylib", "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit"};
    auto b = build_macho(s).bytes;
    auto prov = take(libsystem::LibSystemProvider::create());
    linker::LinkError err;
    auto p = linker::Program::link(ByteView(b), {}, *prov, err);
    CHECK(p == nullptr);
    CHECK(err.kind == loader::LoadFailure::MissingDependency);
    CHECK(err.message.find("libz.1.dylib") != std::string::npos);
    CHECK(err.message.find("AppKit") != std::string::npos);
    CHECK(err.message.find("does not load system libraries") != std::string::npos);
    CHECK_EQ(err.required_libs.size(), size_t{3});
}

CIDER_TEST(tampered_dependency_files_are_rejected_not_loaded) {
    struct Case { const char* what; std::vector<uint8_t> bytes; const char* needle; };
    auto good_exe = file_bytes(dy("one", "one"));
    std::vector<uint8_t> pe = {'M', 'Z', 0x90, 0, 3, 0, 0, 0, 4, 0, 0, 0, 0xff, 0xff};      // a Windows PE header, not Mach-O
    pe.resize(512, 0);
    std::vector<uint8_t> trunc = file_bytes(dy("one", "libmath.dylib"));
    trunc.resize(trunc.size() / 3);
    std::vector<Case> cases = {
        {"text file", std::vector<uint8_t>{'h', 'e', 'l', 'l', 'o'}, "not a valid Mach-O"},
        {"Windows PE", pe, "not a valid Mach-O"},
        {"truncated dylib", trunc, ""},
        {"an executable instead of a dylib", good_exe, "not a dynamic library"},
        {"empty file", {}, ""},
    };
    for (auto& c : cases) {
        TempDir t;
        t.write("one", good_exe);
        t.write("libmath.dylib", c.bytes);
        Linked l = link_path(t.path() / "one");
        if (l.program) { report_failure(__FILE__, __LINE__, std::string("accepted: ") + c.what); continue; }
        if (c.needle[0] && l.error.message.find(c.needle) == std::string::npos) report_failure(__FILE__, __LINE__, std::string(c.what) + ": message was " + l.error.message);
    }
    CHECK(is_free(0x100000000ull));
}

CIDER_TEST(a_directory_or_a_dangling_name_is_not_a_library) {
    TempDir t;
    t.write("one", file_bytes(dy("one", "one")));
    fs::create_directories(t.path() / "libmath.dylib");              // a directory named like the dependency
    Linked l = link_path(t.path() / "one");
    CHECK(l.program == nullptr);
    CHECK(l.error.kind == loader::LoadFailure::MissingDependency);
}

CIDER_TEST(unresolved_symbols_in_a_dylib_are_reported_with_the_library) {
    // libmath without cider_add: the executable's bind cannot be resolved
    TempDir t;
    t.write("one", file_bytes(dy("one", "one")));
    t.write("libmath.dylib", file_bytes(dy("one", "libmath.dylib")));
    auto lib = file_bytes(t.path() / "libmath.dylib");
    // rename the exported symbol in the string table: "_cider_add" -> "_cider_sub"
    const std::string from = "_cider_add", to = "_cider_sub";
    auto it = std::search(lib.begin(), lib.end(), from.begin(), from.end());
    REQUIRE(it != lib.end());
    std::copy(to.begin(), to.end(), it);
    // the export trie stores the label too (patch every occurrence of the label bytes "cider_add")
    const std::string lf = "cider_add", lt = "cider_sub";
    for (auto jt = std::search(lib.begin(), lib.end(), lf.begin(), lf.end()); jt != lib.end(); jt = std::search(jt, lib.end(), lf.begin(), lf.end()))
        std::copy(lt.begin(), lt.end(), jt);
    t.write("libmath.dylib", lib);
    Linked l = link_path(t.path() / "one");
    CHECK(l.program == nullptr);
    CHECK(l.error.kind == loader::LoadFailure::UnresolvedSymbol);
    REQUIRE_EQ(l.error.unresolved.size(), size_t{1});
    CHECK_EQ(l.error.unresolved[0].symbol, std::string("_cider_add"));
    CHECK_EQ(l.error.unresolved[0].library, std::string("@rpath/libmath.dylib"));
    CHECK(l.error.unresolved[0].reason.find("not exported") != std::string::npos);
}

CIDER_TEST(image_count_depth_and_file_size_limits_are_enforced) {
    CHECK(link_scenario("two", [](linker::LinkOptions& o) { o.max_images = 2; }).program == nullptr);
    CHECK(link_scenario("trans", [](linker::LinkOptions& o) { o.max_depth = 1; }).program == nullptr);
    auto big = link_scenario("one", [](linker::LinkOptions& o) { o.max_file_size = 100; });
    CHECK(big.program == nullptr);
    CHECK(big.error.message.find("cannot read") != std::string::npos);
    CHECK(link_scenario("one").program != nullptr);                  // the defaults are fine
}

CIDER_TEST(failing_initializer_exit_stops_before_main) {
    Run r = run_scenario("initexit");
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 7);                                      // main would have returned 42
    CHECK(!r.r.returned_from_main);
    CHECK_EQ(r.r.initializers_run, uint64_t{1});
    CHECK(r.r.message.find("initializer called exit(7)") != std::string::npos);
}

CIDER_TEST(initializers_are_not_executed_when_symbols_are_unresolved) {
    // initialisation happens only after every relocation succeeded: a failed link never runs initializers
    TempDir t;
    t.write("initexit", file_bytes(dy("initexit", "initexit")));
    t.write("libinitexit.dylib", file_bytes(dy("initexit", "libinitexit.dylib")));
    auto exe = file_bytes(t.path() / "initexit");
    const std::string from = "cider_dummy_exit", to = "cider_dummy_exlt";
    for (auto jt = std::search(exe.begin(), exe.end(), from.begin(), from.end()); jt != exe.end(); jt = std::search(jt, exe.end(), from.begin(), from.end()))
        std::copy(to.begin(), to.end(), jt);
    t.write("initexit", exe);
    Run r = run_path(t.path() / "initexit");
    CHECK(r.r.state == guest::State::LoadFailed);
    CHECK_EQ(r.r.initializers_run, uint64_t{0});
}

// ============================== memory ======================================================================================
CIDER_TEST(every_image_has_wx_clean_protections) {
    auto l = link_scenario("init");
    REQUIRE(l.program != nullptr);
    for (auto& r : l.program->images()) {
        for (auto& s : r->mapped.layout.segments) {
            const uint8_t* host = r->mapped.base() + (s.vmaddr - r->mapped.layout.preferred_base);
            for (uint64_t off = 0; off < s.vmsize; off += 4096) {
                auto pi = memory::query_address(host + off);
                if (pi.state != memory::PageInfo::State::Committed || (pi.writable && pi.executable) || pi.prot != s.final_prot)
                    report_failure(__FILE__, __LINE__, r->leaf + " " + s.name + ": wrong page protection");
            }
        }
    }
}

CIDER_TEST(unloading_releases_every_image) {
    std::vector<uint64_t> bases;
    {
        auto l = link_scenario("two");
        REQUIRE(l.program != nullptr);
        for (auto& r : l.program->images()) {
            bases.push_back(r->mapped.actual_base());
            CHECK(!is_free(r->mapped.actual_base()));
        }
        l.program->unload();
        CHECK(l.program->images().empty());
        l.program->unload();                                          // idempotent
    }
    for (auto b : bases) CHECK(is_free(b));
}

CIDER_TEST(repeated_load_and_unload_does_not_leak_address_space) {
    for (int i = 0; i < 40; ++i) {
        auto l = link_scenario("trans");
        REQUIRE(l.program != nullptr);
    }
    CHECK(is_free(0x100000000ull));
}

// ============================== robustness ==================================================================================
CIDER_TEST(mutated_dylibs_never_crash_the_linker) {
    // Link-only (guest code is never executed): 600 random byte mutations of a real dylib, each with its dependants.
    const auto original = file_bytes(dy("trans", "libtransa.dylib"));
    const auto exe = file_bytes(dy("trans", "trans"));
    const auto libb = file_bytes(dy("trans", "libtransb.dylib"));
    std::mt19937 rng(0xC1DE5);
    size_t accepted = 0;
    for (int i = 0; i < 600; ++i) {
        auto mutated = original;
        const int n = 1 + static_cast<int>(rng() % 6);
        for (int k = 0; k < n; ++k) {
            size_t pos = rng() % mutated.size();
            switch (rng() % 3) {
                case 0: mutated[pos] = static_cast<uint8_t>(rng()); break;
                case 1: mutated[pos] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
                default: mutated[pos] = 0xff; break;
            }
        }
        if (rng() % 8 == 0) mutated.resize(rng() % mutated.size());
        TempDir t;
        t.write("trans", exe);
        t.write("libtransb.dylib", libb);
        t.write("libtransa.dylib", mutated);
        Linked l = link_path(t.path() / "trans");
        if (l.program) ++accepted;
    }
    CHECK(accepted < 600);            // many mutations hit headers and are rejected; the rest are harmless data changes
    CHECK(is_free(0x100000000ull));
}

CIDER_TEST(windows_paths_and_dlls_are_never_accepted_as_install_names) {
    auto prov = take(libsystem::LibSystemProvider::create());
    for (const char* name : {"C:\\Windows\\System32\\kernel32.dll","/C:/Windows/System32/user32.dll", "//server/share/x.dylib", "/tmp/a*.dylib", "/tmp/x.dylib:stream"}) {
        MachoSpec s;
        s.dylibs = {"/usr/lib/libSystem.B.dylib", name};
        auto b = build_macho(s).bytes;
        linker::LinkError err;
        auto p = linker::Program::link(ByteView(b), {}, *prov, err);
        if (p) { report_failure(__FILE__, __LINE__, std::string("accepted install name ") + name); continue; }
        if (err.message.find("not valid path text") == std::string::npos) report_failure(__FILE__, __LINE__, std::string(name) + ": " + err.message);
    }
    // A syntactically fine Darwin path that happens to name a real Windows DLL on this machine: the file is
    // opened as DATA only to parse it, and rejected because it is not Mach-O. It is never passed to LoadLibrary.
    MachoSpec s;
    s.dylibs = {"/usr/lib/libSystem.B.dylib", "/Windows/System32/kernel32.dll"};
    auto b = build_macho(s).bytes;
    linker::LinkError err;
    auto p = linker::Program::link(ByteView(b), {}, *prov, err);
    CHECK(p == nullptr);
    CHECK(err.message.find("kernel32.dll") != std::string::npos);
    CHECK(is_free(0x100000000ull));
}
