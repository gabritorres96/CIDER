#include <algorithm>
#include <format>

#include "orvyn/loader.hpp"
#include "orvyn/log.hpp"

namespace orvyn::loader {
namespace {
constexpr uint64_t kPage = 4096;  // Mach-O x86_64 page size (Windows commits at the same granularity)
Error bad(ErrorCode c, std::string m) { return Error{c, std::move(m)}; }
}  // namespace

Result<ImageLayout> plan_layout(const macho::Image& img, const ValidatorOptions& opts) {
    ImageLayout lay;
    const bool is_dylib = img.filetype == macho::kFileDylib;
    if (!is_dylib && (!img.entry || img.entry->kind != macho::EntryPoint::Kind::Main))
        return bad(ErrorCode::Unsupported, "image has no LC_MAIN entry point");

    for (size_t i = 0; i < img.segments.size(); ++i) {
        const auto& s = img.segments[i];
        if (s.name == "__PAGEZERO") {
            if (s.initprot != 0 || s.maxprot != 0)
                return bad(ErrorCode::Malformed, "__PAGEZERO must have no access rights");
            continue;  // never mapped: the null page region stays unmapped (Windows keeps the low 64 KiB invalid)
        }
        if (s.vmsize == 0) continue;
        if (s.name == "__LINKEDIT") continue;  // link-edit data is not needed at run time (symbols/fixups are consumed by the loader)

        if (!memory::is_aligned(s.vmaddr, kPage))
            return bad(ErrorCode::Malformed, std::format("segment {} vmaddr {:#x} is not page aligned", s.name, s.vmaddr));
        auto size = memory::align_up(s.vmsize, kPage);
        if (!size) return bad(ErrorCode::Overflow, std::format("segment {} size overflows", s.name));
        auto end = checked_add(s.vmaddr, *size);
        if (!end) return bad(ErrorCode::Overflow, std::format("segment {} extends past the address space", s.name));
        if (s.filesize > *size) return bad(ErrorCode::Malformed, std::format("segment {} filesize exceeds its memory size", s.name));

        auto prot = memory::from_mach_prot(s.initprot);
        if (!prot.ok()) return bad(prot.error().code, std::format("segment {}: {}", s.name, prot.error().message));

        SegmentPlan p;
        p.index = static_cast<uint32_t>(i);
        p.name = s.name;
        p.vmaddr = s.vmaddr;
        p.vmsize = *size;
        p.fileoff = s.fileoff;
        p.filesize = s.filesize;
        p.final_prot = prot.value();
        p.read_only_after_fixups = s.name == "__DATA_CONST" || (s.flags & 0x10) != 0;  // SG_READ_ONLY
        if (p.read_only_after_fixups && p.final_prot == memory::Prot::ReadWrite) p.final_prot = memory::Prot::Read;
        lay.segments.push_back(std::move(p));
    }
    if (lay.segments.empty()) return bad(ErrorCode::Malformed, "image has no mappable segment");

    std::sort(lay.segments.begin(), lay.segments.end(), [](auto& a, auto& b) { return a.vmaddr < b.vmaddr; });
    for (size_t i = 1; i < lay.segments.size(); ++i) {
        const auto &a = lay.segments[i - 1], &b = lay.segments[i];
        if (a.vmaddr + a.vmsize > b.vmaddr)
            return bad(ErrorCode::Overlap, std::format("segments {} and {} overlap once rounded to pages", a.name, b.name));
    }
    lay.preferred_base = lay.segments.front().vmaddr;
    const auto& last = lay.segments.back();
    lay.span = last.vmaddr + last.vmsize - lay.preferred_base;
    if (lay.span > opts.max_image_span)
        return bad(ErrorCode::LimitExceeded, std::format("image span {:#x} exceeds the limit {:#x}", lay.span, opts.max_image_span));
    if (!memory::is_aligned(lay.preferred_base, 65536))
        return bad(ErrorCode::Unsupported, std::format("lowest segment address {:#x} is not 64 KiB aligned (Windows reservation granularity)", lay.preferred_base));

    // The entry point must be inside an executable mapped segment (dylibs have none: their code is reached through exports).
    if (!is_dylib) {
        lay.entry_vmaddr = img.entry->vmaddr;
        const SegmentPlan* holder = nullptr;
        for (auto& s : lay.segments)
            if (lay.entry_vmaddr >= s.vmaddr && lay.entry_vmaddr - s.vmaddr < s.vmsize) holder = &s;
        if (!holder) return bad(ErrorCode::Malformed, std::format("entry point {:#x} is outside every mapped segment", lay.entry_vmaddr));
        if (holder->final_prot != memory::Prot::ReadExecute)
            return bad(ErrorCode::Malformed, std::format("entry point {:#x} lies in non-executable segment {}", lay.entry_vmaddr, holder->name));
    }

    lay.stack_size = (!is_dylib && img.entry->stack_size) ? img.entry->stack_size : kDefaultStackSize;
    if (lay.stack_size > opts.max_stack_size)
        return bad(ErrorCode::LimitExceeded, std::format("requested stack size {:#x} exceeds the limit {:#x}", lay.stack_size, opts.max_stack_size));
    lay.stack_size = std::max<uint64_t>(lay.stack_size, 64 * 1024);
    return lay;
}

}  // namespace orvyn::loader
