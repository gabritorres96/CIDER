#pragma once
// Synthetic DMG fixtures: a minimal ISO 9660 + Joliet writer (the filesystem hdiutil produces for
// "hybrid" images) wrapped in a UDIF container (raw or zlib-"stored" chunks).
//
// These are NOT produced by Apple tools. They exercise CIDER's UDIF parser and the 7-Zip backend;
// they say nothing about HFS+/APFS support (no HFS+/APFS writer exists in this project).
#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "macho_builder.hpp"

namespace cider::testing {

struct FsNode {
    std::string name;
    bool dir = false;
    std::vector<uint8_t> data;
    std::vector<FsNode> children;
};

inline FsNode dir(const std::string& name, std::vector<FsNode> kids = {}) { return FsNode{name, true, {}, std::move(kids)}; }
inline FsNode file(const std::string& name, std::vector<uint8_t> data) { return FsNode{name, false, std::move(data), {}}; }
inline FsNode file(const std::string& name, const std::string& text) { return file(name, std::vector<uint8_t>(text.begin(), text.end())); }

namespace detail {

constexpr uint32_t kSector = 2048;

struct N {
    int parent = -1;
    bool dir = false;
    std::string name, iso_name;
    const std::vector<uint8_t>* data = nullptr;
    std::vector<int> kids;
    uint32_t iso_lba = 0, iso_size = 0, jol_lba = 0, jol_size = 0, data_lba = 0;
};

inline void both32(Buf& b, uint32_t v) { b.u32(v); b.be32(v); }
inline void both16(Buf& b, uint16_t v) { b.u8(static_cast<uint8_t>(v)); b.u8(static_cast<uint8_t>(v >> 8)); b.u8(static_cast<uint8_t>(v >> 8)); b.u8(static_cast<uint8_t>(v)); }

inline std::string ucs2be(const std::string& s) {
    std::string o;
    for (unsigned char c : s) { o.push_back('\0'); o.push_back(static_cast<char>(c)); }  // ASCII names only
    return o;
}

inline void record(Buf& b, const std::string& id, uint32_t lba, uint32_t size, bool is_dir) {
    size_t start = b.size();
    uint8_t len_fi = static_cast<uint8_t>(id.size());
    uint8_t len = static_cast<uint8_t>(33 + len_fi + ((len_fi % 2) == 0 ? 1 : 0));
    b.u8(len); b.u8(0);
    both32(b, lba); both32(b, size);
    for (int i = 0; i < 7; ++i) b.u8(i == 0 ? 124 : (i == 1 || i == 2) ? 1 : 0);  // 2024-01-01 00:00:00 GMT
    b.u8(is_dir ? 2 : 0); b.u8(0); b.u8(0);
    both16(b, 1);
    b.u8(len_fi);
    for (char c : id) b.u8(static_cast<uint8_t>(c));
    while (b.size() - start < len) b.u8(0);
}

inline std::vector<uint8_t> dir_bytes(const std::vector<N>& n, int idx, bool joliet) {
    Buf out, cur;
    auto lba_of = [&](int i) { return joliet ? n[i].jol_lba : n[i].iso_lba; };
    auto size_of = [&](int i) { return joliet ? n[i].jol_size : n[i].iso_size; };
    auto flush_if_needed = [&](const Buf& rec) {
        if (cur.size() % kSector + rec.size() > kSector) cur.zeros(kSector - cur.size() % kSector);
    };
    auto add = [&](const std::string& id, uint32_t lba, uint32_t size, bool d) {
        Buf rec;
        record(rec, id, lba, size, d);
        flush_if_needed(rec);
        cur.bytes(rec.v);
    };
    int parent = n[idx].parent < 0 ? idx : n[idx].parent;
    add(std::string(1, '\0'), lba_of(idx), size_of(idx), true);
    add(std::string(1, '\1'), lba_of(parent), size_of(parent), true);
    for (int k : n[idx].kids) {
        std::string id = joliet ? ucs2be(n[k].name) : n[k].iso_name;
        add(id, n[k].dir ? lba_of(k) : n[k].data_lba, n[k].dir ? size_of(k) : static_cast<uint32_t>(n[k].data->size()), n[k].dir);
    }
    cur.pad_to(kSector);
    return cur.v;
}

inline std::vector<uint8_t> path_table(const std::vector<N>& n, const std::vector<int>& bfs, bool joliet, bool big) {
    Buf b;
    std::vector<int> number(n.size(), 0);
    for (size_t i = 0; i < bfs.size(); ++i) number[bfs[i]] = static_cast<int>(i) + 1;
    for (int idx : bfs) {
        std::string id = idx == 0 ? std::string(1, '\0') : (joliet ? ucs2be(n[idx].name) : n[idx].iso_name);
        uint32_t lba = joliet ? n[idx].jol_lba : n[idx].iso_lba;
        uint16_t parent = idx == 0 ? 1 : static_cast<uint16_t>(number[n[idx].parent]);
        b.u8(static_cast<uint8_t>(id.size())); b.u8(0);
        if (big) b.be32(lba); else b.u32(lba);
        if (big) { b.u8(static_cast<uint8_t>(parent >> 8)); b.u8(static_cast<uint8_t>(parent)); }
        else { b.u8(static_cast<uint8_t>(parent)); b.u8(static_cast<uint8_t>(parent >> 8)); }
        for (char c : id) b.u8(static_cast<uint8_t>(c));
        if (id.size() % 2) b.u8(0);
    }
    return b.v;
}

inline void fixed_text(Buf& b, const std::string& s, size_t width, uint8_t pad = ' ') {
    for (size_t i = 0; i < width; ++i) b.u8(i < s.size() ? static_cast<uint8_t>(s[i]) : pad);
}

inline void volume_descriptor(Buf& b, uint8_t type, const std::string& volume, uint32_t total_sectors, uint32_t path_size,
                              uint32_t l_lba, uint32_t m_lba, const N& root, bool joliet) {
    size_t start = b.size();
    b.u8(type); fixed_text(b, "CD001", 5); b.u8(1); b.u8(0);
    fixed_text(b, "", 32);                                                    // system id
    if (joliet) { for (char c : volume) { b.u8(0); b.u8(static_cast<uint8_t>(c)); } for (size_t i = volume.size() * 2; i < 32; i += 2) { b.u8(0); b.u8(' '); } }
    else fixed_text(b, volume, 32);                                           // volume id
    b.zeros(8);
    both32(b, total_sectors);
    if (joliet) { fixed_text(b, "%/E", 32, 0); } else b.zeros(32);
    both16(b, 1); both16(b, 1); both16(b, kSector);
    both32(b, path_size);
    b.u32(l_lba); b.u32(0); b.be32(m_lba); b.be32(0);
    record(b, std::string(1, '\0'), joliet ? root.jol_lba : root.iso_lba, joliet ? root.jol_size : root.iso_size, true);
    fixed_text(b, "", 128); fixed_text(b, "", 128); fixed_text(b, "", 128); fixed_text(b, "CIDER TEST FIXTURE", 128);
    fixed_text(b, "", 37); fixed_text(b, "", 37); fixed_text(b, "", 37);
    for (int i = 0; i < 4; ++i) { fixed_text(b, "2024010100000000", 16, '0'); b.u8(0); }
    b.u8(1); b.u8(0);
    b.zeros(kSector - (b.size() - start));
}

}  // namespace detail

inline std::vector<uint8_t> build_iso(const FsNode& root_node, const std::string& volume = "CIDERTEST") {
    using namespace detail;
    std::vector<N> n;
    std::vector<int> bfs;
    // flatten (pre-order for storage, BFS order computed separately)
    struct Walk { static void run(const FsNode& f, int parent, std::vector<N>& n, int& file_counter, int& dir_counter) {
        int idx = static_cast<int>(n.size());
        N node;
        node.parent = parent; node.dir = f.dir; node.name = f.name; node.data = &f.data;
        node.iso_name = parent < 0 ? "" : f.dir ? std::format("D{:04}", dir_counter++) : std::format("F{:04}.;1", file_counter++);
        n.push_back(node);
        if (parent >= 0) n[parent].kids.push_back(idx);
        for (auto& c : f.children) run(c, idx, n, file_counter, dir_counter);
    } };
    int fc = 1, dc = 1;
    Walk::run(root_node, -1, n, fc, dc);
    bfs.push_back(0);
    for (size_t i = 0; i < bfs.size(); ++i)
        for (int k : n[bfs[i]].kids) if (n[k].dir) bfs.push_back(k);

    // directory sizes (record sizes do not depend on LBAs)
    for (size_t i = 0; i < n.size(); ++i) {
        if (!n[i].dir) continue;
        n[i].iso_size = static_cast<uint32_t>(dir_bytes(n, static_cast<int>(i), false).size());
        n[i].jol_size = static_cast<uint32_t>(dir_bytes(n, static_cast<int>(i), true).size());
    }
    uint32_t lba = 19 + 4;  // 16 system, PVD, SVD, terminator, 4 path-table sectors
    auto sectors = [](uint64_t bytes) { return static_cast<uint32_t>((bytes + kSector - 1) / kSector); };
    for (auto& x : n) if (x.dir) { x.iso_lba = lba; lba += sectors(x.iso_size); }
    for (auto& x : n) if (x.dir) { x.jol_lba = lba; lba += sectors(x.jol_size); }
    for (auto& x : n) if (!x.dir) { x.data_lba = lba; lba += std::max<uint32_t>(1, sectors(x.data->size())); }
    const uint32_t total = lba;

    auto iso_l = path_table(n, bfs, false, false), iso_m = path_table(n, bfs, false, true);
    auto jol_l = path_table(n, bfs, true, false), jol_m = path_table(n, bfs, true, true);

    Buf out;
    out.zeros(16 * kSector);
    volume_descriptor(out, 1, volume, total, static_cast<uint32_t>(iso_l.size()), 19, 20, n[0], false);
    volume_descriptor(out, 2, volume, total, static_cast<uint32_t>(jol_l.size()), 21, 22, n[0], true);
    out.u8(255); detail::fixed_text(out, "CD001", 5); out.u8(1); out.zeros(kSector - 7);
    for (auto* t : {&iso_l, &iso_m, &jol_l, &jol_m}) { out.bytes(*t); out.pad_to(kSector); }
    for (size_t i = 0; i < n.size(); ++i) if (n[i].dir) { auto d = dir_bytes(n, static_cast<int>(i), false); out.bytes(d); }
    for (size_t i = 0; i < n.size(); ++i) if (n[i].dir) { auto d = dir_bytes(n, static_cast<int>(i), true); out.bytes(d); }
    for (auto& x : n) if (!x.dir) { out.bytes(*x.data); out.pad_to(kSector); if (x.data->empty()) out.zeros(kSector); }
    return out.v;
}

// ---- UDIF --------------------------------------------------------------------------------
enum class DmgCodec { Raw, ZlibStored };

enum class DmgCorrupt { None, BadMishMagic, ChunkPastFork, ChunkSectorsPastPartition, HugeChunkCount, TruncatedMish };

struct DmgOptions {
    DmgCorrupt corrupt = DmgCorrupt::None;
    bool real_layout = false;  // add the metadata partitions hdiutil writes (MBR, GPT header, free space)
    DmgCodec codec = DmgCodec::Raw;
    std::string partition_name = "disk image (CIDER_ISO : 1)";
    uint32_t chunk_sectors = 2048;
    uint32_t version = 4;
    uint32_t header_size = 512;
    bool include_blkx = true;
};

inline uint32_t crc32(const std::vector<uint8_t>& d) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t b : d) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

inline std::vector<uint8_t> zlib_stored(const std::vector<uint8_t>& d) {
    Buf b;
    b.u8(0x78); b.u8(0x01);
    size_t pos = 0;
    do {
        size_t n = std::min<size_t>(65535, d.size() - pos);
        bool last = pos + n >= d.size();
        b.u8(last ? 1 : 0);
        b.u8(static_cast<uint8_t>(n)); b.u8(static_cast<uint8_t>(n >> 8));
        b.u8(static_cast<uint8_t>(~n)); b.u8(static_cast<uint8_t>((~n) >> 8));
        b.v.insert(b.v.end(), d.begin() + static_cast<std::ptrdiff_t>(pos), d.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    } while (pos < d.size());
    uint32_t a = 1, s2 = 0;
    for (uint8_t x : d) { a = (a + x) % 65521; s2 = (s2 + a) % 65521; }
    b.be32((s2 << 16) | a);
    return b.v;
}

inline std::string base64(const std::vector<uint8_t>& d) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < d.size(); i += 3) {
        uint32_t v = d[i] << 16 | (i + 1 < d.size() ? d[i + 1] << 8 : 0) | (i + 2 < d.size() ? d[i + 2] : 0);
        o.push_back(t[(v >> 18) & 63]); o.push_back(t[(v >> 12) & 63]);
        o.push_back(i + 1 < d.size() ? t[(v >> 6) & 63] : '='); o.push_back(i + 2 < d.size() ? t[v & 63] : '=');
    }
    return o;
}

struct Dmg {
    std::vector<uint8_t> bytes;
    size_t fork_length = 0, xml_offset = 0, xml_length = 0, mish_offset_in_xml = 0;
};

inline Dmg build_dmg(std::vector<uint8_t> image, const DmgOptions& o = {}) {
    image.resize((image.size() + 511) / 512 * 512, 0);
    Buf fork;
    uint64_t total_sectors = 0;

    // One blkx entry (mish block table + data chunks appended to the shared data fork).
    auto blkx = [&](const std::string& name, int id, std::vector<uint8_t> data, DmgCodec codec, DmgCorrupt corrupt, bool free_space = false,
                    uint64_t free_sectors = 0) -> std::string {
        data.resize((data.size() + 511) / 512 * 512, 0);
        const uint64_t secs = free_space ? free_sectors : data.size() / 512;
        Buf chunks;
        uint32_t nchunks = 0;
        if (free_space) {
            chunks.be32(2u); chunks.be32(0); chunks.be64(0); chunks.be64(secs); chunks.be64(fork.size()); chunks.be64(0);  // "ignore" chunk
            ++nchunks;
        } else {
            for (uint64_t s = 0; s < secs; s += o.chunk_sectors) {
                uint64_t cnt = std::min<uint64_t>(o.chunk_sectors, secs - s);
                std::vector<uint8_t> raw(data.begin() + static_cast<std::ptrdiff_t>(s * 512), data.begin() + static_cast<std::ptrdiff_t>((s + cnt) * 512));
                std::vector<uint8_t> stored = codec == DmgCodec::Raw ? raw : zlib_stored(raw);
                chunks.be32(codec == DmgCodec::Raw ? 1u : 0x80000005u); chunks.be32(0);
                chunks.be64(s); chunks.be64(cnt); chunks.be64(fork.size()); chunks.be64(stored.size());
                fork.bytes(stored);
                ++nchunks;
            }
        }
        chunks.be32(0xffffffffu); chunks.be32(0); chunks.be64(secs); chunks.be64(0); chunks.be64(fork.size()); chunks.be64(0);
        ++nchunks;
        Buf mish;
        mish.be32(0x6d697368); mish.be32(1); mish.be64(total_sectors); mish.be64(secs); mish.be64(0);
        mish.be32(o.chunk_sectors); mish.be32(0); mish.zeros(24);
        mish.be32(2); mish.be32(32); mish.be32(crc32(data)); mish.zeros(31 * 4);
        mish.be32(nchunks);
        mish.bytes(chunks.v);
        auto be_patch = [&](size_t off, uint64_t v, int bytes) { for (int i = 0; i < bytes; ++i) mish.v[off + i] = static_cast<uint8_t>(v >> (8 * (bytes - 1 - i))); };
        switch (corrupt) {
            case DmgCorrupt::None: break;
            case DmgCorrupt::BadMishMagic: mish.v[0] = 0; break;
            case DmgCorrupt::ChunkPastFork: be_patch(204 + 32, 0x100000000ull, 8); break;
            case DmgCorrupt::ChunkSectorsPastPartition: be_patch(204 + 16, secs + 10, 8); break;
            case DmgCorrupt::HugeChunkCount: be_patch(200, 0x7fffffffu, 4); break;
            case DmgCorrupt::TruncatedMish: mish.v.resize(150); break;
        }
        total_sectors += secs;
        return "\t\t\t<dict>\n\t\t\t\t<key>Attributes</key>\n\t\t\t\t<string>0x0050</string>\n\t\t\t\t<key>CFName</key>\n\t\t\t\t<string>" + name +
               "</string>\n\t\t\t\t<key>Data</key>\n\t\t\t\t<data>\n" + base64(mish.v) + "\n\t\t\t\t</data>\n\t\t\t\t<key>ID</key>\n\t\t\t\t<string>" +
               std::to_string(id) + "</string>\n\t\t\t\t<key>Name</key>\n\t\t\t\t<string>" + name + "</string>\n\t\t\t</dict>\n";
    };

    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n\t<key>resource-fork</key>\n\t<dict>\n"
                      "\t\t<key>blkx</key>\n\t\t<array>\n";
    if (o.include_blkx) {
        int id = 0;
        if (o.real_layout) {
            // what hdiutil writes around the filesystem: protective MBR, GPT header/table, free space
            std::vector<uint8_t> mbr(512, 0);
            mbr[510] = 0x55; mbr[511] = 0xAA;
            xml += blkx("Protective Master Boot Record (MBR : 0)", id++, mbr, DmgCodec::ZlibStored, DmgCorrupt::None);
            std::vector<uint8_t> gpt(512, 0);
            std::copy_n("EFI PART", 8, gpt.begin());
            xml += blkx("GPT Header (Primary GPT Header : 1)", id++, gpt, DmgCodec::ZlibStored, DmgCorrupt::None);
            xml += blkx(" (Apple_Free : 2)", id++, {}, DmgCodec::Raw, DmgCorrupt::None, true, 6);
        }
        xml += blkx(o.partition_name, id++, image, o.codec, o.corrupt);
        if (o.real_layout) xml += blkx(" (Apple_Free : 4)", id++, {}, DmgCodec::Raw, DmgCorrupt::None, true, 5);
    }
    xml += "\t\t</array>\n\t</dict>\n</dict>\n</plist>\n";

    Dmg d;
    Buf f;
    f.bytes(fork.v);
    d.fork_length = fork.size();
    d.xml_offset = f.size();
    d.xml_length = xml.size();
    f.v.insert(f.v.end(), xml.begin(), xml.end());
    // koly trailer (all big-endian)
    Buf k;
    k.be32(0x6b6f6c79); k.be32(o.version); k.be32(o.header_size); k.be32(1);
    k.be64(0); k.be64(0); k.be64(d.fork_length);
    k.be64(0); k.be64(0);
    k.be32(0); k.be32(1);
    k.zeros(16);
    k.be32(0); k.be32(0); k.zeros(128);
    k.be64(d.xml_offset); k.be64(d.xml_length);
    k.zeros(120);
    k.be32(0); k.be32(0); k.zeros(128);
    k.be32(1);
    k.be64(total_sectors);
    k.be32(0); k.be32(0); k.be32(0);
    f.bytes(k.v);
    d.bytes = std::move(f.v);
    return d;
}

// Patch helpers for malformed-image tests (offsets from the END of the file into the koly trailer).
inline void patch_koly32(std::vector<uint8_t>& b, size_t koly_field_off, uint32_t v) {
    size_t base = b.size() - 512 + koly_field_off;
    for (int i = 0; i < 4; ++i) b[base + i] = static_cast<uint8_t>(v >> (8 * (3 - i)));
}
inline void patch_koly64(std::vector<uint8_t>& b, size_t koly_field_off, uint64_t v) {
    size_t base = b.size() - 512 + koly_field_off;
    for (int i = 0; i < 8; ++i) b[base + i] = static_cast<uint8_t>(v >> (8 * (7 - i)));
}

// A hybrid "installer" image with one app (Mach-O + Info.plist + an embedded framework).
inline FsNode example_app_tree(const std::vector<uint8_t>& exe = build_macho({}).bytes) {
    MachoSpec lib;
    lib.filetype = 6;
    lib.main = false;
    lib.id_dylib = "@rpath/Foo.framework/Versions/A/Foo";
    return dir("", {
        file("README.txt", "hello"),
        dir("Example.app", {dir("Contents", {
            file("Info.plist", xml_plist({{"CFBundleExecutable", "Example"}, {"CFBundleIdentifier", "com.example.dmgapp"},
                                          {"CFBundleName", "Example"}, {"CFBundleVersion", "7"}, {"CFBundleShortVersionString", "2.0"},
                                          {"CFBundlePackageType", "APPL"}})),
            dir("MacOS", {file("Example", exe)}),
            dir("Frameworks", {dir("Foo.framework", {dir("Versions", {dir("A", {file("Foo", build_macho(lib).bytes)})})})}),
            dir("Resources", {file("icon.txt", "icon")})})}),
    });
}

}  // namespace cider::testing
