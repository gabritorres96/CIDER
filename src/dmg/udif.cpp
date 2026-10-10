#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>

#include "cider/bytes.hpp"
#include "cider/dmg.hpp"
#include "cider/log.hpp"
#include "cider/plist.hpp"
#include "cider/util.hpp"

namespace fs = std::filesystem;

namespace cider::dmg {
namespace {

constexpr uint64_t kKolySize = 512;
constexpr uint32_t kKolyMagic = 0x6b6f6c79;  // 'koly'
constexpr uint32_t kMishMagic = 0x6d697368;  // 'mish'
constexpr uint64_t kMishHeader = 204, kChunkSize = 40;

// Seekable reader: DMGs can be many GiB, so the file is never read as a whole.
class FileSource {
public:
    explicit FileSource(const fs::path& p) : f_(p, std::ios::binary) {
        std::error_code ec;
        size_ = fs::file_size(p, ec);
        if (ec) f_.close();
    }
    bool ok() const { return f_.is_open(); }
    uint64_t size() const { return size_; }
    Result<std::vector<uint8_t>> read(uint64_t off, uint64_t len) {
        if (off > size_ || len > size_ - off) return Error{ErrorCode::OutOfBounds, std::format("read [{:#x}, +{:#x}) past end of file", off, len)};
        std::vector<uint8_t> buf(static_cast<size_t>(len));
        f_.clear();
        f_.seekg(static_cast<std::streamoff>(off));
        if (len && !f_.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(len)))
            return Error{ErrorCode::Io, "short read"};
        return buf;
    }

private:
    std::ifstream f_;
    uint64_t size_ = 0;
};

bool has(const uint8_t* d, size_t size, size_t off, const char* s) {
    size_t n = std::strlen(s);
    return off <= size && n <= size - off && std::memcmp(d + off, s, n) == 0;
}

Compression compression_of(uint32_t t) {
    switch (t) {
        case 0x00000000: return Compression::Zero;
        case 0x00000001: return Compression::Raw;
        case 0x00000002: return Compression::Ignore;
        case 0x80000004: return Compression::Adc;
        case 0x80000005: return Compression::Zlib;
        case 0x80000006: return Compression::Bzip2;
        case 0x80000007: return Compression::Lzfse;
        case 0x80000008: return Compression::Lzma;
        default: return Compression::Unknown;
    }
}

std::string declared_type_of(const std::string& name) {
    // "disk image (Apple_HFS : 2)" -> "Apple_HFS"
    auto open = name.rfind('(');
    auto colon = name.rfind(" : ");
    if (open == std::string::npos || colon == std::string::npos || colon < open) return {};
    std::string t = name.substr(open + 1, colon - open - 1);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t;
}

std::string filesystem_from_declared(const std::string& t) {
    if (t == "Apple_HFS") return "HFS+";
    if (t == "Apple_HFSX") return "HFSX";
    if (t == "Apple_APFS") return "APFS";
    return {};
}

void add_unique(std::vector<std::string>& v, const std::string& s) {
    if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
}

Error malformed(std::string m) { return Error{ErrorCode::Malformed, std::move(m)}; }

Result<Partition> parse_blkx(const plist::Value& entry, const UdifTrailer& u, uint64_t file_size, FileSource& src,
                             const InspectLimits& lim, std::vector<std::string>& warnings) {
    Partition p;
    p.name = entry.get_string("Name").value_or(entry.get_string("CFName").value_or(""));
    p.id = entry.get_string("ID").value_or("");
    p.declared_type = declared_type_of(p.name);
    const plist::Value* data = entry.get("Data");
    if (!data || data->type != plist::Value::Type::Data) return malformed("blkx entry '" + p.name + "' has no Data");
    ByteView mish(data->data);
    if (mish.size() < kMishHeader) return malformed("blkx '" + p.name + "': mish block truncated");
    if (mish.peek_u32(0, true) != kMishMagic) return malformed("blkx '" + p.name + "': bad 'mish' signature");
    uint64_t sector_count = mish.peek_u64(16, true);
    uint64_t data_offset = mish.peek_u64(24, true);
    uint32_t nchunks = mish.peek_u32(200, true);
    p.sector_count = sector_count;
    if (nchunks > lim.max_chunks_per_partition)
        return Error{ErrorCode::LimitExceeded, std::format("blkx '{}': {} chunks exceed limit", p.name, nchunks)};
    auto table = checked_mul(nchunks, kChunkSize);
    if (!table || *table > mish.size() - kMishHeader) return malformed("blkx '" + p.name + "': chunk table exceeds the mish block");

    uint64_t fork_end = u.data_fork_offset + u.data_fork_length;  // validated against file size earlier
    bool first_chunk_checked = false;
    for (uint32_t i = 0; i < nchunks; ++i) {
        uint64_t o = kMishHeader + kChunkSize * i;
        uint32_t type = mish.peek_u32(o, true);
        uint64_t sec_no = mish.peek_u64(o + 8, true), sec_cnt = mish.peek_u64(o + 16, true);
        uint64_t off = mish.peek_u64(o + 24, true), len = mish.peek_u64(o + 32, true);
        if (type == 0xffffffff) break;       // terminator
        if (type == 0x7ffffffe) continue;    // comment
        Compression c = compression_of(type);
        if (c == Compression::Unknown) { add_unique(warnings, std::format("blkx '{}': unknown chunk type {:#x}", p.name, type)); }
        auto end_sector = checked_add(sec_no, sec_cnt);
        if (!end_sector || *end_sector > sector_count)
            return Error{ErrorCode::OutOfBounds, std::format("blkx '{}': chunk {} sectors outside the partition", p.name, i)};
        bool carries_data = c == Compression::Raw || c == Compression::Adc || c == Compression::Zlib ||
                            c == Compression::Bzip2 || c == Compression::Lzfse || c == Compression::Lzma;
        if (carries_data) {
            auto start = checked_add(u.data_fork_offset, data_offset);
            if (start) start = checked_add(*start, off);
            auto end = start ? checked_add(*start, len) : std::nullopt;
            if (!end || *end > file_size || (u.data_fork_length && *end > fork_end))
                return Error{ErrorCode::OutOfBounds, std::format("blkx '{}': chunk {} data lies outside the data fork", p.name, i)};
            if (c == Compression::Raw && sec_no == 0 && !first_chunk_checked) {
                first_chunk_checked = true;
                auto head = src.read(*start, std::min<uint64_t>(len, 40 * 1024));
                if (head.ok()) {
                    std::string fsname = detect_filesystem(head.value().data(), head.value().size());
                    if (!fsname.empty()) { p.filesystem = fsname; p.filesystem_verified = true; }
                }
            }
        }
        auto it = std::find_if(p.compression.begin(), p.compression.end(), [&](auto& e) { return e.first == c; });
        if (it == p.compression.end()) { p.compression.push_back({c, {}}); it = p.compression.end() - 1; }
        it->second.chunks += 1;
        it->second.sectors += sec_cnt;
    }
    if (p.filesystem.empty()) p.filesystem = filesystem_from_declared(p.declared_type);
    return p;
}

Result<UdifTrailer> parse_koly(ByteView k) {
    UdifTrailer u;
    u.version = k.peek_u32(4, true);
    u.header_size = k.peek_u32(8, true);
    u.flags = k.peek_u32(12, true);
    u.running_data_fork_offset = k.peek_u64(16, true);
    u.data_fork_offset = k.peek_u64(24, true);
    u.data_fork_length = k.peek_u64(32, true);
    u.rsrc_fork_offset = k.peek_u64(40, true);
    u.rsrc_fork_length = k.peek_u64(48, true);
    u.segment_number = k.peek_u32(56, true);
    u.segment_count = k.peek_u32(60, true);
    u.data_checksum_type = k.peek_u32(80, true);
    u.xml_offset = k.peek_u64(216, true);
    u.xml_length = k.peek_u64(224, true);
    u.master_checksum_type = k.peek_u32(352, true);
    u.image_variant = k.peek_u32(488, true);
    u.sector_count = k.peek_u64(492, true);
    return u;
}

}  // namespace

const char* to_string(Kind k) noexcept {
    switch (k) {
        case Kind::Unknown: return "unknown";
        case Kind::Udif: return "UDIF";
        case Kind::Encrypted: return "encrypted";
        case Kind::Sparse: return "sparse image";
        case Kind::RawImage: return "raw disk image";
    }
    return "?";
}

const char* to_string(Compression c) noexcept {
    switch (c) {
        case Compression::Zero: return "zero-fill";
        case Compression::Raw: return "raw (uncompressed)";
        case Compression::Ignore: return "ignore/free";
        case Compression::Adc: return "ADC";
        case Compression::Zlib: return "zlib";
        case Compression::Bzip2: return "bzip2";
        case Compression::Lzfse: return "LZFSE";
        case Compression::Lzma: return "LZMA";
        case Compression::Unknown: return "unknown";
    }
    return "?";
}

std::string detect_filesystem(const uint8_t* d, size_t n) {
    if (has(d, n, 1024, "H+")) return "HFS+";
    if (has(d, n, 1024, "HX")) return "HFSX";
    if (has(d, n, 32, "NXSB")) return "APFS";
    if (has(d, n, 3, "NTFS    ")) return "NTFS";
    if (has(d, n, 3, "EXFAT   ")) return "exFAT";
    if (has(d, n, 82, "FAT32") ) return "FAT32";
    if (has(d, n, 54, "FAT12") || has(d, n, 54, "FAT16")) return "FAT";
    if (has(d, n, 16 * 2048 + 1, "CD001")) {
        for (size_t s = 17; s < 20; ++s)
            if (has(d, n, s * 2048 + 1, "BEA01")) return "UDF/ISO 9660";
        for (size_t s = 17; s < 20; ++s)
            if (has(d, n, s * 2048 + 1, "CD001") && n > s * 2048 + 88 + 3 && d[s * 2048] == 2 && has(d, n, s * 2048 + 88, "%/")) return "ISO 9660 + Joliet";
        return "ISO 9660";
    }
    return {};
}

Result<Info> inspect_file(const fs::path& image, const InspectLimits& lim) {
    FileSource src(image);
    if (!src.ok()) return Error{ErrorCode::NotFound, "cannot open " + path_to_utf8(image)};
    Info info;
    info.file_size = src.size();
    log::debug("Inspecting disk image candidate ({} bytes)", info.file_size);
    if (info.file_size == 0) return Error{ErrorCode::Truncated, "empty file"};

    auto head = src.read(0, std::min<uint64_t>(info.file_size, 64 * 1024));
    if (!head.ok()) return head.error();
    const auto& h = head.value();

    if (has(h.data(), h.size(), 0, "encrcdsa") || has(h.data(), h.size(), 0, "cdsaencr")) {
        info.kind = Kind::Encrypted;
        return info;
    }
    if (has(h.data(), h.size(), 0, "sprs")) {
        info.kind = Kind::Sparse;
        return info;
    }

    if (info.file_size >= kKolySize) {
        auto tail = src.read(info.file_size - kKolySize, kKolySize);
        if (!tail.ok()) return tail.error();
        ByteView k(tail.value());
        if (k.peek_u32(0, true) == kKolyMagic) {
            info.kind = Kind::Udif;
            auto u = parse_koly(k).value();
            info.udif = u;
            const uint64_t body = info.file_size - kKolySize;  // data + xml must live before the trailer
            if (u.version != 4) info.warnings.push_back(std::format("unexpected UDIF version {} (expected 4)", u.version));
            if (u.header_size != kKolySize) return malformed(std::format("koly header size {} (expected 512)", u.header_size));
            if (u.segment_count > 1) return Error{ErrorCode::Unsupported, "multi-segment disk images are not supported"};
            if (!checked_add(u.data_fork_offset, u.data_fork_length) || u.data_fork_offset + u.data_fork_length > body)
                return Error{ErrorCode::OutOfBounds, "koly: data fork extends past the trailer"};
            if (u.xml_length == 0) {
                info.warnings.emplace_back(u.rsrc_fork_length ? "legacy resource-fork partition map (no XML plist): partitions not parsed"
                                                              : "image has no partition map");
                return info;
            }
            if (u.xml_length > lim.max_xml_bytes)
                return Error{ErrorCode::LimitExceeded, std::format("XML plist of {} bytes exceeds limit", u.xml_length)};
            if (!checked_add(u.xml_offset, u.xml_length) || u.xml_offset + u.xml_length > body)
                return Error{ErrorCode::OutOfBounds, "koly: XML plist lies outside the file"};
            auto xml = src.read(u.xml_offset, u.xml_length);
            if (!xml.ok()) return xml.error();
            auto pl = plist::parse_xml(std::string_view(reinterpret_cast<const char*>(xml.value().data()), xml.value().size()));
            if (!pl.ok()) return malformed("UDIF XML plist: " + pl.error().message);
            const plist::Value* rf = pl.value().get("resource-fork");
            const plist::Value* blkx = rf ? rf->get("blkx") : nullptr;
            if (!blkx || blkx->type != plist::Value::Type::Array) return malformed("UDIF plist has no resource-fork/blkx array");
            if (blkx->items.size() > lim.max_partitions)
                return Error{ErrorCode::LimitExceeded, std::format("{} partitions exceed limit", blkx->items.size())};
            for (const auto& e : blkx->items) {
                if (!e.is_dict()) return malformed("blkx entry is not a dictionary");
                auto p = parse_blkx(e, u, info.file_size, src, lim, info.warnings);
                if (!p.ok()) return p.error();
                for (auto& [c, st] : p.value().compression) {
                    (void)st;
                    if (std::find(info.compressions.begin(), info.compressions.end(), c) == info.compressions.end())
                        info.compressions.push_back(c);
                }
                if (!p.value().filesystem.empty())
                    add_unique(info.filesystems, p.value().filesystem + (p.value().filesystem_verified ? " (verified from data)" : " (inferred from partition name)"));
                info.partitions.push_back(std::move(p.value()));
            }
            // Zero-fill/free chunks carry no data: don't list them as a compression format of interest
            std::erase_if(info.compressions, [](Compression c) { return c == Compression::Zero || c == Compression::Ignore; });
            return info;
        }
    }

    // No UDIF trailer: maybe a raw image.
    std::string fsname = detect_filesystem(h.data(), h.size());
    if (!fsname.empty()) {
        info.kind = Kind::RawImage;
        info.raw_description = fsname;
        info.filesystems.push_back(fsname + " (verified from data)");
    } else if (has(h.data(), h.size(), 512, "EFI PART")) {
        info.kind = Kind::RawImage;
        info.raw_description = "GPT partition table";
    } else if (has(h.data(), h.size(), 0, "ER") && h.size() > 512 && has(h.data(), h.size(), 512, "PM")) {
        info.kind = Kind::RawImage;
        info.raw_description = "Apple partition map";
    }
    return info;
}

}  // namespace cider::dmg
