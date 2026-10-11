#pragma once
// Builds small, deterministic, redistributable Mach-O / FAT / plist fixtures in memory.
// These are SYNTHETIC files written from the public Mach-O format description; they are not
// produced by ld64 or clang (no Apple toolchain is available on Windows CI). They contain
// harmless placeholder code bytes and are only ever parsed, never executed.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace orvyn::testing {

class Buf {
public:
    std::vector<uint8_t> v;
    size_t size() const { return v.size(); }
    void u8(uint8_t x) { v.push_back(x); }
    void u32(uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
    void u64(uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
    void be32(uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
    void be64(uint64_t x) { for (int i = 7; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
    void zeros(size_t n) { v.insert(v.end(), n, 0); }
    void bytes(const std::vector<uint8_t>& b) { v.insert(v.end(), b.begin(), b.end()); }
    void cstr(const std::string& s) { v.insert(v.end(), s.begin(), s.end()); v.push_back(0); }
    void fixed(const std::string& s, size_t width) {
        for (size_t i = 0; i < width; ++i) v.push_back(i < s.size() ? static_cast<uint8_t>(s[i]) : 0);
    }
    void pad_to(size_t align) { while (v.size() % align) v.push_back(0); }
    void patch32(size_t off, uint32_t x) { for (int i = 0; i < 4; ++i) v[off + i] = static_cast<uint8_t>(x >> (8 * i)); }
    void patch64(size_t off, uint64_t x) { for (int i = 0; i < 8; ++i) v[off + i] = static_cast<uint8_t>(x >> (8 * i)); }
};

inline void patch32(std::vector<uint8_t>& b, size_t off, uint32_t x) { for (int i = 0; i < 4; ++i) b[off + i] = static_cast<uint8_t>(x >> (8 * i)); }
inline void patch64(std::vector<uint8_t>& b, size_t off, uint64_t x) { for (int i = 0; i < 8; ++i) b[off + i] = static_cast<uint8_t>(x >> (8 * i)); }

struct MachoSpec {
    uint32_t cputype = 0x01000007, cpusubtype = 3, filetype = 2, flags = 0x200085;
    std::vector<std::string> dylibs{"/usr/lib/libSystem.B.dylib"};
    std::vector<std::string> weak_dylibs;
    std::vector<std::string> reexport_dylibs;
    std::vector<std::string> rpaths;
    std::string id_dylib;  // LC_ID_DYLIB when non-empty
    bool main = true, unixthread = false, symtab = true, dysymtab = true, dyld_info = true, build_version = true,
         uuid = true, dylinker = true, code_signature = false, encrypted = false, legacy_version_min = false;
    uint32_t platform = 1, minos = 0x000a0f00, sdk = 0x000c0000;
    std::vector<std::string> defined_symbols{"_main"};
    std::vector<std::string> undefined_symbols{"_puts"};
};

struct Built {
    std::vector<uint8_t> bytes;
    std::map<std::string, size_t> cmd;  // load-command label -> absolute file offset
    size_t text_section_off = 0;        // file offset of __text
    size_t linkedit_off = 0;
};

constexpr size_t kTextSize = 0x1000, kTextAddr = 0x100000000ull;

inline void lc_begin(Buf& b, Built& out, const std::string& label, uint32_t cmd, uint32_t size) {
    out.cmd[label] = b.size();
    b.u32(cmd);
    b.u32(size);
}

inline uint32_t padded(size_t n) { return static_cast<uint32_t>((n + 7) & ~size_t{7}); }

inline void lc_string_cmd(Buf& b, Built& out, const std::string& label, uint32_t cmd, const std::string& s, bool dylib) {
    uint32_t fixed = dylib ? 24 : 12;
    uint32_t size = padded(fixed + s.size() + 1);
    size_t start = b.size();
    lc_begin(b, out, label, cmd, size);
    b.u32(fixed);  // name/path offset
    if (dylib) { b.u32(2); b.u32(0x00010000); b.u32(0x00010000); }
    b.cstr(s);
    while (b.size() - start < size) b.u8(0);
}

inline Built build_macho(const MachoSpec& s) {
    Built out;
    // ---- __LINKEDIT payload first so offsets are known
    Buf le;
    const size_t le_base = 0x1000;
    out.linkedit_off = le_base;
    uint32_t export_off = 0, export_size = 0;
    if (s.dyld_info) { export_off = static_cast<uint32_t>(le_base + le.size()); export_size = 8; le.zeros(8); }
    uint32_t symoff = static_cast<uint32_t>(le_base + le.size()), nsyms = 0, stroff = 0, strsize = 0;
    if (s.symtab) {
        Buf strtab;
        strtab.cstr(" ");
        std::vector<uint32_t> strx;
        std::vector<std::string> names = s.defined_symbols;
        names.insert(names.end(), s.undefined_symbols.begin(), s.undefined_symbols.end());
        for (auto& n : names) { strx.push_back(static_cast<uint32_t>(strtab.size())); strtab.cstr(n); }
        for (size_t i = 0; i < names.size(); ++i) {
            bool defined = i < s.defined_symbols.size();
            le.u32(strx[i]);
            le.u8(defined ? 0x0f : 0x01);
            le.u8(defined ? 1 : 0);
            le.v.push_back(0); le.v.push_back(defined ? 0 : 1);  // n_desc: library ordinal 1 in the high byte
            le.u64(defined ? kTextAddr + 0xf00 : 0);
        }
        nsyms = static_cast<uint32_t>(names.size());
        stroff = static_cast<uint32_t>(le_base + le.size());
        strsize = static_cast<uint32_t>(strtab.size());
        le.bytes(strtab.v);
    }
    le.pad_to(16);
    uint32_t cs_off = 0, cs_size = 0;
    if (s.code_signature) {
        cs_off = static_cast<uint32_t>(le_base + le.size());
        Buf cd;
        const std::string ident = "com.example.fixture", team = "TEAM123456";
        uint32_t ident_off = 52, team_off = static_cast<uint32_t>(52 + ident.size() + 1);
        uint32_t hash_off = static_cast<uint32_t>(team_off + team.size() + 1);
        cd.be32(0xfade0c02); cd.be32(0); cd.be32(0x20400); cd.be32(0x2 /*adhoc*/ | 0x10000 /*runtime*/);
        cd.be32(hash_off); cd.be32(ident_off); cd.be32(0); cd.be32(1);
        cd.be32(0x1000); cd.u8(32); cd.u8(2); cd.u8(0); cd.u8(12); cd.be32(0); cd.be32(0); cd.be32(team_off);
        cd.cstr(ident); cd.cstr(team); cd.zeros(32);
        uint32_t cd_len = static_cast<uint32_t>(cd.size());
        for (int i = 0; i < 4; ++i) cd.v[4 + i] = static_cast<uint8_t>(cd_len >> (8 * (3 - i)));
        Buf sb;
        sb.be32(0xfade0cc0); sb.be32(static_cast<uint32_t>(12 + 8 + cd_len)); sb.be32(1);
        sb.be32(0); sb.be32(20);
        sb.bytes(cd.v);
        le.bytes(sb.v);
        cs_size = static_cast<uint32_t>(sb.size());
    }
    const size_t le_filesize = le.size();

    // ---- load commands
    Buf lc;
    Built tmp;  // offsets recorded relative to lc start, rebased later
    uint32_t ncmds = 0;
    auto count = [&] { ++ncmds; };

    // segments
    {
        lc_begin(lc, tmp, "pagezero", 0x19, 72);
        lc.fixed("__PAGEZERO", 16); lc.u64(0); lc.u64(0x100000000ull); lc.u64(0); lc.u64(0);
        lc.u32(0); lc.u32(0); lc.u32(0); lc.u32(0);
        count();
        lc_begin(lc, tmp, "text", 0x19, 72 + 80);
        lc.fixed("__TEXT", 16); lc.u64(kTextAddr); lc.u64(kTextSize); lc.u64(0); lc.u64(kTextSize);
        lc.u32(5); lc.u32(5); lc.u32(1); lc.u32(0);
        lc.fixed("__text", 16); lc.fixed("__TEXT", 16); lc.u64(kTextAddr + 0xf00); lc.u64(6);
        lc.u32(0xf00); lc.u32(4); lc.u32(0); lc.u32(0); lc.u32(0x80000400); lc.u32(0); lc.u32(0); lc.u32(0);
        count();
        lc_begin(lc, tmp, "linkedit", 0x19, 72);
        lc.fixed("__LINKEDIT", 16); lc.u64(kTextAddr + kTextSize); lc.u64(0x1000); lc.u64(le_base); lc.u64(le_filesize);
        lc.u32(1); lc.u32(1); lc.u32(0); lc.u32(0);
        count();
    }
    if (s.dyld_info) {
        lc_begin(lc, tmp, "dyld_info", 0x80000022, 48);
        for (int i = 0; i < 8; ++i) lc.u32(0);       // rebase, bind, weak_bind, lazy_bind: empty
        lc.u32(export_off); lc.u32(export_size);
        count();
    }
    if (s.symtab) {
        lc_begin(lc, tmp, "symtab", 0x2, 24);
        lc.u32(symoff); lc.u32(nsyms); lc.u32(stroff); lc.u32(strsize);
        count();
    }
    if (s.dysymtab) {
        lc_begin(lc, tmp, "dysymtab", 0xb, 80);
        uint32_t nd = static_cast<uint32_t>(s.defined_symbols.size()), nu = static_cast<uint32_t>(s.undefined_symbols.size());
        lc.u32(0); lc.u32(0); lc.u32(0); lc.u32(nd); lc.u32(nd); lc.u32(nu);
        for (int i = 0; i < 12; ++i) lc.u32(0);
        count();
    }
    if (s.dylinker) { lc_string_cmd(lc, tmp, "dylinker", 0xe, "/usr/lib/dyld", false); count(); }
    if (s.uuid) {
        lc_begin(lc, tmp, "uuid", 0x1b, 24);
        for (int i = 0; i < 16; ++i) lc.u8(static_cast<uint8_t>(0x10 + i));
        count();
    }
    if (s.build_version) {
        lc_begin(lc, tmp, "build_version", 0x32, 32);
        lc.u32(s.platform); lc.u32(s.minos); lc.u32(s.sdk); lc.u32(1); lc.u32(3); lc.u32(0x04b00000);
        count();
    } else if (s.legacy_version_min) {
        lc_begin(lc, tmp, "version_min", 0x24, 16);
        lc.u32(s.minos); lc.u32(s.sdk);
        count();
    }
    if (s.main) {
        lc_begin(lc, tmp, "main", 0x80000028, 24);
        lc.u64(0xf00); lc.u64(0);
        count();
    }
    if (s.unixthread) {
        lc_begin(lc, tmp, "unixthread", 0x5, 184);
        lc.u32(4); lc.u32(42);
        for (int r = 0; r < 21; ++r) lc.u64(r == 16 ? kTextAddr + 0xf00 : 0);
        count();
    }
    if (!s.id_dylib.empty()) { lc_string_cmd(lc, tmp, "id_dylib", 0xd, s.id_dylib, true); count(); }
    int n = 0;
    for (auto& d : s.dylibs) { lc_string_cmd(lc, tmp, "dylib" + std::to_string(n++), 0xc, d, true); count(); }
    n = 0;
    for (auto& d : s.weak_dylibs) { lc_string_cmd(lc, tmp, "weak" + std::to_string(n++), 0x80000018, d, true); count(); }
    n = 0;
    for (auto& d : s.reexport_dylibs) { lc_string_cmd(lc, tmp, "reexport" + std::to_string(n++), 0x8000001f, d, true); count(); }
    n = 0;
    for (auto& r : s.rpaths) { lc_string_cmd(lc, tmp, "rpath" + std::to_string(n++), 0x8000001c, r, false); count(); }
    if (s.code_signature) {
        lc_begin(lc, tmp, "code_signature", 0x1d, 16);
        lc.u32(cs_off); lc.u32(cs_size);
        count();
    }
    if (s.encrypted) {
        lc_begin(lc, tmp, "encryption", 0x2c, 24);
        lc.u32(0x1000); lc.u32(0x10); lc.u32(1); lc.u32(0);
        count();
    }

    // ---- assemble
    Buf f;
    f.u32(0xfeedfacf); f.u32(s.cputype); f.u32(s.cpusubtype); f.u32(s.filetype);
    f.u32(ncmds); f.u32(static_cast<uint32_t>(lc.size())); f.u32(s.flags); f.u32(0);
    for (auto& [k, off] : tmp.cmd) out.cmd[k] = 32 + off;
    f.bytes(lc.v);
    if (f.size() > 0xf00) throw std::runtime_error("fixture: load commands too large");
    f.zeros(0xf00 - f.size());
    out.text_section_off = f.size();
    for (int b : {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3}) f.u8(static_cast<uint8_t>(b));  // mov eax,42 ; ret (never executed)
    f.zeros(0x1000 - f.size());
    f.bytes(le.v);
    out.bytes = std::move(f.v);
    return out;
}

struct FatSlice {
    uint32_t cputype, subtype;
    std::vector<uint8_t> bytes;
};

inline std::vector<uint8_t> build_fat(const std::vector<FatSlice>& slices, bool fat64 = false) {
    Buf f;
    f.be32(fat64 ? 0xcafebabf : 0xcafebabe);
    f.be32(static_cast<uint32_t>(slices.size()));
    const size_t entry = fat64 ? 32 : 20;
    size_t off = 0x4000;
    std::vector<size_t> offs;
    for (auto& s : slices) {
        offs.push_back(off);
        f.be32(s.cputype); f.be32(s.subtype);
        if (fat64) { f.be64(off); f.be64(s.bytes.size()); f.be32(14); f.be32(0); }
        else { f.be32(static_cast<uint32_t>(off)); f.be32(static_cast<uint32_t>(s.bytes.size())); f.be32(14); }
        off = (off + s.bytes.size() + 0x3fff) & ~size_t{0x3fff};
    }
    (void)entry;
    for (size_t i = 0; i < slices.size(); ++i) {
        f.zeros(offs[i] - f.size());
        f.bytes(slices[i].bytes);
    }
    return f.v;
}

// ---- plist helpers -----------------------------------------------------------------------
inline std::string xml_plist(const std::vector<std::pair<std::string, std::string>>& kv_string_values) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n";
    for (auto& [k, v] : kv_string_values) s += "  <key>" + k + "</key>\n  <string>" + v + "</string>\n";
    return s + "</dict>\n</plist>\n";
}

// Binary plist: dict of ASCII string -> ASCII string (all strings < 15 chars, < 255 bytes total).
inline std::vector<uint8_t> binary_plist(const std::vector<std::pair<std::string, std::string>>& kv) {
    Buf b;
    for (char c : std::string("bplist00")) b.u8(static_cast<uint8_t>(c));
    std::vector<size_t> offsets;
    size_t n = kv.size();
    offsets.push_back(b.size());  // object 0: dict
    b.u8(static_cast<uint8_t>(0xD0 | n));
    for (size_t i = 0; i < n; ++i) b.u8(static_cast<uint8_t>(1 + i));
    for (size_t i = 0; i < n; ++i) b.u8(static_cast<uint8_t>(1 + n + i));
    auto add = [&](const std::string& s) { offsets.push_back(b.size()); b.u8(static_cast<uint8_t>(0x50 | s.size())); for (char c : s) b.u8(static_cast<uint8_t>(c)); };
    for (auto& [k, v] : kv) add(k);
    for (auto& [k, v] : kv) add(v);
    size_t table = b.size();
    for (size_t o : offsets) b.u8(static_cast<uint8_t>(o));
    for (int i = 0; i < 6; ++i) b.u8(0);
    b.u8(1); b.u8(1);
    b.be64(offsets.size()); b.be64(0); b.be64(table);
    return b.v;
}

// ---- temp directory ----------------------------------------------------------------------
class TempDir {
public:
    TempDir() {
        static int counter = 0;
        ++counter;
        auto tag = std::to_string(reinterpret_cast<uintptr_t>(this) ^ static_cast<uintptr_t>(counter));
        path_ = std::filesystem::temp_directory_path() / ("orvyn-test-" + tag + "-" + std::to_string(counter));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path_, ec); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

    std::filesystem::path write(const std::string& rel, const std::vector<uint8_t>& data) const {
        auto p = path_ / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return p;
    }
    std::filesystem::path write(const std::string& rel, const std::string& text) const {
        return write(rel, std::vector<uint8_t>(text.begin(), text.end()));
    }

private:
    std::filesystem::path path_;
};

// Creates Example.app with the given executable and Info.plist entries. Returns the .app path.
inline std::filesystem::path make_app(const TempDir& t, const std::string& app, const std::vector<uint8_t>& exe,
                                      const std::string& exe_name = "Example") {
    t.write(app + "/Contents/Info.plist",
            xml_plist({{"CFBundleExecutable", exe_name}, {"CFBundleIdentifier", "com.example.app"},
                       {"CFBundleName", "Example"}, {"CFBundleVersion", "42"}, {"CFBundleShortVersionString", "1.2.3"},
                       {"CFBundlePackageType", "APPL"}, {"LSMinimumSystemVersion", "10.15"}}));
    t.write(app + "/Contents/MacOS/" + exe_name, exe);
    t.write(app + "/Contents/Resources/readme.txt", std::string("resource"));
    return t.path() / app;
}

}  // namespace orvyn::testing
