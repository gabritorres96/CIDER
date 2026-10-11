// LC_DYLD_CHAINED_FIXUPS decoder (ORVYN 0.3). Supported, exactly:
//   * header version 0 (the only one that exists), imports formats DYLD_CHAINED_IMPORT (1), IMPORT_ADDEND (2) and
//     IMPORT_ADDEND64 (3), uncompressed symbol pool;
//   * chain pointer formats DYLD_CHAINED_PTR_64 (2) and DYLD_CHAINED_PTR_64_OFFSET (6): 64-bit pointers, 4-byte stride,
//     page size 4 KiB or 16 KiB, one chain start per page.
// Everything else is refused with a message that names the format: arm64e pointer authentication (1, 7, 9, 10, 12),
// 32-bit and kernel-cache formats (3, 4, 5, 8, 11), "multiple starts in one page" overflow lists, later header versions.
// The decoder never reads outside the load command's data, never follows a chain out of its page or out of the
// file-backed part of the segment, and bounds the number of fixups: a hostile chain cannot loop or write elsewhere.
#include <format>

#include "orvyn/fixups.hpp"

namespace orvyn::dyld {
namespace {

constexpr uint16_t kPtr64 = 2, kPtr64Offset = 6;
constexpr uint16_t kStartNone = 0xFFFF, kStartMulti = 0x8000;

Error bad(std::string m) { return Error{ErrorCode::Malformed, "chained fixups: " + std::move(m)}; }
Error unsupported(std::string m) { return Error{ErrorCode::Unsupported, "chained fixups: " + std::move(m)}; }

const char* format_name(uint16_t f) {
    switch (f) {
        case 1: return "DYLD_CHAINED_PTR_ARM64E";
        case 2: return "DYLD_CHAINED_PTR_64";
        case 3: return "DYLD_CHAINED_PTR_32";
        case 4: return "DYLD_CHAINED_PTR_32_CACHE";
        case 5: return "DYLD_CHAINED_PTR_32_FIRMWARE";
        case 6: return "DYLD_CHAINED_PTR_64_OFFSET";
        case 7: return "DYLD_CHAINED_PTR_ARM64E_KERNEL";
        case 8: return "DYLD_CHAINED_PTR_64_KERNEL_CACHE";
        case 9: return "DYLD_CHAINED_PTR_ARM64E_USERLAND";
        case 10: return "DYLD_CHAINED_PTR_ARM64E_FIRMWARE";
        case 11: return "DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE";
        case 12: return "DYLD_CHAINED_PTR_ARM64E_USERLAND24";
        default: return "unknown";
    }
}

struct Import {
    std::string name;
    int64_t ordinal = 0;
    bool weak = false;
    int64_t addend = 0;
};

}  // namespace

Result<std::vector<Fixup>> decode_chained_fixups(const macho::Image& img, ByteView slice, const FixupLimits& lim) {
    if (!img.chained_fixups) return std::vector<Fixup>{};
    auto blob_r = slice.slice(img.chained_fixups->offset, img.chained_fixups->size);
    if (!blob_r.ok()) return Error{ErrorCode::OutOfBounds, "chained fixups: data lies outside the image"};
    const ByteView blob = blob_r.value();

    // ---- header ---------------------------------------------------------------------------------------------
    if (blob.size() < 28) return bad("header is truncated");
    const uint32_t version = blob.peek_u32(0), starts_off = blob.peek_u32(4), imports_off = blob.peek_u32(8), symbols_off = blob.peek_u32(12),
                   imports_count = blob.peek_u32(16), imports_format = blob.peek_u32(20), symbols_format = blob.peek_u32(24);
    if (version != 0) return unsupported(std::format("header version {} is not supported (only 0)", version));
    if (symbols_format != 0) return unsupported(std::format("symbol pool format {} is not supported (only 0, uncompressed)", symbols_format));
    uint64_t import_size;
    switch (imports_format) {
        case 1: import_size = 4; break;
        case 2: import_size = 8; break;
        case 3: import_size = 16; break;
        default: return unsupported(std::format("imports format {} is not supported (supported: 1, 2, 3)", imports_format));
    }
    if (imports_count > lim.max_fixups) return Error{ErrorCode::LimitExceeded, "chained fixups: too many imports"};
    if (symbols_off > blob.size()) return bad("symbol pool offset is outside the data");
    {
        auto total = checked_mul(imports_count, import_size);
        auto end = total ? checked_add(imports_off, *total) : std::nullopt;
        if (!end || *end > blob.size()) return bad("imports table extends past the data");
    }

    // ---- imports ----------------------------------------------------------------------------------------------
    std::vector<Import> imports(imports_count);
    for (uint32_t i = 0; i < imports_count; ++i) {
        const uint64_t at = imports_off + static_cast<uint64_t>(i) * import_size;
        Import& im = imports[i];
        uint64_t name_off;
        if (imports_format == 3) {
            const uint64_t v = blob.peek_u64(at);
            int64_t ord = static_cast<int64_t>(v & 0xFFFF);
            if (ord > 0xFFF0) ord -= 0x10000;           // BIND_SPECIAL_DYLIB_* are stored sign-extended in 16 bits
            im.ordinal = ord;
            im.weak = ((v >> 16) & 1) != 0;
            name_off = v >> 32;
            im.addend = static_cast<int64_t>(blob.peek_u64(at + 8));
        } else {
            const uint32_t v = blob.peek_u32(at);
            int64_t ord = static_cast<int64_t>(v & 0xFF);
            if (ord > 0xF0) ord -= 0x100;
            im.ordinal = ord;
            im.weak = ((v >> 8) & 1) != 0;
            name_off = v >> 9;
            if (imports_format == 2) im.addend = static_cast<int32_t>(blob.peek_u32(at + 4));
        }
        auto name = blob.cstring(symbols_off + name_off, blob.size());
        if (!name.ok()) return bad(std::format("import {} has an unterminated or out-of-range name", i));
        if (name.value().empty()) return bad(std::format("import {} has an empty name", i));
        im.name = std::move(name.value());
    }

    // ---- segments and chains -----------------------------------------------------------------------------------------
    if (starts_off > blob.size() || blob.size() - starts_off < 4) return bad("starts_in_image is outside the data");
    const uint32_t seg_count = blob.peek_u32(starts_off);
    if (seg_count > img.segments.size()) return bad(std::format("starts_in_image lists {} segments but the image has {}", seg_count, img.segments.size()));
    if (static_cast<uint64_t>(seg_count) * 4 > blob.size() - starts_off - 4) return bad("segment offset table extends past the data");

    uint64_t header_vmaddr = 0;
    bool have_header = false;
    for (const auto& s : img.segments)
        if (s.fileoff == 0 && s.filesize > 0 && s.name != "__PAGEZERO") { header_vmaddr = s.vmaddr; have_header = true; break; }
    if (!have_header) return bad("no segment contains the Mach-O header");

    std::vector<Fixup> out;
    for (uint32_t si = 0; si < seg_count; ++si) {
        const uint32_t seg_info = blob.peek_u32(starts_off + 4 + 4ull * si);
        if (seg_info == 0) continue;                                      // no fixups in this segment
        const uint64_t at = static_cast<uint64_t>(starts_off) + seg_info;
        if (at > blob.size() || blob.size() - at < 22) return bad(std::format("segment {} starts record is outside the data", si));
        const uint32_t size = blob.peek_u32(at);
        const uint16_t page_size = blob.u16(at + 4).value();           // the 22-byte record header is inside the data (checked above)
        const uint16_t format = blob.u16(at + 6).value();
        const uint64_t segment_offset = blob.peek_u64(at + 8);
        const uint16_t page_count = blob.u16(at + 20).value();
        if (format != kPtr64 && format != kPtr64Offset)
            return unsupported(std::format("pointer format {} ({}) is not supported (supported: 2 DYLD_CHAINED_PTR_64, 6 DYLD_CHAINED_PTR_64_OFFSET)", format, format_name(format)));
        if (page_size != 0x1000 && page_size != 0x4000) return unsupported(std::format("page size {:#x} is not supported (4 KiB or 16 KiB)", page_size));
        if (size < 22 + 2ull * page_count || size > blob.size() - at) return bad(std::format("segment {} starts record has an inconsistent size", si));
        const macho::Segment& seg = img.segments[si];
        if (segment_offset != seg.vmaddr - header_vmaddr) return bad(std::format("segment {} offset {:#x} does not match its address", si, segment_offset));
        if (page_count && static_cast<uint64_t>(page_count - 1) * page_size >= seg.vmsize) return bad(std::format("segment {} has more pages than its size allows", si));

        for (uint32_t p = 0; p < page_count; ++p) {
            const uint16_t start = blob.u16(at + 22 + 2ull * p).value();     // inside the record: size >= 22 + 2 * page_count
            if (start == kStartNone) continue;
            if (start & kStartMulti) return unsupported("pages with several chain starts (DYLD_CHAINED_PTR_START_MULTI) are not supported");
            if (start >= page_size) return bad(std::format("chain start {:#x} is outside its page", start));
            uint64_t off = static_cast<uint64_t>(p) * page_size + start;
            const uint64_t page_end = static_cast<uint64_t>(p + 1) * page_size;
            for (uint64_t guard = 0;; ++guard) {
                if (guard > page_size / 4) return bad("chain is longer than its page");
                if (off > seg.filesize || seg.filesize - off < 8) return bad(std::format("fixup at segment offset {:#x} is outside the file-backed part of the segment", off));
                auto raw = slice.u64(seg.fileoff + off);
                if (!raw.ok()) return Error{ErrorCode::OutOfBounds, "chained fixups: fixup location lies outside the image"};
                const uint64_t v = raw.value();
                if (out.size() >= lim.max_fixups) return Error{ErrorCode::LimitExceeded, "chained fixups: too many fixups"};
                Fixup f;
                f.segment = si;
                f.offset = off;
                if (v >> 63) {                                                  // bind
                    const uint64_t ordinal = v & 0xFFFFFF;
                    if (ordinal >= imports.size()) return bad(std::format("bind refers to import {} but there are {}", ordinal, imports.size()));
                    const Import& im = imports[ordinal];
                    f.kind = Fixup::Kind::Bind;
                    f.symbol = im.name;
                    f.library_ordinal = im.ordinal;
                    f.weak_import = im.weak;
                    f.addend = im.addend + static_cast<int64_t>((v >> 24) & 0xFF);
                } else {                                                        // rebase
                    const uint64_t target = v & 0xFFFFFFFFFull;
                    const uint64_t high8 = (v >> 36) & 0xFF;
                    f.kind = Fixup::Kind::ChainedRebase;
                    f.target = format == kPtr64Offset ? header_vmaddr + target : target;   // always an unslid virtual address
                    f.addend = static_cast<int64_t>(high8);
                }
                out.push_back(std::move(f));
                const uint64_t next = (v >> 51) & 0xFFF;
                if (next == 0) break;
                off += next * 4;
                if (off >= page_end) return bad("chain leaves its page");
            }
        }
    }
    return out;
}

}  // namespace orvyn::dyld
