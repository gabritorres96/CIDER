#include "cider/macho.hpp"

#include <algorithm>
#include <format>

#include "cider/log.hpp"
#include "cider/util.hpp"
#include "codesign.hpp"

namespace cider::macho {
namespace {

constexpr uint32_t MH_MAGIC_64 = 0xfeedfacf, MH_CIGAM_64 = 0xcffaedfe, MH_MAGIC = 0xfeedface, MH_CIGAM = 0xcefaedfe;
constexpr uint32_t FAT_MAGIC = 0xcafebabe, FAT_MAGIC_64 = 0xcafebabf;

constexpr uint32_t LC_SYMTAB = 0x2, LC_UNIXTHREAD = 0x5, LC_DYSYMTAB = 0xb, LC_LOAD_DYLIB = 0xc, LC_ID_DYLIB = 0xd,
                   LC_LOAD_DYLINKER = 0xe, LC_LAZY_LOAD_DYLIB = 0x20, LC_SEGMENT_64 = 0x19, LC_UUID = 0x1b,
                   LC_CODE_SIGNATURE = 0x1d, LC_DYLD_INFO = 0x22, LC_VERSION_MIN_MACOSX = 0x24,
                   LC_VERSION_MIN_IPHONEOS = 0x25, LC_FUNCTION_STARTS = 0x26, LC_ENCRYPTION_INFO_64 = 0x2c,
                   LC_VERSION_MIN_TVOS = 0x2f, LC_VERSION_MIN_WATCHOS = 0x30, LC_BUILD_VERSION = 0x32,
                   LC_LOAD_WEAK_DYLIB = 0x80000018, LC_RPATH = 0x8000001c, LC_REEXPORT_DYLIB = 0x8000001f,
                   LC_DYLD_INFO_ONLY = 0x80000022, LC_LOAD_UPWARD_DYLIB = 0x80000023, LC_MAIN = 0x80000028,
                   LC_DYLD_EXPORTS_TRIE = 0x80000033, LC_DYLD_CHAINED_FIXUPS = 0x80000034;

Error malformed(std::string msg) { return Error{ErrorCode::Malformed, std::move(msg)}; }
Error oob(std::string msg) { return Error{ErrorCode::OutOfBounds, std::move(msg)}; }

// A (offset,size) pair inside the slice. size == 0 means "absent" and the offset is ignored.
Status check_range(ByteView slice, const char* what, uint64_t off, uint64_t size) {
    if (size == 0) return {};
    if (!slice.contains(off, size))
        return oob(std::format("{}: range [{:#x}, +{:#x}) exceeds slice size {:#x}", what, off, size, slice.size()));
    return {};
}

Status check_table(ByteView slice, const char* what, uint64_t off, uint64_t count, uint64_t entry) {
    auto bytes = checked_mul(count, entry);
    if (!bytes) return Error{ErrorCode::Overflow, std::format("{}: entry count overflows", what)};
    return check_range(slice, what, off, *bytes);
}

bool is_zerofill(uint32_t flags) {
    uint32_t t = flags & 0xff;
    return t == 0x1 || t == 0xc || t == 0x12;
}

Status parse_segment(ByteView cmd, ByteView slice, Image& img) {
    if (cmd.size() < 72) return malformed(std::format("LC_SEGMENT_64: cmdsize {} < 72", cmd.size()));
    Segment s;
    s.name = cmd.fixed_string(8, 16);
    s.vmaddr = cmd.peek_u64(24);
    s.vmsize = cmd.peek_u64(32);
    s.fileoff = cmd.peek_u64(40);
    s.filesize = cmd.peek_u64(48);
    s.maxprot = static_cast<int32_t>(cmd.peek_u32(56));
    s.initprot = static_cast<int32_t>(cmd.peek_u32(60));
    uint32_t nsects = cmd.peek_u32(64);
    s.flags = cmd.peek_u32(68);

    auto sect_bytes = checked_mul(nsects, 80);
    if (!sect_bytes || *sect_bytes > cmd.size() - 72)
        return malformed(std::format("segment {}: {} sections do not fit in cmdsize {}", s.name, nsects, cmd.size()));
    if (!checked_add(s.vmaddr, s.vmsize))
        return Error{ErrorCode::Overflow, std::format("segment {}: vmaddr+vmsize overflows", s.name)};
    if (!checked_add(s.fileoff, s.filesize))
        return Error{ErrorCode::Overflow, std::format("segment {}: fileoff+filesize overflows", s.name)};
    if (!slice.contains(s.fileoff, s.filesize))
        return oob(std::format("segment {}: file range [{:#x}, +{:#x}) exceeds slice size {:#x}", s.name, s.fileoff,
                               s.filesize, slice.size()));
    if (s.filesize > s.vmsize)
        return malformed(std::format("segment {}: filesize {:#x} > vmsize {:#x}", s.name, s.filesize, s.vmsize));

    for (uint32_t i = 0; i < nsects; ++i) {
        uint64_t o = 72 + 80ull * i;
        Section sec;
        sec.name = cmd.fixed_string(o, 16);
        sec.segment = cmd.fixed_string(o + 16, 16);
        sec.addr = cmd.peek_u64(o + 32);
        sec.size = cmd.peek_u64(o + 40);
        sec.offset = cmd.peek_u32(o + 48);
        sec.align = cmd.peek_u32(o + 52);
        sec.reloff = cmd.peek_u32(o + 56);
        sec.nreloc = cmd.peek_u32(o + 60);
        sec.flags = cmd.peek_u32(o + 64);
        if (sec.size) {
            auto end = checked_add(sec.addr, sec.size);
            if (!end) return Error{ErrorCode::Overflow, std::format("section {},{}: addr+size overflows", s.name, sec.name)};
            if (sec.addr < s.vmaddr || *end > s.vmaddr + s.vmsize)
                return oob(std::format("section {},{}: address range outside its segment", s.name, sec.name));
            if (!is_zerofill(sec.flags)) {
                auto fend = checked_add(sec.offset, sec.size);
                if (!fend || *fend > slice.size())
                    return oob(std::format("section {},{}: file range outside the slice", s.name, sec.name));
                if (sec.offset < s.fileoff || *fend > s.fileoff + s.filesize)
                    return oob(std::format("section {},{}: file range outside its segment", s.name, sec.name));
            }
        }
        CIDER_CHECK(check_table(slice, "section relocations", sec.reloff, sec.nreloc, 8));
        s.sections.push_back(std::move(sec));
    }
    img.segments.push_back(std::move(s));
    return {};
}

Status parse_dylib(ByteView cmd, uint32_t id, Image& img) {
    if (cmd.size() < 24) return malformed(std::format("dylib command {:#x}: cmdsize {} < 24", id, cmd.size()));
    uint32_t name_off = cmd.peek_u32(8);
    if (name_off < 24 || name_off >= cmd.size())
        return malformed(std::format("dylib command {:#x}: name offset {} invalid", id, name_off));
    auto name = cmd.cstring(name_off, cmd.size());
    if (!name.ok()) return malformed("dylib command: unterminated install name");
    Dylib d;
    d.path = name.value();
    d.timestamp = cmd.peek_u32(12);
    d.current_version = cmd.peek_u32(16);
    d.compat_version = cmd.peek_u32(20);
    switch (id) {
        case LC_ID_DYLIB: img.id_dylib = d; return {};
        case LC_LOAD_WEAK_DYLIB: d.kind = DylibKind::Weak; break;
        case LC_REEXPORT_DYLIB: d.kind = DylibKind::Reexport; break;
        case LC_LAZY_LOAD_DYLIB: d.kind = DylibKind::Lazy; break;
        case LC_LOAD_UPWARD_DYLIB: d.kind = DylibKind::Upward; break;
        default: d.kind = DylibKind::Load;
    }
    d.ordinal = static_cast<uint32_t>(img.dylibs.size()) + 1;
    img.dylibs.push_back(std::move(d));
    return {};
}

Status parse_symtab(ByteView cmd, ByteView slice, Image& img) {
    if (cmd.size() < 24) return malformed("LC_SYMTAB: cmdsize < 24");
    SymtabInfo s{cmd.peek_u32(8), cmd.peek_u32(12), cmd.peek_u32(16), cmd.peek_u32(20)};
    CIDER_CHECK(check_table(slice, "symbol table", s.symoff, s.nsyms, 16));
    CIDER_CHECK(check_range(slice, "string table", s.stroff, s.strsize));
    img.symtab = s;
    return {};
}

Status parse_dysymtab(ByteView cmd, ByteView slice, Image& img) {
    if (cmd.size() < 80) return malformed("LC_DYSYMTAB: cmdsize < 80");
    DysymtabInfo d;
    d.ilocalsym = cmd.peek_u32(8);
    d.nlocalsym = cmd.peek_u32(12);
    d.iextdefsym = cmd.peek_u32(16);
    d.nextdefsym = cmd.peek_u32(20);
    d.iundefsym = cmd.peek_u32(24);
    d.nundefsym = cmd.peek_u32(28);
    d.nindirectsyms = cmd.peek_u32(60);
    CIDER_CHECK(check_table(slice, "table of contents", cmd.peek_u32(32), cmd.peek_u32(36), 8));
    CIDER_CHECK(check_table(slice, "module table", cmd.peek_u32(40), cmd.peek_u32(44), 56));
    CIDER_CHECK(check_table(slice, "external reference table", cmd.peek_u32(48), cmd.peek_u32(52), 4));
    CIDER_CHECK(check_table(slice, "indirect symbol table", cmd.peek_u32(56), d.nindirectsyms, 4));
    CIDER_CHECK(check_table(slice, "external relocations", cmd.peek_u32(64), cmd.peek_u32(68), 8));
    CIDER_CHECK(check_table(slice, "local relocations", cmd.peek_u32(72), cmd.peek_u32(76), 8));
    img.dysymtab = d;
    return {};
}

Status parse_dyld_info(ByteView cmd, ByteView slice, uint32_t id, Image& img) {
    if (cmd.size() < 48) return malformed("LC_DYLD_INFO: cmdsize < 48");
    DyldInfo d;
    d.only = id == LC_DYLD_INFO_ONLY;
    LinkeditRange* r[5] = {&d.rebase, &d.bind, &d.weak_bind, &d.lazy_bind, &d.exports};
    static constexpr const char* names[5] = {"rebase info", "bind info", "weak bind info", "lazy bind info", "export info"};
    for (int i = 0; i < 5; ++i) {
        r[i]->offset = cmd.peek_u32(8 + 8 * i);
        r[i]->size = cmd.peek_u32(12 + 8 * i);
        CIDER_CHECK(check_range(slice, names[i], r[i]->offset, r[i]->size));
    }
    img.dyld_info = d;
    return {};
}

Status parse_linkedit_data(ByteView cmd, ByteView slice, const char* what, LinkeditRange& out) {
    if (cmd.size() < 16) return malformed(std::format("{}: cmdsize < 16", what));
    out.offset = cmd.peek_u32(8);
    out.size = cmd.peek_u32(12);
    return check_range(slice, what, out.offset, out.size);
}

Status parse_build_version(ByteView cmd, Image& img) {
    if (cmd.size() < 24) return malformed("LC_BUILD_VERSION: cmdsize < 24");
    BuildVersion b;
    b.platform = cmd.peek_u32(8);
    b.minos = cmd.peek_u32(12);
    b.sdk = cmd.peek_u32(16);
    uint32_t ntools = cmd.peek_u32(20);
    auto tool_bytes = checked_mul(ntools, 8);
    if (!tool_bytes || *tool_bytes > cmd.size() - 24) return malformed("LC_BUILD_VERSION: tool list exceeds cmdsize");
    for (uint32_t i = 0; i < ntools; ++i) b.tools.emplace_back(cmd.peek_u32(24 + 8ull * i), cmd.peek_u32(28 + 8ull * i));
    img.build_versions.push_back(std::move(b));
    return {};
}

Status parse_version_min(ByteView cmd, uint32_t id, Image& img) {
    if (cmd.size() < 16) return malformed("LC_VERSION_MIN_*: cmdsize < 16");
    BuildVersion b;
    b.legacy = true;
    b.platform = id == LC_VERSION_MIN_MACOSX ? 1 : id == LC_VERSION_MIN_IPHONEOS ? 2 : id == LC_VERSION_MIN_TVOS ? 3 : 4;
    b.minos = cmd.peek_u32(8);
    b.sdk = cmd.peek_u32(12);
    img.build_versions.push_back(std::move(b));
    return {};
}

std::string format_uuid(ByteView cmd) {
    std::string s;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
        s += std::format("{:02X}", cmd.data()[8 + i]);
    }
    return s;
}

Status parse_symbols(ByteView slice, const ParseOptions& opts, Image& img) {
    if (!img.symtab) return {};
    const SymtabInfo& st = *img.symtab;
    if (st.nsyms > opts.limits.max_symbols)
        return Error{ErrorCode::LimitExceeded, std::format("{} symbols exceed limit {}", st.nsyms, opts.limits.max_symbols)};
    // strsize == 0 means "no string table": its offset is then not validated (see check_range), so
    // never slice with it. (Found by libFuzzer: the unchecked slice() threw bad_variant_access.)
    ByteView strtab;
    if (st.strsize) strtab = slice.slice(st.stroff, st.strsize).value();
    img.symbols.reserve(st.nsyms);
    for (uint32_t i = 0; i < st.nsyms; ++i) {
        uint64_t o = st.symoff + 16ull * i;
        Symbol s;
        uint32_t strx = slice.peek_u32(o);
        s.type = static_cast<uint8_t>(slice.u8(o + 4).value());
        s.section = static_cast<uint8_t>(slice.u8(o + 5).value());
        s.desc = slice.u16(o + 6).value();
        s.value = slice.peek_u64(o + 8);
        if (strx != 0) {
            auto n = strtab.cstring(strx, strtab.size());
            if (!n.ok()) return malformed(std::format("symbol #{}: invalid string table index {:#x}", i, strx));
            s.name = std::move(n.value());
        }
        s.debug = (s.type & 0xe0) != 0;
        s.external = (s.type & 0x01) != 0;
        // N_TYPE == N_UNDF is "undefined" whether or not N_EXT is set (matches llvm-objdump/nm); stab
        // (debugging) entries are never undefined, whatever their n_sect.
        s.undefined = !s.debug && (s.type & 0x0e) == 0;
        if (s.undefined) s.library_ordinal = static_cast<uint8_t>(s.desc >> 8);
        img.symbols.push_back(std::move(s));
    }
    return {};
}

template <class T>
Status ensure_unique(const std::optional<T>& o, const char* what) {
    if (o) return malformed(std::format("duplicate {} load command", what));
    return {};
}

}  // namespace

Format identify(ByteView d) noexcept {
    if (d.size() < 4) return Format::Unknown;
    uint32_t be = d.peek_u32(0, true), le = d.peek_u32(0, false);
    if (be == FAT_MAGIC) return Format::Fat;
    if (be == FAT_MAGIC_64) return Format::Fat64;
    switch (le) {
        case MH_MAGIC_64: return Format::MachO64;
        case MH_MAGIC: return Format::MachO32;
        case MH_CIGAM_64: return Format::MachO64Swapped;
        case MH_CIGAM: return Format::MachO32Swapped;
        default: return Format::Unknown;
    }
}

Result<Image> parse_image(ByteView data, const ParseOptions& opts) {
    log::debug("Parsing Mach-O slice ({} bytes)", data.size());
    switch (identify(data)) {
        case Format::MachO64: break;
        case Format::MachO32: return Error{ErrorCode::Unsupported, "32-bit Mach-O is not supported"};
        case Format::MachO64Swapped:
        case Format::MachO32Swapped: return Error{ErrorCode::Unsupported, "big-endian Mach-O is not supported"};
        case Format::Fat:
        case Format::Fat64: return Error{ErrorCode::BadMagic, "nested FAT container inside a slice"};
        default: return Error{ErrorCode::BadMagic, "not a Mach-O file (bad magic)"};
    }
    if (data.size() < 32) return Error{ErrorCode::Truncated, "file smaller than mach_header_64"};

    Image img;
    img.slice_size = data.size();
    img.cputype = data.peek_u32(4);
    img.cpusubtype = data.peek_u32(8);
    img.filetype = data.peek_u32(12);
    img.ncmds = data.peek_u32(16);
    img.sizeofcmds = data.peek_u32(20);
    img.flags = data.peek_u32(24);

    if (img.cputype != kCpuX86_64 && img.cputype != kCpuArm64 && img.cputype != kCpuPpc64)
        return Error{ErrorCode::Unsupported, std::format("unknown or unsupported 64-bit cputype {:#x}", img.cputype)};
    if (img.filetype < kFileObject || img.filetype > kFileSet)
        return malformed(std::format("invalid filetype {}", img.filetype));
    if (img.ncmds > opts.limits.max_load_commands)
        return Error{ErrorCode::LimitExceeded, std::format("ncmds {} exceeds limit {}", img.ncmds, opts.limits.max_load_commands)};
    if (img.sizeofcmds > opts.limits.max_sizeofcmds)
        return Error{ErrorCode::LimitExceeded, std::format("sizeofcmds {} exceeds limit {}", img.sizeofcmds, opts.limits.max_sizeofcmds)};
    if (!data.contains(32, img.sizeofcmds))
        return Error{ErrorCode::Truncated, std::format("load commands ({} bytes) extend past end of file", img.sizeofcmds)};
    if (static_cast<uint64_t>(img.ncmds) * 8 > img.sizeofcmds)
        return malformed(std::format("ncmds {} cannot fit in sizeofcmds {}", img.ncmds, img.sizeofcmds));

    std::optional<uint64_t> main_entryoff, main_stack;
    std::optional<uint64_t> thread_pc;
    bool have_main = false, have_uuid = false;
    uint64_t pos = 0;

    for (uint32_t i = 0; i < img.ncmds; ++i) {
        if (img.sizeofcmds - pos < 8) return malformed(std::format("load command #{} header exceeds sizeofcmds", i));
        uint32_t id = data.peek_u32(32 + pos);
        uint32_t size = data.peek_u32(36 + pos);
        if (size < 8 || size % 8 != 0) return malformed(std::format("load command #{}: invalid cmdsize {}", i, size));
        if (size > img.sizeofcmds - pos) return oob(std::format("load command #{}: cmdsize {} exceeds sizeofcmds", i, size));
        ByteView cmd = data.slice(32 + pos, size).value();
        pos += size;
        img.load_commands.push_back(id);
        log::trace("load command #{}: {:#x} size {}", i, id, size);

        switch (id) {
            case LC_SEGMENT_64: CIDER_CHECK(parse_segment(cmd, data, img)); break;
            case LC_LOAD_DYLIB: case LC_LOAD_WEAK_DYLIB: case LC_REEXPORT_DYLIB: case LC_LAZY_LOAD_DYLIB:
            case LC_LOAD_UPWARD_DYLIB: CIDER_CHECK(parse_dylib(cmd, id, img)); break;
            case LC_ID_DYLIB:
                CIDER_CHECK(ensure_unique(img.id_dylib, "LC_ID_DYLIB"));
                CIDER_CHECK(parse_dylib(cmd, id, img));
                break;
            case LC_RPATH: {
                if (cmd.size() < 12) return malformed("LC_RPATH: cmdsize < 12");
                uint32_t off = cmd.peek_u32(8);
                if (off < 12 || off >= cmd.size()) return malformed("LC_RPATH: path offset invalid");
                auto p = cmd.cstring(off, cmd.size());
                if (!p.ok()) return malformed("LC_RPATH: unterminated path");
                img.rpaths.push_back(p.value());
                break;
            }
            case LC_LOAD_DYLINKER: {
                if (cmd.size() < 12) return malformed("LC_LOAD_DYLINKER: cmdsize < 12");
                uint32_t off = cmd.peek_u32(8);
                if (off < 12 || off >= cmd.size()) return malformed("LC_LOAD_DYLINKER: name offset invalid");
                auto p = cmd.cstring(off, cmd.size());
                if (!p.ok()) return malformed("LC_LOAD_DYLINKER: unterminated name");
                img.dylinker = p.value();
                break;
            }
            case LC_MAIN:
                if (have_main) return malformed("duplicate LC_MAIN load command");
                if (cmd.size() < 24) return malformed("LC_MAIN: cmdsize < 24");
                have_main = true;
                main_entryoff = cmd.peek_u64(8);
                main_stack = cmd.peek_u64(16);
                break;
            case LC_UNIXTHREAD:
                // x86_THREAD_STATE64: flavor 4, count 42; rip is the 17th 64-bit register.
                if (img.cputype == kCpuX86_64 && cmd.size() >= 16 + 42 * 4 && cmd.peek_u32(8) == 4 && cmd.peek_u32(12) == 42)
                    thread_pc = cmd.peek_u64(16 + 16 * 8);
                else
                    img.warnings.emplace_back("LC_UNIXTHREAD present but its thread state is not x86_THREAD_STATE64");
                break;
            case LC_SYMTAB:
                CIDER_CHECK(ensure_unique(img.symtab, "LC_SYMTAB"));
                CIDER_CHECK(parse_symtab(cmd, data, img));
                break;
            case LC_DYSYMTAB:
                CIDER_CHECK(ensure_unique(img.dysymtab, "LC_DYSYMTAB"));
                CIDER_CHECK(parse_dysymtab(cmd, data, img));
                break;
            case LC_DYLD_INFO: case LC_DYLD_INFO_ONLY:
                CIDER_CHECK(ensure_unique(img.dyld_info, "LC_DYLD_INFO"));
                CIDER_CHECK(parse_dyld_info(cmd, data, id, img));
                break;
            case LC_CODE_SIGNATURE: {
                CIDER_CHECK(ensure_unique(img.code_signature, "LC_CODE_SIGNATURE"));
                LinkeditRange r;
                CIDER_CHECK(parse_linkedit_data(cmd, data, "code signature", r));
                CodeSignature cs;
                cs.dataoff = r.offset;
                cs.datasize = r.size;
                if (r.size) parse_code_signature(data.slice(r.offset, r.size).value(), cs, img.warnings);
                img.code_signature = cs;
                break;
            }
            case LC_FUNCTION_STARTS: {
                LinkeditRange r;
                CIDER_CHECK(parse_linkedit_data(cmd, data, "function starts", r));
                img.function_starts = r;
                break;
            }
            case LC_DYLD_EXPORTS_TRIE: {
                LinkeditRange r;
                CIDER_CHECK(parse_linkedit_data(cmd, data, "exports trie", r));
                img.exports_trie = r;
                break;
            }
            case LC_DYLD_CHAINED_FIXUPS: {
                LinkeditRange r;
                CIDER_CHECK(parse_linkedit_data(cmd, data, "chained fixups", r));
                img.chained_fixups = r;
                break;
            }
            case LC_BUILD_VERSION: CIDER_CHECK(parse_build_version(cmd, img)); break;
            case LC_VERSION_MIN_MACOSX: case LC_VERSION_MIN_IPHONEOS: case LC_VERSION_MIN_TVOS:
            case LC_VERSION_MIN_WATCHOS: CIDER_CHECK(parse_version_min(cmd, id, img)); break;
            case LC_UUID:
                if (have_uuid) return malformed("duplicate LC_UUID load command");
                if (cmd.size() < 24) return malformed("LC_UUID: cmdsize < 24");
                have_uuid = true;
                img.uuid = format_uuid(cmd);
                break;
            case LC_ENCRYPTION_INFO_64:
                if (cmd.size() < 24) return malformed("LC_ENCRYPTION_INFO_64: cmdsize < 24");
                CIDER_CHECK(check_range(data, "encrypted range", cmd.peek_u32(8), cmd.peek_u32(12)));
                if (cmd.peek_u32(16) != 0) img.encrypted = true;
                break;
            default: break;  // recognised by size only; contents ignored
        }
    }

    // Segments must not overlap in virtual memory or in the file.
    std::vector<const Segment*> order;
    for (auto& s : img.segments) order.push_back(&s);
    std::sort(order.begin(), order.end(), [](auto* a, auto* b) { return a->vmaddr < b->vmaddr; });
    for (size_t i = 1; i < order.size(); ++i) {
        const Segment *a = order[i - 1], *b = order[i];
        if (a->vmsize && b->vmsize && a->vmaddr + a->vmsize > b->vmaddr)
            return Error{ErrorCode::Overlap, std::format("segments {} and {} overlap in virtual memory", a->name, b->name)};
    }
    order.erase(std::remove_if(order.begin(), order.end(), [](auto* s) { return s->filesize == 0; }), order.end());
    std::sort(order.begin(), order.end(), [](auto* a, auto* b) { return a->fileoff < b->fileoff; });
    for (size_t i = 1; i < order.size(); ++i) {
        const Segment *a = order[i - 1], *b = order[i];
        if (a->fileoff + a->filesize > b->fileoff)
            return Error{ErrorCode::Overlap, std::format("segments {} and {} overlap in the file", a->name, b->name)};
    }

    // symtab / dysymtab consistency
    if (img.dysymtab) {
        if (!img.symtab) return malformed("LC_DYSYMTAB without LC_SYMTAB");
        const auto &d = *img.dysymtab;
        uint64_t n = img.symtab->nsyms;
        if (uint64_t(d.ilocalsym) + d.nlocalsym > n || uint64_t(d.iextdefsym) + d.nextdefsym > n ||
            uint64_t(d.iundefsym) + d.nundefsym > n)
            return oob("LC_DYSYMTAB: symbol groups exceed symbol table");
    }

    // entry point
    if (have_main) {
        const Segment* seg = img.segment_for_fileoff(*main_entryoff);
        if (!seg) return malformed(std::format("LC_MAIN entryoff {:#x} is not inside any segment", *main_entryoff));
        EntryPoint e;
        e.kind = EntryPoint::Kind::Main;
        e.file_offset = *main_entryoff;
        e.stack_size = *main_stack;
        e.vmaddr = seg->vmaddr + (*main_entryoff - seg->fileoff);
        e.segment = seg->name;
        img.entry = e;
    } else if (thread_pc) {
        const Segment* seg = img.segment_for_vmaddr(*thread_pc);
        if (!seg) return malformed(std::format("LC_UNIXTHREAD pc {:#x} is not inside any segment", *thread_pc));
        EntryPoint e;
        e.kind = EntryPoint::Kind::UnixThread;
        e.vmaddr = *thread_pc;
        e.segment = seg->name;
        img.entry = e;
    } else if (img.filetype == kFileExecute) {
        img.warnings.emplace_back("executable has no LC_MAIN/LC_UNIXTHREAD entry point");
    }

    if (opts.parse_symbols) CIDER_CHECK(parse_symbols(data, opts, img));
    return img;
}

Result<File> parse_file(ByteView data, const ParseOptions& opts) {
    File f;
    f.file_size = data.size();
    Format fmt = identify(data);

    if (fmt != Format::Fat && fmt != Format::Fat64) {
        CIDER_TRY(img, parse_image(data, opts));
        Slice s;
        s.cputype = img.cputype;
        s.cpusubtype = img.cpusubtype;
        s.offset = 0;
        s.size = data.size();
        s.image = std::move(img);
        f.slices.push_back(std::move(s));
        return f;
    }

    f.fat = true;
    f.fat64 = fmt == Format::Fat64;
    if (data.size() < 8) return Error{ErrorCode::Truncated, "FAT header truncated"};
    uint32_t n = data.peek_u32(4, true);
    // 0xcafebabe is also the Java class-file magic; real FAT files have few architectures.
    if (n == 0 || n > opts.limits.max_fat_archs)
        return malformed(std::format("FAT arch count {} invalid (or file is a Java class)", n));
    const uint64_t entry = f.fat64 ? 32 : 20;
    const uint64_t table_end = 8 + entry * n;
    if (!data.contains(0, table_end)) return Error{ErrorCode::Truncated, "FAT arch table truncated"};

    for (uint32_t i = 0; i < n; ++i) {
        uint64_t o = 8 + entry * i;
        Slice s;
        s.cputype = data.peek_u32(o, true);
        s.cpusubtype = data.peek_u32(o + 4, true);
        if (f.fat64) {
            s.offset = data.peek_u64(o + 8, true);
            s.size = data.peek_u64(o + 16, true);
            s.align = data.peek_u32(o + 24, true);
        } else {
            s.offset = data.peek_u32(o + 8, true);
            s.size = data.peek_u32(o + 12, true);
            s.align = data.peek_u32(o + 16, true);
        }
        if (s.size == 0) return malformed(std::format("FAT slice #{} has zero size", i));
        if (!checked_add(s.offset, s.size) || !data.contains(s.offset, s.size))
            return oob(std::format("FAT slice #{} range [{:#x}, +{:#x}) exceeds file size {:#x}", i, s.offset, s.size, data.size()));
        if (s.offset < table_end) return Error{ErrorCode::Overlap, std::format("FAT slice #{} overlaps the arch table", i)};
        if (s.align > 30) return malformed(std::format("FAT slice #{} alignment 2^{} is absurd", i, s.align));
        if (s.align && s.offset % (1ull << s.align) != 0)
            f.warnings.push_back(std::format("FAT slice #{} is not aligned to 2^{}", i, s.align));
        for (auto& prev : f.slices) {
            if (prev.offset < s.offset + s.size && s.offset < prev.offset + prev.size)
                return Error{ErrorCode::Overlap, std::format("FAT slices {} and {} overlap", cpu_name(prev.cputype, prev.cpusubtype), cpu_name(s.cputype, s.cpusubtype))};
            if (prev.cputype == s.cputype && prev.cpusubtype == s.cpusubtype)
                f.warnings.push_back("duplicate architecture " + cpu_name(s.cputype, s.cpusubtype) + " in FAT file");
        }
        f.slices.push_back(std::move(s));
    }

    for (auto& s : f.slices) {
        auto bytes = data.slice(s.offset, s.size).value();
        auto r = parse_image(bytes, opts);
        if (r.ok()) {
            if (r.value().cputype != s.cputype)
                s.error = Error{ErrorCode::Malformed, "slice header cputype disagrees with FAT arch table"};
            else
                s.image = std::move(r.value());
        } else {
            s.error = r.error();
            log::warn("slice {}: {}", cpu_name(s.cputype, s.cpusubtype), r.error().message);
        }
    }
    return f;
}

}  // namespace cider::macho
