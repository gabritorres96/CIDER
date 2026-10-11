// Objective-C runtime (ORVYN 0.4.3-0.4.5): metadata parsing checked against llvm-objdump --objc-meta-data, real Apple-ABI programs built by clang
// and executed through objc_msgSend, runtime construction, categories/+load, ABI return classes, non-fragile layout, error traps and a
// malformed-metadata matrix. The programs are compiled Objective-C: nothing here interprets source code or fakes a result.
#include <algorithm>
#include <cstring>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>

#include "orvyn/guest.hpp"
#include "orvyn/linker.hpp"
#include "orvyn/objc.hpp"
#include "guest_code.hpp"
#include "macho_builder.hpp"
#include "relmeth.hpp"
#include "test_framework.hpp"

using namespace orvyn;
using namespace orvyn::testing;
namespace fs = std::filesystem;

namespace {

const fs::path kObjc = fs::path(ORVYN_OBJC_FIXTURES_DIR);

fs::path bin(const std::string& scenario, const std::string& file = {}) { return kObjc / "bin" / scenario / (file.empty() ? scenario : file); }
std::vector<uint8_t> load(const fs::path& p) {
    auto r = read_file(p, 32u << 20);
    if (!r.ok()) { report_failure(__FILE__, __LINE__, "fixture missing: " + path_to_utf8(p)); throw AbortTest{}; }
    return r.value();
}

struct Run {
    guest::Outcome r;
    std::string out, err;
};
Run run(const fs::path& exe, std::vector<std::string> args = {}, const std::vector<uint8_t>* bytes = nullptr) {
    Run run;
    guest::GuestConfig cfg;
    cfg.args = {path_to_utf8(exe)};
    cfg.args.insert(cfg.args.end(), args.begin(), args.end());
    cfg.image_path = exe;
    cfg.sink = [&](int fd, const char* d, size_t n) { (fd == 2 ? run.err : run.out).append(d, n); };
    auto b = bytes ? *bytes : load(exe);
    run.r = guest::run_in_process(ByteView(b), cfg);
    return run;
}
void expect_exit(const Run& r, int code, const char* what) {
    if (r.r.state != guest::State::Exited) { report_failure(__FILE__, __LINE__, std::string(what) + ": state " + guest::to_string(r.r.state) + " " + r.r.message); return; }
    if (r.r.exit_code != code) report_failure(__FILE__, __LINE__, std::string(what) + ": exit code " + std::to_string(r.r.exit_code) + ", expected " + std::to_string(code));
}

// Link + register the metadata WITHOUT running any guest code (used for metadata inspection and for hostile inputs).
struct Loaded {
    std::unique_ptr<libsystem::LibSystemProvider> provider;
    std::unique_ptr<libsystem::RuntimeContext> ctx;
    std::unique_ptr<linker::Program> program;
    std::vector<uint8_t> bytes;
    linker::LinkError link_error;
    std::string objc_error;
    bool ok = false;
    objc::Runtime* rt() { return ctx->objc.get(); }
};
std::unique_ptr<Loaded> prepare(const fs::path& exe, const std::vector<uint8_t>* patched = nullptr) {
    auto l = std::make_unique<Loaded>();
    l->provider = take(libsystem::LibSystemProvider::create());
    l->ctx = std::make_unique<libsystem::RuntimeContext>();
    l->ctx->objc = std::make_unique<objc::Runtime>(*l->ctx);
    objc::install_foundation(*l->ctx->objc);
    std::string why;
    l->ctx->objc->build_native_thunks(why);
    objc::Runtime* rt = l->ctx->objc.get();
    l->provider->set_dynamic_data([rt](std::string_view s) { return rt->dynamic_symbol(s); });
    l->bytes = patched ? *patched : load(exe);
    linker::LinkOptions lo;
    lo.executable_path = exe;
    lo.before_protect = [rt](linker::ImageRecord& image, std::string& error) { return rt->prepare_image(image, error); };
    l->program = linker::Program::link(ByteView(l->bytes), lo, *l->provider, l->link_error);
    if (l->program) l->ok = rt->finish_images(l->objc_error);
    return l;
}

// ---- llvm-objdump --objc-meta-data golden: (class, +/-, selector, types) --------------------------------------------------------------------
struct GoldenMethod { std::string cls; bool meta; std::string sel, types; };
struct GoldenIvar { std::string cls, name; long offset; int size; };
struct Golden {
    std::vector<GoldenMethod> methods;
    std::vector<GoldenIvar> ivars;
    std::vector<std::string> classes;
};
Golden read_golden(const std::string& key) {
    Golden g;
    std::ifstream f(kObjc / "golden" / (key + ".objc.txt"));
    std::string line, cls;
    bool meta = false, in_ivars = false, in_proto = false;
    std::string sel;
    long off = 0;
    std::string ivname;
    auto word_after = [](const std::string& l, const std::string& key_) -> std::string {
        auto p = l.find(key_);
        if (p == std::string::npos) return {};
        std::istringstream is(l.substr(p + key_.size()));
        std::string addr, text;
        is >> addr;
        std::getline(is, text);
        while (!text.empty() && text.front() == ' ') text.erase(text.begin());
        while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) text.pop_back();
        return text;
    };
    while (std::getline(f, line)) {
        std::smatch m;
        if (line.rfind("Contents of", 0) == 0) { cls.clear(); meta = false; in_ivars = false; in_proto = false; continue; }
        if (std::regex_search(line, m, std::regex(R"(^[0-9a-f]{16} 0x[0-9a-f]+ _OBJC_CLASS_\$_(\w+))"))) { cls = m[1]; meta = false; in_ivars = false; g.classes.push_back(cls); continue; }
        if (line.rfind("Meta Class", 0) == 0) { meta = true; in_ivars = false; in_proto = false; continue; }
        if (line.find("(struct protocol_t *)") != std::string::npos) in_proto = true;       // protocol method lists are not the class's
        if (line.find(" ivars ") != std::string::npos || line.find("weakIvarLayout") != std::string::npos || line.find("baseProperties") != std::string::npos) in_proto = false;
        if (line.find("baseMethods") != std::string::npos) in_ivars = false;
        if (line.find(" ivars ") != std::string::npos && line.find("0x0") == std::string::npos) in_ivars = true;
        if (line.find("      name 0x") != std::string::npos && !in_ivars && line.find("baseMethods") == std::string::npos) {
            // method name line or ivar name line (in_ivars): method lines are "name 0x.. sel"
        }
        if (in_proto) continue;
        const std::string t = line.substr(line.find_first_not_of(" \t") == std::string::npos ? 0 : line.find_first_not_of(" \t"));
        if (t.rfind("name 0x", 0) == 0 && !cls.empty()) {
            const std::string text = word_after(t, "name");
            if (in_ivars) ivname = text; else sel = text;
        } else if (t.rfind("types 0x", 0) == 0 && !cls.empty() && !in_ivars) {
            g.methods.push_back({cls, meta, sel, word_after(t, "types")});
        } else if (t.rfind("offset 0x", 0) == 0 && in_ivars) {
            std::istringstream is(t.substr(7));
            std::string addr;
            is >> addr >> off;
        } else if (t.rfind("size ", 0) == 0 && in_ivars) {
            g.ivars.push_back({cls, ivname, off, std::stoi(t.substr(5))});
        }
    }
    return g;
}

}  // namespace

// ============================== the reference program ==========================================================================
ORVYN_TEST(objective_c_calculator_returns_42_from_a_method_running_in_the_runtime) {
    Run r = run(bin("objc_calc"));
    expect_exit(r, 42, "objc_calc");
}

ORVYN_TEST(the_result_changes_when_the_method_machine_code_changes) {
    // patch `lea eax,[rdx+rcx]` (a + b) in -[Calculator add:to:] to `mov eax,edx ; nop` (just a): the guest returns 20
    auto b = load(bin("objc_calc"));
    const std::vector<uint8_t> lea = {0x8d, 0x04, 0x0a};         // leal (%rdx,%rcx), %eax
    auto it = std::search(b.begin(), b.end(), lea.begin(), lea.end());
    REQUIRE(it != b.end());
    *it++ = 0x89; *it++ = 0xd0; *it = 0x90;
    Run r = run(bin("objc_calc"), {}, &b);
    expect_exit(r, 20, "patched objc_calc");
}

ORVYN_TEST(runtime_state_after_loading_the_calculator) {
    auto l = prepare(bin("objc_calc"));
    REQUIRE(l->ok);
    objc::Runtime& rt = *l->rt();
    objc::Class* c = rt.class_named("Calculator");
    REQUIRE(c != nullptr);
    REQUIRE(c->meta != nullptr);
    CHECK(c->root && c->super == nullptr);
    CHECK(c->meta->super == c);                              // root metaclass: its superclass is the root class itself
    CHECK_EQ(c->meta->is_meta, true);
    CHECK_EQ(c->instance_size, 12u);                         // isa + int
    REQUIRE_EQ(c->ivars.size(), size_t{2});
    CHECK_EQ(c->ivars[1].name, std::string("total"));
    CHECK_EQ(c->ivars[1].offset, 8u);
    // the same selector string always maps to the same unique SEL, whichever image or API asks for it
    const auto s1 = rt.sel("add:to:"), s2 = rt.sel(std::string("add:") + "to:");
    CHECK_EQ(s1, s2);
    CHECK(c->by_sel.count(s1) == 1);
    CHECK(c->meta->by_sel.count(rt.sel("new")) == 1);
    CHECK(rt.find_imp(c, s1) != 0);
    CHECK(rt.find_imp(c, rt.sel("nonexistent:")) == 0);
    CHECK(rt.find_imp(c->meta, s1) != 0);                    // root class: class messages fall back to its INSTANCE methods (metaclass chain ends at the root class)
    CHECK(rt.find_imp(c->meta, rt.sel("total")) != 0);
    // the selector references in the image were rewritten to the runtime's unique SELs
    bool saw = false;
    for (auto& seg : l->program->main().info.segments)
        for (auto& sec : seg.sections)
            if (sec.name == "__objc_selrefs") {
                const auto* slots = reinterpret_cast<const uint64_t*>(l->program->main().mapped.host_address(sec.addr));
                for (size_t i = 0; i < sec.size / 8; ++i) saw |= slots[i] == s1;
            }
    CHECK(saw);
}

// ============================== metadata against llvm-objdump ====================================================================
ORVYN_TEST(parsed_metadata_equals_llvm_objdump) {
    struct Case { const char* scenario; const char* file; const char* key; };
    for (Case c : {Case{"objc_calc", "objc_calc", "objc_calc_objc_calc"}, Case{"objc_inherit", "objc_inherit", "objc_inherit_objc_inherit"},
                   Case{"objc_category", "objc_category", "objc_category_objc_category"}, Case{"objc_abi", "objc_abi", "objc_abi_objc_abi"},
                   Case{"slide", "libshape.dylib", "slide_libshape.dylib"}}) {
        Golden g = read_golden(c.key);
        REQUIRE(!g.classes.empty());
        REQUIRE(!g.methods.empty());
        // the dylib alone cannot be loaded as a program: load its executable and look at the class by name
        const std::string exe = std::string(c.scenario) == "slide" ? "slide" : c.file;
        auto l = prepare(bin(c.scenario, exe));
        if (!l->ok) { report_failure(__FILE__, __LINE__, std::string(c.key) + ": " + l->link_error.message + l->objc_error); continue; }
        objc::Runtime& rt = *l->rt();
        for (auto& cls : g.classes) CHECK(rt.class_named(cls) != nullptr);
        for (auto& m : g.methods) {
            objc::Class* k = rt.class_named(m.cls);
            if (!k) { report_failure(__FILE__, __LINE__, "class missing: " + m.cls); continue; }
            objc::Class* target = m.meta ? k->meta : k;
            // categories attach later methods to the same lists: look the method up by selector, compare name and type encoding
            bool found = false;
            for (auto& mm : target->methods)
                if (mm.name == m.sel && mm.types == m.types) found = true;
            if (!found) report_failure(__FILE__, __LINE__, std::string(c.key) + ": " + (m.meta ? "+" : "-") + "[" + m.cls + " " + m.sel + "] (" + m.types + ") not registered as llvm-objdump shows it");
        }
        for (auto& iv : g.ivars) {
            objc::Class* k = rt.class_named(iv.cls);
            if (!k) continue;
            bool found = false;
            for (auto& v : k->ivars)
                if (v.name == iv.name && static_cast<int>(v.size) == iv.size) found = true;
            if (!found) report_failure(__FILE__, __LINE__, std::string(c.key) + ": ivar " + iv.cls + "." + iv.name + " differs from llvm-objdump");
        }
    }
}

// ============================== features ==============================================================================================
ORVYN_TEST(inheritance_super_calls_class_methods_and_layout) { expect_exit(run(bin("objc_inherit")), 42, "objc_inherit"); }
ORVYN_TEST(runtime_api_builds_and_uses_a_class_at_run_time) {
    Run r = run(bin("objc_dynamic"));
    expect_exit(r, 42, "objc_dynamic");
    CHECK_EQ(r.out, std::string("dynamic classes ok\n"));
}
ORVYN_TEST(categories_load_methods_and_protocol_conformance) {
    Run r = run(bin("objc_category"));
    expect_exit(r, 42, "objc_category");
    CHECK_EQ(r.out, std::string("loads=2\n"));
}
ORVYN_TEST(message_abi_covers_every_return_class_nil_receivers_varargs_and_stack_arguments) {
    Run r = run(bin("objc_abi"));
    expect_exit(r, 42, "objc_abi");
    CHECK_EQ(r.out, std::string("abi ok\n"));
}
ORVYN_TEST(non_fragile_layout_slides_instance_variables_of_a_subclass) {
    Run r = run(bin("slide"));
    expect_exit(r, 42, "slide");
    CHECK_EQ(r.out, std::string("area=42 side=5 extra=99 size=32\n"));
    auto l = prepare(bin("slide"));
    REQUIRE(l->ok);
    objc::Class* sq = l->rt()->class_named("Square");
    REQUIRE(sq != nullptr);
    CHECK_EQ(sq->instance_start, 24u);                       // compiled as 16 against Shape v1, slid by the 8 bytes the dylib added
    CHECK_EQ(sq->ivars[0].offset, 24u);
    uint32_t in_memory = 0;
    std::memcpy(&in_memory, reinterpret_cast<const void*>(sq->ivars[0].offset_var), 4);
    CHECK_EQ(in_memory, 24u);                                // the offset VARIABLE in the image was rewritten, as the ABI requires
}

ORVYN_TEST(runtime_errors_end_the_guest_with_a_precise_diagnosis) {
    const struct { const char* mode; const char* needle; } cases[] = {
        {"u", "-[Thing notImplemented]: unrecognized selector sent to instance"},
        {"c", "+[Thing alsoMissing]: unrecognized selector sent to class"},
        {"i", "invalid object"},
        {"d", "invalid object"}};
    for (auto& c : cases) {
        Run r = run(bin("objc_error"), {c.mode});
        if (r.r.state != guest::State::Faulted) { report_failure(__FILE__, __LINE__, std::string("mode ") + c.mode + ": not faulted, state " + guest::to_string(r.r.state)); continue; }
        if (r.r.message.find(c.needle) == std::string::npos) report_failure(__FILE__, __LINE__, std::string("mode ") + c.mode + ": message was " + r.r.message);
        CHECK_EQ(r.out, std::string("before\n"));
    }
}

// ============================== relative ("small") method lists =================================================================================
// clang/ld64.lld only emit absolute lists, so relmeth.hpp converts the linked fixture into the format ld64 writes for macOS 11+ targets.
namespace {
std::vector<uint8_t> relative_fixture(testing::relmeth::Mode mode) {
    std::string why;
    auto r = testing::relmeth::to_relative(load(bin("objc_relative")), mode, &why);
    if (!r) { report_failure(__FILE__, __LINE__, "cannot convert fixture: " + why); throw AbortTest{}; }
    return *r;
}
}  // namespace

ORVYN_TEST(relative_method_lists_with_direct_selectors_run_like_absolute_lists) {
    expect_exit(run(bin("objc_relative")), 42, "absolute reference");
    const auto b = relative_fixture(testing::relmeth::Mode::Direct);
    CHECK(b != load(bin("objc_relative")));
    Run r = run(bin("objc_relative"), {}, &b);
    expect_exit(r, 42, "relative direct");
    CHECK_EQ(r.out, std::string("answer=42\n"));
}
ORVYN_TEST(relative_method_lists_with_indirect_selectors_run_like_absolute_lists) {
    const auto b = relative_fixture(testing::relmeth::Mode::Indirect);
    Run r = run(bin("objc_relative"), {}, &b);
    expect_exit(r, 42, "relative indirect");
    CHECK_EQ(r.out, std::string("answer=42\n"));
}
ORVYN_TEST(relative_and_absolute_lists_register_the_same_methods) {
    auto ref = prepare(bin("objc_relative"));
    REQUIRE(ref->ok);
    for (auto mode : {testing::relmeth::Mode::Direct, testing::relmeth::Mode::Indirect}) {
        const auto b = relative_fixture(mode);
        auto rel = prepare(bin("objc_relative"), &b);
        if (!rel->ok) { report_failure(__FILE__, __LINE__, "relative image did not load: " + rel->link_error.message + rel->objc_error); continue; }
        for (const char* cname : {"Item"})
            for (bool meta : {false, true}) {
                objc::Class* a = ref->rt()->class_named(cname);
                objc::Class* c = rel->rt()->class_named(cname);
                REQUIRE(a != nullptr);
                REQUIRE(c != nullptr);
                if (meta) { a = a->meta; c = c->meta; }
                REQUIRE_EQ(a->methods.size(), c->methods.size());
                CHECK(!a->methods.empty());
                for (size_t i = 0; i < a->methods.size(); ++i) {
                    CHECK_EQ(a->methods[i].name, c->methods[i].name);
                    CHECK_EQ(a->methods[i].types, c->methods[i].types);
                    // the implementations are addresses in two different loads: only their presence is comparable here (the run tests execute them)
                    CHECK(c->methods[i].imp != 0);
                }
            }
    }
}
ORVYN_TEST(malformed_relative_method_lists_are_refused_before_any_guest_code_runs) {
    const fs::path exe = bin("objc_relative");
    for (auto mode : {testing::relmeth::Mode::Direct, testing::relmeth::Mode::Indirect}) {
        const auto good = relative_fixture(mode);
        const uint64_t list = testing::relmeth::first_method_list(good);
        REQUIRE(list != 0);
        const uint64_t off = testing::relmeth::file_offset_of(good, list);
        REQUIRE(off != 0);
        auto put32 = [&](std::vector<uint8_t>& v, uint64_t at, uint32_t x) { std::memcpy(v.data() + at, &x, 4); };
        auto expect = [&](const std::vector<uint8_t>& v, const char* needle, const char* what) {
            Run r = run(exe, {}, &v);
            if (r.r.state != guest::State::LoadFailed) { report_failure(__FILE__, __LINE__, std::string("accepted: ") + what); return; }
            if (r.r.message.find(needle) == std::string::npos) report_failure(__FILE__, __LINE__, std::string(what) + ": message was " + r.r.message);
            CHECK_EQ(r.out, std::string(""));
        };
        {   auto v = good; put32(v, off, 0x80000000u | (mode == testing::relmeth::Mode::Direct ? 0x40000000u : 0u) | 16);
            expect(v, "unexpected small method entry size", "entsize 16"); }
        {   auto v = good; put32(v, off, 0x80000000u | (mode == testing::relmeth::Mode::Direct ? 0x40000000u : 0u) | 12 | 0x1000);
            expect(v, "unexpected small method entry size", "entsize 4108"); }
        {   auto v = good; put32(v, off + 4, 0x7fffffffu);
            expect(v, "truncated or too large", "huge count"); }
        {   auto v = good; put32(v, off + 4, 0xffffffffu);
            expect(v, "truncated or too large", "count -1"); }
        {   auto v = good; put32(v, off + 8, 0x7ffffff0u);             // name offset far outside the image
            expect(v, "outside the image", "name offset beyond the image"); }
        {   auto v = good; put32(v, off + 8, 0x80000010u);             // large negative offset
            expect(v, "outside the image", "negative offset beyond the image"); }
        {   auto v = good; put32(v, off + 16, 0x7fffffffu);            // imp offset: INT32_MAX
            expect(v, "outside the image", "imp offset INT32_MAX"); }
        {   auto v = good; put32(v, off + 12, 0x80000000u);            // types offset: INT32_MIN
            expect(v, "outside the image", "types offset INT32_MIN"); }
        if (mode == testing::relmeth::Mode::Indirect) {
            auto v = good; put32(v, off + 8, 0x10);                    // points into the list itself, not into __objc_selrefs
            expect(v, "__objc_selrefs", "indirect selector outside __objc_selrefs");
        }
    }
}

// ============================== @synchronized and associated objects (ORVYN 0.6) =======================================================
ORVYN_TEST(synchronized_is_recursive_exclusive_between_guest_threads_and_associations_follow_their_policies) {
    Run r = run(bin("objc_sync"));
    expect_exit(r, 42, "objc_sync");
    CHECK_EQ(r.out, std::string("sync and associations ok\n"));
}
ORVYN_TEST(objc_sync_exit_reports_a_lock_the_thread_does_not_own) {
    Run r = run(bin("objc_sync"), {"unlock"});
    expect_exit(r, 0, "objc_sync unlock");
    CHECK_EQ(r.out, std::string("exit=-1\nexit=0\nexit=-1\n"));              // not locked; then enter+exit; then not locked again
}
ORVYN_TEST(an_undefined_association_policy_ends_the_program_with_a_diagnostic) {
    Run r = run(bin("objc_sync"), {"bad_policy"});
    REQUIRE(r.r.state == guest::State::Faulted);
    CHECK(r.r.message.find("invalid association policy 2") != std::string::npos);
    CHECK_EQ(r.out, std::string("before\n"));
}

// ============================== hostile metadata ===============================================================================================
namespace {
struct Patcher {
    std::vector<uint8_t> b;
    macho::File file;
    explicit Patcher(const fs::path& p) : b(load(p)) { file = take(macho::parse_file(ByteView(b))); }
    uint64_t off(uint64_t vmaddr) const {
        for (auto& s : file.slices[0].image->segments)
            if (vmaddr >= s.vmaddr && vmaddr - s.vmaddr < s.filesize) return s.fileoff + (vmaddr - s.vmaddr);
        return 0;
    }
    uint64_t section_addr(const char* name) const {
        for (auto& s : file.slices[0].image->segments)
            for (auto& sec : s.sections) if (sec.name == name) return sec.addr;
        return 0;
    }
    uint64_t get64(uint64_t vm) const { uint64_t v; std::memcpy(&v, b.data() + off(vm), 8); return v; }
    void put64(uint64_t vm, uint64_t v) { std::memcpy(b.data() + off(vm), &v, 8); }
    void put32(uint64_t vm, uint32_t v) { std::memcpy(b.data() + off(vm), &v, 4); }
};
void expect_refused(const std::vector<uint8_t>& bytes, const char* needle, const char* what) {
    Run r = run(bin("objc_calc"), {}, &bytes);
    if (r.r.state != guest::State::LoadFailed) { report_failure(__FILE__, __LINE__, std::string("accepted: ") + what); return; }
    if (r.r.message.find(needle) == std::string::npos) report_failure(__FILE__, __LINE__, std::string(what) + ": message was " + r.r.message);
    CHECK_EQ(r.out, std::string(""));
}
}  // namespace

ORVYN_TEST(malformed_objective_c_metadata_is_refused_before_any_guest_code_runs) {
    const fs::path exe = bin("objc_calc");
    {   // class_ro_t.name -> unmapped address
        Patcher p(exe);
        const uint64_t classlist = p.section_addr("__objc_classlist");
        const uint64_t cls = p.get64(classlist), ro = p.get64(cls + 32) & ~7ull;
        p.put64(ro + 24, 0x1000);
        expect_refused(p.b, "class name is not readable", "class name pointer");
    }
    {   // method list with the relative-list flag (macOS 11+ format)
        Patcher p(exe);
        const uint64_t cls = p.get64(p.section_addr("__objc_classlist")), ro = p.get64(cls + 32) & ~7ull;
        const uint64_t methods = p.get64(ro + 32);
        p.put32(methods, 0x80000000u | 24);
        expect_refused(p.b, "unexpected small method entry size", "relative flag on a 24-byte entry list");
    }
    {   // wrong method entry size
        Patcher p(exe);
        const uint64_t cls = p.get64(p.section_addr("__objc_classlist")), ro = p.get64(cls + 32) & ~7ull;
        p.put32(p.get64(ro + 32), 16);
        expect_refused(p.b, "unexpected method entry size", "entsize 16");
    }
    {   // absurd method count
        Patcher p(exe);
        const uint64_t cls = p.get64(p.section_addr("__objc_classlist")), ro = p.get64(cls + 32) & ~7ull;
        p.put32(p.get64(ro + 32) + 4, 0x7fffffffu);
        expect_refused(p.b, "truncated or too large", "method count");
    }
    {   // superclass pointer to something that is not a class
        Patcher p(exe);
        const uint64_t cls = p.get64(p.section_addr("__objc_classlist"));
        p.put64(cls + 8, 0x100003018ull + 8);
        expect_refused(p.b, "is not a registered class", "bogus superclass");
    }
    {   // isa pointing at the class itself instead of a metaclass
        Patcher p(exe);
        const uint64_t cls = p.get64(p.section_addr("__objc_classlist"));
        p.put64(cls, cls);
        expect_refused(p.b, "metaclass", "isa is not a metaclass");
    }
    {   // ivar offset variable pointer -> null
        Patcher p(exe);
        const uint64_t cls = p.get64(p.section_addr("__objc_classlist")), ro = p.get64(cls + 32) & ~7ull;
        const uint64_t ivars = p.get64(ro + 48);
        p.put64(ivars + 8, 0);
        expect_refused(p.b, "ivar offset variable is not readable", "null ivar offset pointer");
    }
    {   // class list entry that points nowhere
        Patcher p(exe);
        p.put64(p.section_addr("__objc_classlist"), 0x10);
        expect_refused(p.b, "not readable", "class list entry");
    }
    {   // selector reference that is not a string pointer
        Patcher p(exe);
        p.put64(p.section_addr("__objc_selrefs"), 0x10);
        expect_refused(p.b, "does not point to a string", "selector reference");
    }
    {   // unsupported image info version
        Patcher p(exe);
        p.put32(p.section_addr("__objc_imageinfo"), 1);
        expect_refused(p.b, "__objc_imageinfo", "image info version");
    }
}

ORVYN_TEST(mutated_objective_c_images_never_crash_the_runtime_loader) {
    const std::vector<std::pair<std::string, std::string>> targets = {{"objc_category", "objc_category"}, {"objc_inherit", "objc_inherit"}, {"slide", "libshape.dylib"}};
    std::mt19937 rng(0x0B1EC7C);
    size_t refused = 0, total = 0;
    for (auto& [scenario, file] : targets) {
        Patcher base(bin(scenario, file));
        // the metadata lives in __DATA / __DATA_CONST: mutate those file ranges only (headers and code are covered by other suites)
        std::vector<std::pair<uint64_t, uint64_t>> ranges;
        for (auto& s : base.file.slices[0].image->segments)
            if (s.name.rfind("__DATA", 0) == 0 && s.filesize) ranges.emplace_back(s.fileoff, s.filesize);
        REQUIRE(!ranges.empty());
        for (int i = 0; i < 400; ++i) {
            auto m = base.b;
            for (int k = 0, n = 1 + static_cast<int>(rng() % 4); k < n; ++k) {
                auto [off, len] = ranges[rng() % ranges.size()];
                m[off + rng() % len] = static_cast<uint8_t>(rng());
            }
            // link-only for libshape.dylib is not possible (not an executable); mutate the executable's data in that scenario instead
            const std::string exe = scenario == "slide" ? "slide" : file;
            if (scenario == "slide") {
                Patcher exe_p(bin("slide", "slide"));
                std::vector<std::pair<uint64_t, uint64_t>> r2;
                for (auto& s : exe_p.file.slices[0].image->segments)
                    if (s.name.rfind("__DATA", 0) == 0 && s.filesize) r2.emplace_back(s.fileoff, s.filesize);
                m = exe_p.b;
                auto [off, len] = r2[rng() % r2.size()];
                m[off + rng() % len] = static_cast<uint8_t>(rng());
            }
            auto l = prepare(bin(scenario, exe), &m);
            ++total;
            if (!l->program || !l->ok) ++refused;
        }
    }
    CHECK(refused > 0);
    CHECK(total == 1200);
}

// ============================== memory management (0.4.5) ==================================================================
ORVYN_TEST(manual_reference_counting_pools_and_dealloc_chains) {
    Run r = run(bin("objc_mrc"));
    expect_exit(r, 42, "objc_mrc");
    CHECK_EQ(r.out, std::string("mrc ok\n"));
    CHECK_EQ(r.r.objc_live_objects, uint64_t{0});                // every object the program created was released and freed
}

ORVYN_TEST(automatic_reference_counting_strong_weak_and_autoreleased_returns) {
    Run r = run(bin("objc_arc"));
    expect_exit(r, 42, "objc_arc");
    CHECK_EQ(r.out, std::string("arc ok\n"));
    CHECK_EQ(r.r.objc_live_objects, uint64_t{0});
}

ORVYN_TEST(a_program_that_does_not_release_its_object_is_seen_as_leaking_it) {
    Run r = run(bin("objc_calc"));
    expect_exit(r, 42, "objc_calc");
    CHECK_EQ(r.r.objc_live_objects, uint64_t{1});                // the Calculator is never freed by that program
}

ORVYN_TEST(memory_management_errors_end_the_guest_with_a_diagnosis) {
    const struct { const char* mode; const char* needle; } cases[] = {
        {"o", "release of"}, {"r", "retain of"}, {"p", "invalid token"}};
    for (auto& c : cases) {
        Run r = run(bin("objc_memory_error"), {c.mode});
        if (r.r.state != guest::State::Faulted) { report_failure(__FILE__, __LINE__, std::string("mode ") + c.mode + ": not faulted"); continue; }
        if (r.r.message.find(c.needle) == std::string::npos) report_failure(__FILE__, __LINE__, std::string("mode ") + c.mode + ": " + r.r.message);
        CHECK_EQ(r.out, std::string("before\n"));
    }
    Run n = run(bin("objc_memory_error"), {"n"});                  // no autorelease pool: a warning and a leak, the program goes on
    expect_exit(n, 5, "autorelease without a pool");
    CHECK_EQ(n.out, std::string("before\ncontinued\n"));
    Run x = run(bin("objc_memory_error"), {"x"});                  // a -dealloc that forgets to free: the runtime frees the object
    expect_exit(x, 6, "forgetful dealloc");
    CHECK_EQ(x.out, std::string("before\nfreed=1\n"));
    CHECK_EQ(x.r.objc_live_objects, uint64_t{1});                  // only the first object (never released by mode x) remains
}
