#pragma once
// Parsing of llvm-objdump output (--private-headers, --syms) and field-by-field comparison with CIDER's
// Mach-O parser. Shared by the real-fixture, real-world and execution-fixture tests.
#include <algorithm>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "cider/macho.hpp"
#include "cider/util.hpp"
#include "test_framework.hpp"

namespace cider::testing::objdump {

using namespace cider;

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


}  // namespace cider::testing::objdump
