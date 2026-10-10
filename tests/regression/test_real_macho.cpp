// Real-world validation: Mach-O files produced by LLVM (clang + ld64.lld, see
// tools/gen-real-fixtures.cmd) are parsed by CIDER and the result is compared field by field with
// the output of an independent tool, llvm-objdump.
//
// * The committed golden snapshots (tests/fixtures/real/golden) are always compared.
// * When llvm-objdump is available (CIDER_LLVM_BIN or PATH) the live output must also equal the
//   golden snapshot (detects tool drift); otherwise that check is skipped.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include "cider/analysis.hpp"
#include "cider/bundle.hpp"
#include "cider/macho.hpp"
#include "cider/process.hpp"
#include "cider/util.hpp"
#include "macho_builder.hpp"
#include "test_framework.hpp"

using namespace cider;
using namespace cider::testing;
namespace fs = std::filesystem;

namespace {

const fs::path kReal = fs::path(CIDER_REAL_FIXTURES_DIR);

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { report_failure(__FILE__, __LINE__, "cannot read " + path_to_utf8(p)); throw AbortTest{}; }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<uint8_t> bytes_of(const std::string& name) {
    auto r = read_file(kReal / "bin" / name, 64u << 20);
    if (!r.ok()) { report_failure(__FILE__, __LINE__, "fixture missing: " + name); throw AbortTest{}; }
    return r.value();
}

macho::File parse_real(const std::string& name, bool symbols = true) {
    auto b = bytes_of(name);
    macho::ParseOptions o;
    o.parse_symbols = symbols;
    auto r = macho::parse_file(ByteView(b), o);
    if (!r.ok()) { report_failure(__FILE__, __LINE__, name + ": " + r.error().message); throw AbortTest{}; }
    return r.value();
}

// ---- llvm-objdump --macho --private-headers parser ------------------------------------------------
struct Cmd {
    std::string name;
    std::map<std::string, std::string> kv;
    std::vector<std::map<std::string, std::string>> sections;
};
struct Dump {
    uint32_t ncmds = 0, sizeofcmds = 0;
    std::vector<Cmd> cmds;
};

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

Dump parse_private_headers(const std::string& text) {
    Dump d;
    std::istringstream in(text);
    bool header_next = false, in_section = false;
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (line.rfind("Mach header", 0) == 0) { header_next = true; continue; }
        if (header_next && line.rfind("magic", 0) == 0) continue;
        if (header_next) {
            std::istringstream ls(line);
            std::vector<std::string> tok;
            for (std::string t; ls >> t;) tok.push_back(t);
            bool first = true;
            for (auto& t : tok) {
                if (!std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
                (first ? d.ncmds : d.sizeofcmds) = static_cast<uint32_t>(std::stoul(t));
                if (!first) break;
                first = false;
            }
            header_next = false;
            continue;
        }
        if (line.rfind("Load command", 0) == 0) { d.cmds.emplace_back(); in_section = false; continue; }
        if (line == "Section") { if (!d.cmds.empty()) d.cmds.back().sections.emplace_back(); in_section = true; continue; }
        if (d.cmds.empty() || line.empty()) continue;
        auto sp = line.find(' ');
        std::string key = line.substr(0, sp), val = sp == std::string::npos ? "" : trim(line.substr(sp + 1));
        if (key == "cmd") { d.cmds.back().name = val; in_section = false; continue; }
        if (key == "compatibility" && val.rfind("version", 0) == 0) { key = "compatibility_version"; val = trim(val.substr(7)); }
        if (key == "current" && val.rfind("version", 0) == 0) { key = "current_version"; val = trim(val.substr(7)); }
        if (key == "time") continue;
        if (key == "tool") { auto sp2 = val.find(' '); val = val.substr(0, sp2); }
        auto off = val.rfind(" (offset");
        if (off != std::string::npos) val = val.substr(0, off);
        if (in_section && !d.cmds.back().sections.empty()) d.cmds.back().sections.back()[key] = val;
        else d.cmds.back().kv[key] = val;
    }
    return d;
}

std::string full_version(uint32_t v) { return std::to_string(v >> 16) + "." + std::to_string((v >> 8) & 0xff) + "." + std::to_string(v & 0xff); }

uint64_t num(const std::string& s) { return std::stoull(s, nullptr, 0); }

uint32_t lc_id(const std::string& n) {
    static const std::map<std::string, uint32_t> t = {
        {"LC_SEGMENT_64", 0x19}, {"LC_SYMTAB", 0x2}, {"LC_DYSYMTAB", 0xb}, {"LC_LOAD_DYLIB", 0xc}, {"LC_ID_DYLIB", 0xd},
        {"LC_LOAD_DYLINKER", 0xe}, {"LC_UUID", 0x1b}, {"LC_CODE_SIGNATURE", 0x1d}, {"LC_DYLD_INFO_ONLY", 0x80000022},
        {"LC_FUNCTION_STARTS", 0x26}, {"LC_DATA_IN_CODE", 0x29}, {"LC_SOURCE_VERSION", 0x2a}, {"LC_BUILD_VERSION", 0x32},
        {"LC_LOAD_WEAK_DYLIB", 0x80000018}, {"LC_RPATH", 0x8000001c}, {"LC_MAIN", 0x80000028},
        {"LC_DYLD_EXPORTS_TRIE", 0x80000033}, {"LC_DYLD_CHAINED_FIXUPS", 0x80000034}, {"LC_REEXPORT_DYLIB", 0x8000001f}};
    auto it = t.find(n);
    return it == t.end() ? 0 : it->second;
}

// Compares everything the objdump dump says about one Mach-O slice against CIDER's parse.
void compare_image(const std::string& label, const Dump& d, const macho::Image& img, bool strict = true) {
    auto at = [&](const std::string& m) { return label + ": " + m; };
    CHECK_EQ(d.ncmds, img.ncmds);
    CHECK_EQ(d.sizeofcmds, img.sizeofcmds);
    REQUIRE_EQ(d.cmds.size(), img.load_commands.size());

    size_t seg_i = 0, dylib_i = 0, rpath_i = 0;
    for (size_t i = 0; i < d.cmds.size(); ++i) {
        const Cmd& c = d.cmds[i];
        uint32_t id = lc_id(c.name);
        if (id == 0) {
            if (strict) report_failure(__FILE__, __LINE__, at("unmapped load command " + c.name));
        } else {
            CHECK_EQ(img.load_commands[i], id);
        }

        if (c.name == "LC_SEGMENT_64") {
            REQUIRE(seg_i < img.segments.size());
            const auto& s = img.segments[seg_i++];
            CHECK_EQ(s.name, c.kv.at("segname"));
            CHECK_EQ(s.vmaddr, num(c.kv.at("vmaddr")));
            CHECK_EQ(s.vmsize, num(c.kv.at("vmsize")));
            CHECK_EQ(s.fileoff, num(c.kv.at("fileoff")));
            CHECK_EQ(s.filesize, num(c.kv.at("filesize")));
            auto prot = [](int32_t p) { return std::string{(p & 1) ? 'r' : '-', (p & 2) ? 'w' : '-', (p & 4) ? 'x' : '-'}; };
            CHECK_EQ(prot(s.maxprot), c.kv.at("maxprot"));
            CHECK_EQ(prot(s.initprot), c.kv.at("initprot"));
            CHECK_EQ(s.sections.size(), static_cast<size_t>(num(c.kv.at("nsects"))));
            REQUIRE_EQ(s.sections.size(), c.sections.size());
            for (size_t k = 0; k < s.sections.size(); ++k) {
                CHECK_EQ(s.sections[k].name, c.sections[k].at("sectname"));
                CHECK_EQ(s.sections[k].segment, c.sections[k].at("segname"));
                CHECK_EQ(s.sections[k].addr, num(c.sections[k].at("addr")));
                CHECK_EQ(s.sections[k].size, num(c.sections[k].at("size")));
                CHECK_EQ(static_cast<uint64_t>(s.sections[k].offset), num(c.sections[k].at("offset")));
            }
        } else if (c.name == "LC_LOAD_DYLIB" || c.name == "LC_LOAD_WEAK_DYLIB" || c.name == "LC_REEXPORT_DYLIB") {
            REQUIRE(dylib_i < img.dylibs.size());
            const auto& dl = img.dylibs[dylib_i++];
            CHECK_EQ(dl.path, c.kv.at("name"));
            CHECK(dl.kind == (c.name == "LC_LOAD_DYLIB" ? macho::DylibKind::Load
                              : c.name == "LC_LOAD_WEAK_DYLIB" ? macho::DylibKind::Weak : macho::DylibKind::Reexport));
            CHECK_EQ(full_version(dl.current_version), c.kv.at("current_version"));
            CHECK_EQ(full_version(dl.compat_version), c.kv.at("compatibility_version"));
        } else if (c.name == "LC_ID_DYLIB") {
            REQUIRE(img.id_dylib.has_value());
            CHECK_EQ(img.id_dylib->path, c.kv.at("name"));
        } else if (c.name == "LC_RPATH") {
            REQUIRE(rpath_i < img.rpaths.size());
            CHECK_EQ(img.rpaths[rpath_i++], c.kv.at("path"));
        } else if (c.name == "LC_LOAD_DYLINKER") {
            CHECK_EQ(img.dylinker, c.kv.at("name"));
        } else if (c.name == "LC_MAIN") {
            REQUIRE(img.entry.has_value());
            CHECK_EQ(img.entry->file_offset, num(c.kv.at("entryoff")));
            CHECK_EQ(img.entry->stack_size, num(c.kv.at("stacksize")));
        } else if (c.name == "LC_UUID") {
            REQUIRE(img.uuid.has_value());
            CHECK_EQ(*img.uuid, c.kv.at("uuid"));
        } else if (c.name == "LC_BUILD_VERSION") {
            if (img.build_versions.size() != 1) continue;  // zippered binaries: several build versions
            const auto& b = img.build_versions[0];
            CHECK_EQ(std::string(macho::platform_name(b.platform)), std::string("macOS"));
            CHECK_EQ(c.kv.at("platform"), std::string("macos"));
            auto ver = [](uint32_t v) { return v ? version_string(v) : std::string("n/a"); };  // objdump prints n/a for 0
            CHECK_EQ(ver(b.minos), c.kv.at("minos"));
            CHECK_EQ(ver(b.sdk), c.kv.at("sdk"));
            CHECK_EQ(b.tools.size(), static_cast<size_t>(num(c.kv.at("ntools"))));
        } else if (c.name == "LC_SYMTAB") {
            REQUIRE(img.symtab.has_value());
            CHECK_EQ(static_cast<uint64_t>(img.symtab->symoff), num(c.kv.at("symoff")));
            CHECK_EQ(static_cast<uint64_t>(img.symtab->nsyms), num(c.kv.at("nsyms")));
            CHECK_EQ(static_cast<uint64_t>(img.symtab->stroff), num(c.kv.at("stroff")));
            CHECK_EQ(static_cast<uint64_t>(img.symtab->strsize), num(c.kv.at("strsize")));
        } else if (c.name == "LC_DYSYMTAB") {
            REQUIRE(img.dysymtab.has_value());
            CHECK_EQ(static_cast<uint64_t>(img.dysymtab->ilocalsym), num(c.kv.at("ilocalsym")));
            CHECK_EQ(static_cast<uint64_t>(img.dysymtab->nlocalsym), num(c.kv.at("nlocalsym")));
            CHECK_EQ(static_cast<uint64_t>(img.dysymtab->iextdefsym), num(c.kv.at("iextdefsym")));
            CHECK_EQ(static_cast<uint64_t>(img.dysymtab->nextdefsym), num(c.kv.at("nextdefsym")));
            CHECK_EQ(static_cast<uint64_t>(img.dysymtab->iundefsym), num(c.kv.at("iundefsym")));
            CHECK_EQ(static_cast<uint64_t>(img.dysymtab->nundefsym), num(c.kv.at("nundefsym")));
            CHECK_EQ(static_cast<uint64_t>(img.dysymtab->nindirectsyms), num(c.kv.at("nindirectsyms")));
        } else if (c.name == "LC_DYLD_INFO_ONLY") {
            REQUIRE(img.dyld_info.has_value());
            CHECK_EQ(static_cast<uint64_t>(img.dyld_info->rebase.offset), num(c.kv.at("rebase_off")));
            CHECK_EQ(static_cast<uint64_t>(img.dyld_info->rebase.size), num(c.kv.at("rebase_size")));
            CHECK_EQ(static_cast<uint64_t>(img.dyld_info->bind.offset), num(c.kv.at("bind_off")));
            CHECK_EQ(static_cast<uint64_t>(img.dyld_info->bind.size), num(c.kv.at("bind_size")));
            CHECK_EQ(static_cast<uint64_t>(img.dyld_info->lazy_bind.offset), num(c.kv.at("lazy_bind_off")));
            CHECK_EQ(static_cast<uint64_t>(img.dyld_info->lazy_bind.size), num(c.kv.at("lazy_bind_size")));
            CHECK_EQ(static_cast<uint64_t>(img.dyld_info->exports.offset), num(c.kv.at("export_off")));
            CHECK_EQ(static_cast<uint64_t>(img.dyld_info->exports.size), num(c.kv.at("export_size")));
        } else if (c.name == "LC_FUNCTION_STARTS" || c.name == "LC_DYLD_CHAINED_FIXUPS" || c.name == "LC_DYLD_EXPORTS_TRIE" ||
                   c.name == "LC_CODE_SIGNATURE") {
            const std::optional<macho::LinkeditRange>* r = nullptr;
            std::optional<macho::LinkeditRange> sig;
            if (c.name == "LC_FUNCTION_STARTS") r = &img.function_starts;
            else if (c.name == "LC_DYLD_CHAINED_FIXUPS") r = &img.chained_fixups;
            else if (c.name == "LC_DYLD_EXPORTS_TRIE") r = &img.exports_trie;
            else {
                REQUIRE(img.code_signature.has_value());
                sig = macho::LinkeditRange{img.code_signature->dataoff, img.code_signature->datasize};
                r = &sig;
            }
            REQUIRE(r->has_value());
            CHECK_EQ(static_cast<uint64_t>((*r)->offset), num(c.kv.at("dataoff")));
            CHECK_EQ(static_cast<uint64_t>((*r)->size), num(c.kv.at("datasize")));
        }
    }
    CHECK_EQ(seg_i, img.segments.size());
    CHECK_EQ(dylib_i, img.dylibs.size());
    CHECK_EQ(rpath_i, img.rpaths.size());
}

// llvm-objdump --syms: "<addr> <flags> <seg,sect> <name>" or "<addr> *UND* <name>"
void compare_symbols(const std::string& label, const std::string& syms_text, const macho::Image& img) {
    struct S { uint64_t value; bool undef, ext, debug; std::string name; };
    std::vector<S> expected;
    std::istringstream in(syms_text);
    for (std::string line; std::getline(in, line);) {
        if (line.size() < 17 || !std::isxdigit(static_cast<unsigned char>(line[0])) || line[16] != ' ') continue;
        S s{};
        s.value = std::stoull(line.substr(0, 16), nullptr, 16);
        std::string rest = trim(line.substr(17));
        s.debug = rest.rfind('d', 0) == 0;  // flag column 'd': stab / debugging entry (printed as *UND* when n_sect == 0)
        s.undef = line.find("*UND*") != std::string::npos && !s.debug;
        // name = everything after the section column ("*UND*" / "__TEXT,__text"): Swift names contain spaces
        size_t pos = 0;
        std::string name;
        for (;;) {
            size_t sp = rest.find(' ', pos);
            std::string tok = rest.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
            bool section = tok.find(',') != std::string::npos || (tok.size() > 2 && tok.front() == '*' && tok.back() == '*');
            if (sp == std::string::npos) { name = tok; break; }
            pos = sp + 1;
            while (pos < rest.size() && rest[pos] == ' ') ++pos;
            if (section) { name = rest.substr(pos); break; }
        }
        bool hidden = false;
        for (const char* mod : {".hidden ", ".private_extern ", ".weak_def_can_be_hidden ", ".weak_definition ", ".no_dead_strip ", ".alt_entry "})
            if (name.rfind(mod, 0) == 0) { hidden = hidden || std::strcmp(mod, ".hidden ") == 0; name = name.substr(std::strlen(mod)); }
        s.name = name;
        // flag column: g = global, w = weak (external unless printed ".hidden", i.e. N_PEXT without N_EXT)
        s.ext = rest.rfind('g', 0) == 0 || (rest.rfind('w', 0) == 0 && !hidden) || s.undef;
        expected.push_back(s);
    }
    REQUIRE_EQ(expected.size(), img.symbols.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        CHECK_EQ(img.symbols[i].name, expected[i].name);
        CHECK_EQ(img.symbols[i].value, expected[i].value);
        CHECK_EQ(img.symbols[i].undefined, expected[i].undef);
        CHECK_EQ(img.symbols[i].debug, expected[i].debug);
        if (img.symbols[i].external != expected[i].ext) report_failure(__FILE__, __LINE__, label + ": external flag of " + expected[i].name);
    }
}

// Is llvm-objdump runnable here?
std::optional<fs::path> objdump_exe() {
    static std::optional<fs::path> found = []() -> std::optional<fs::path> {
        std::vector<fs::path> dirs;
        if (const char* b = std::getenv("CIDER_LLVM_BIN")) dirs.push_back(path_from_utf8(b));
        if (const char* p = std::getenv("PATH")) {
            std::string s = p;
            size_t st = 0;
            while (st <= s.size()) {
                size_t e = s.find(';', st);
                dirs.push_back(path_from_utf8(s.substr(st, e == std::string::npos ? std::string::npos : e - st)));
                if (e == std::string::npos) break;
                st = e + 1;
            }
        }
        for (auto& d : dirs) {
            auto exe = d / "llvm-objdump.exe";
            std::error_code ec;
            if (d.is_absolute() && fs::is_regular_file(exe, ec)) return exe;
        }
        return std::nullopt;
    }();
    return found;
}

std::string live_objdump(const std::string& name, const std::vector<std::string>& flags) {
    std::vector<std::string> args = {"--macho"};
    args.insert(args.end(), flags.begin(), flags.end());
    args.push_back(path_to_utf8(kReal / "bin" / name));
    auto r = run_process(*objdump_exe(), args, std::chrono::seconds(60));
    return r.ok() ? r.value().output : std::string();
}

void check_fixture(const std::string& name, bool has_syms = true) {
    auto file = parse_real(name);
    REQUIRE_EQ(file.slices.size(), size_t{1});
    REQUIRE(file.slices[0].image.has_value());
    const auto& img = *file.slices[0].image;
    auto dump = parse_private_headers(slurp(kReal / "golden" / (name + ".private-headers.txt")));
    compare_image(name, dump, img);
    if (has_syms) compare_symbols(name, slurp(kReal / "golden" / (name + ".syms.txt")), img);
}

}  // namespace

CIDER_TEST(real_executable_matches_llvm_objdump) { check_fixture("hello_x86_64"); }
CIDER_TEST(real_arm64_executable_matches_llvm_objdump) { check_fixture("hello_arm64"); }
CIDER_TEST(real_adhoc_signed_executable_matches_llvm_objdump) {
    check_fixture("hello_signed_x86_64");
    auto img = *parse_real("hello_signed_x86_64").slices[0].image;
    REQUIRE(img.code_signature.has_value());
    CHECK(img.code_signature->blob_parsed);
    CHECK(img.code_signature->adhoc);
    CHECK_EQ(img.code_signature->identifier, std::string("hello_signed_x86_64"));  // from the SuperBlob, not the path
    CHECK(img.warnings.empty());
}
CIDER_TEST(real_chained_fixups_executable_matches_llvm_objdump) {
    check_fixture("hello_chained_x86_64");
    auto img = *parse_real("hello_chained_x86_64").slices[0].image;
    CHECK(img.chained_fixups.has_value());
    CHECK(img.exports_trie.has_value());
    CHECK(!img.dyld_info.has_value());
}
CIDER_TEST(real_dylibs_match_llvm_objdump) {
    check_fixture("libfoo.dylib");
    check_fixture("libweak.dylib");
    auto foo = *parse_real("libfoo.dylib").slices[0].image;
    CHECK_EQ(foo.id_dylib->path, std::string("@rpath/libfoo.dylib"));
    CHECK(foo.filetype == macho::kFileDylib);
    CHECK(!foo.entry.has_value());
}
CIDER_TEST(real_app_with_rpaths_and_weak_dependency_matches_llvm_objdump) {
    check_fixture("app_x86_64");
    auto img = *parse_real("app_x86_64").slices[0].image;
    REQUIRE_EQ(img.dylibs.size(), size_t{3});
    CHECK(img.dylibs[2].kind == macho::DylibKind::Weak);
    REQUIRE_EQ(img.rpaths.size(), size_t{2});
}
CIDER_TEST(real_relocatable_object_matches_llvm_objdump) {
    auto file = parse_real("hello_x86_64.o");
    const auto& img = *file.slices[0].image;
    CHECK(img.filetype == macho::kFileObject);
    auto dump = parse_private_headers(slurp(kReal / "golden" / "hello_x86_64.o.private-headers.txt"));
    compare_image("hello_x86_64.o", dump, img);
    compare_symbols("hello_x86_64.o", slurp(kReal / "golden" / "hello_x86_64.o.syms.txt"), img);
}

CIDER_TEST(real_universal_binary_matches_llvm_objdump_and_its_thin_slices) {
    auto file = parse_real("hello_universal");
    REQUIRE(file.fat);
    REQUIRE_EQ(file.slices.size(), size_t{2});
    std::string text = slurp(kReal / "golden" / "hello_universal.universal-headers.txt");
    // "offset N / size N / align 2^N" appear once per architecture, in order
    std::vector<uint64_t> offsets, sizes, aligns;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (line.rfind("offset ", 0) == 0) offsets.push_back(num(line.substr(7)));
        else if (line.rfind("size ", 0) == 0) sizes.push_back(num(line.substr(5)));
        else if (line.rfind("align 2^", 0) == 0) aligns.push_back(num(line.substr(8)));
    }
    REQUIRE_EQ(offsets.size(), size_t{2});
    for (size_t i = 0; i < 2; ++i) {
        CHECK_EQ(file.slices[i].offset, offsets[i]);
        CHECK_EQ(file.slices[i].size, sizes[i]);
        CHECK_EQ(static_cast<uint64_t>(file.slices[i].align), aligns[i]);
        REQUIRE(file.slices[i].image.has_value());
    }
    CHECK_EQ(macho::cpu_name(file.slices[0].cputype, file.slices[0].cpusubtype), std::string("x86_64"));
    CHECK_EQ(macho::cpu_name(file.slices[1].cputype, file.slices[1].cpusubtype), std::string("arm64"));
    CHECK(file.warnings.empty());
    // each slice parses to the same result as the thin file it was built from
    auto thin_x = parse_real("hello_x86_64");
    auto thin_a = parse_real("hello_arm64");
    CHECK_EQ(file.slices[0].image->segments.size(), thin_x.slices[0].image->segments.size());
    CHECK_EQ(file.slices[1].image->symbols.size(), thin_a.slices[0].image->symbols.size());
    CHECK_EQ(file.slices[0].image->entry->file_offset, thin_x.slices[0].image->entry->file_offset);
}

CIDER_TEST(live_llvm_objdump_still_agrees_with_the_golden_snapshots) {
    if (!objdump_exe()) SKIP("llvm-objdump not found (set CIDER_LLVM_BIN)");
    for (const char* n : {"hello_x86_64", "app_x86_64", "libfoo.dylib", "hello_signed_x86_64"}) {
        std::string live = live_objdump(n, {"--private-headers"});
        std::string golden = slurp(kReal / "golden" / (std::string(n) + ".private-headers.txt"));
        auto norm = [](std::string s) {  // first line is the (path-dependent) file name
            s.erase(0, s.find('\n') + 1);
            s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
            return s;
        };
        CHECK_EQ(norm(live), norm(golden));
        auto img = *parse_real(n).slices[0].image;
        compare_image(std::string("live ") + n, parse_private_headers(live), img);
    }
}

CIDER_TEST(real_fixtures_dependency_analysis_resolves_a_bundle) {
    TempDir t;
    // lay the real files out like an application bundle: rpath @executable_path/../Frameworks
    t.write("Real.app/Contents/Info.plist", xml_plist({{"CFBundleExecutable", "app"}, {"CFBundleIdentifier", "org.cider.real"}, {"CFBundleName", "Real"}}));
    t.write("Real.app/Contents/MacOS/app", bytes_of("app_x86_64"));
    t.write("Real.app/Contents/Frameworks/libfoo.dylib", bytes_of("libfoo.dylib"));
    t.write("Real.app/Contents/Frameworks/libweak.dylib", bytes_of("libweak.dylib"));
    auto info = bundle::inspect(t.path() / "Real.app");
    REQUIRE(info.ok());
    CHECK(info.value().executable_found);
    auto bytes = read_file(info.value().executable_path, 1 << 26).value();
    auto file = macho::parse_file(ByteView(bytes)).value();
    auto rep = analysis::analyze_dependencies(*file.slices[0].image, info.value().executable_path, info.value().root);
    size_t bundled = 0, system = 0;
    for (auto& d : rep.deps) {
        bundled += d.cls == analysis::DepClass::Bundled;
        system += d.cls == analysis::DepClass::System;
        CHECK(d.cls != analysis::DepClass::Missing);
    }
    CHECK_EQ(bundled, size_t{2});
    CHECK_EQ(rep.images_scanned, size_t{3});
    CHECK(system >= 1);
    auto compat = analysis::assess(file, &rep, &info.value());
    CHECK(compat.status == analysis::CompatStatus::AnalysisCompatible);
    CHECK(!compat.execution_confirmed);

    // without the frameworks the same executable reports a missing required dependency
    TempDir t2;
    t2.write("Real.app/Contents/Info.plist", xml_plist({{"CFBundleExecutable", "app"}}));
    t2.write("Real.app/Contents/MacOS/app", bytes_of("app_x86_64"));
    auto info2 = bundle::inspect(t2.path() / "Real.app").value();
    auto rep2 = analysis::analyze_dependencies(*file.slices[0].image, info2.executable_path, info2.root);
    CHECK_EQ(analysis::assess(file, &rep2, &info2).status, analysis::CompatStatus::PendingDependencies);
}

// ---- regression tests for bugs found by real fixtures ---------------------------------------------------------
CIDER_TEST(real_truncations_and_bitflips_of_real_binaries_never_crash) {
    for (const char* n : {"hello_x86_64", "hello_universal", "app_x86_64", "hello_signed_x86_64"}) {
        auto good = bytes_of(n);
        for (size_t cut = 0; cut < good.size(); cut += 61) {
            std::vector<uint8_t> c(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
            macho::ParseOptions o;
            o.parse_symbols = true;
            (void)macho::parse_file(ByteView(c), o);
        }
        for (size_t pos = 0; pos < std::min<size_t>(good.size(), 2048); ++pos) {
            auto b = good;
            b[pos] ^= 0xFF;
            macho::ParseOptions o;
            o.parse_symbols = true;
            (void)macho::parse_file(ByteView(b), o);
        }
    }
    CHECK(true);
}

// Regression for a crash found by libFuzzer (tests/fuzz/fuzz_macho.cpp, 2026-10-10): LC_SYMTAB with
// strsize == 0 skipped offset validation, then parse_symbols sliced at the unvalidated stroff and
// threw std::bad_variant_access. The input is the unminimised crashing file.
CIDER_TEST(regression_fuzz_crash_symtab_zero_strsize) {
    auto r = read_file(fs::path(CIDER_REAL_FIXTURES_DIR).parent_path() / "regression" / "fuzz-macho-strsize0-bad-variant-access.bin", 1 << 20);
    REQUIRE(r.ok());
    macho::ParseOptions o;
    o.parse_symbols = true;
    (void)macho::parse_file(ByteView(r.value()), o);  // must not throw or crash
    CHECK(true);
}

// ---- real-world applications (optional, not downloaded by the test-suite) ----------------------------------------
// tools/validate-real-dmgs.ps1 downloads official open-source release DMGs, extracts them with
// `cider extract` and points CIDER_REAL_APPS_DIR at the result. Every Mach-O file found there is
// parsed by CIDER and compared, slice by slice, with live llvm-objdump output.
namespace {
bool looks_macho(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    uint8_t m[4] = {};
    f.read(reinterpret_cast<char*>(m), 4);
    return f.gcount() == 4 && macho::identify(ByteView(m, 4)) != macho::Format::Unknown;
}

std::string run_objdump(const std::vector<std::string>& args) {
    auto r = run_process(*objdump_exe(), args, std::chrono::seconds(120), 256u << 20);
    return r.ok() ? r.value().output : std::string();
}
}  // namespace

CIDER_TEST(real_world_extracted_apps_match_llvm_objdump) {
    const char* dir = std::getenv("CIDER_REAL_APPS_DIR");
    if (!dir) SKIP("CIDER_REAL_APPS_DIR not set (see tools/validate-real-dmgs.ps1)");
    if (!objdump_exe()) SKIP("llvm-objdump not found (set CIDER_LLVM_BIN)");
    size_t files = 0, slices = 0, sym_checked = 0, failures_before = static_cast<size_t>(failure_count());
    std::error_code ec;
    for (fs::recursive_directory_iterator it(extended_path(path_from_utf8(dir)), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !looks_macho(it->path())) continue;
        auto bytes = read_file(it->path(), 512u << 20);
        if (!bytes.ok()) continue;
        macho::ParseOptions po;
        po.parse_symbols = true;
        auto f = macho::parse_file(ByteView(bytes.value()), po);
        std::string label = path_to_utf8(it->path().filename());
        if (!f.ok()) { report_failure(__FILE__, __LINE__, label + ": CIDER rejected a real file: " + f.error().message); continue; }
        ++files;
        for (auto& sl : f.value().slices) {
            std::string arch = macho::cpu_name(sl.cputype, sl.cpusubtype);
            if (!sl.image) { report_failure(__FILE__, __LINE__, label + " [" + arch + "]: slice rejected: " + (sl.error ? sl.error->message : "")); continue; }
            ++slices;
            std::string path = path_to_utf8(it->path());
            std::vector<std::string> base = {"--macho"};
            if (f.value().fat) base.push_back("--arch=" + arch);
            auto ph = base; ph.push_back("--private-headers"); ph.push_back(path);
            std::string text = run_objdump(ph);
            if (text.empty()) { report_failure(__FILE__, __LINE__, label + ": llvm-objdump produced no output"); continue; }
            compare_image(label + " [" + arch + "]", parse_private_headers(text), *sl.image, /*strict=*/false);
            if (sl.image->symtab && sl.image->symtab->nsyms < 20000) {
                auto sy = base; sy.push_back("--syms"); sy.push_back(path);
                compare_symbols(label + " [" + arch + "]", run_objdump(sy), *sl.image);
                ++sym_checked;
            }
        }
    }
    std::printf("    real-world: %zu Mach-O file(s), %zu slice(s) compared with llvm-objdump (%zu with symbol tables)\n", files, slices, sym_checked);
    CHECK(files > 0);
    CHECK_EQ(static_cast<size_t>(failure_count()), failures_before);
}
