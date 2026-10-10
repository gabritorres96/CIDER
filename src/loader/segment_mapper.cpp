#include <cstring>
#include <format>

#include "cider/loader.hpp"
#include "cider/log.hpp"

namespace cider::loader {

uint8_t* LoadedImage::host_address(uint64_t vmaddr) const noexcept {
    for (const auto& s : layout.segments)
        if (vmaddr >= s.vmaddr && vmaddr - s.vmaddr < s.vmsize) return region.base() + (vmaddr - layout.preferred_base);
    return nullptr;
}

Result<LoadedImage> map_segments(const ImageLayout& layout, ByteView slice) {
    LoadedImage img;
    img.layout = layout;
    // Preferred base first (position-dependent code keeps working untouched); otherwise any address and
    // the fixups add the slide. Windows gives out 64 KiB aligned reservations, as required.
    auto region = memory::Region::reserve(static_cast<uintptr_t>(layout.preferred_base), static_cast<size_t>(layout.span), true);
    if (!region.ok()) return region.error();
    img.region = std::move(region.value());
    img.slide = static_cast<int64_t>(img.actual_base()) - static_cast<int64_t>(layout.preferred_base);
    log::info(log::Category::Memory, "reserved {:#x} bytes at {:#x}{}", layout.span, img.actual_base(),
              img.slide == 0 ? " (preferred base)" : " (relocated, slide applied by fixups)");

    for (const auto& s : layout.segments) {
        const size_t off = static_cast<size_t>(s.vmaddr - layout.preferred_base);
        if (auto c = img.region.commit(off, static_cast<size_t>(s.vmsize)); !c.ok()) return c.error();  // zero-filled pages
        if (s.filesize) {
            auto src = slice.slice(s.fileoff, s.filesize);
            if (!src.ok()) return Error{ErrorCode::OutOfBounds, std::format("segment {}: file contents outside the image", s.name)};
            std::memcpy(img.region.base() + off, src.value().data(), static_cast<size_t>(s.filesize));
        }
        // Zero-fill: pages are fresh (zero), the tail of the last file page and every page beyond
        // filesize explicitly stay zero.
        if (s.vmsize > s.filesize) std::memset(img.region.base() + off + s.filesize, 0, static_cast<size_t>(s.vmsize - s.filesize));
        img.mapped_indices.push_back(s.index);
        log::info(log::Category::Memory, "mapping {} at {:#x} ({:#x} bytes from file, {:#x} bytes zero-fill)", s.name,
                  static_cast<uint64_t>(static_cast<int64_t>(s.vmaddr) + img.slide), s.filesize, s.vmsize - s.filesize);
    }
    return img;
}

Status finalize_protections(LoadedImage& image) {
    for (const auto& s : image.layout.segments) {
        const size_t off = static_cast<size_t>(s.vmaddr - image.layout.preferred_base);
        if (auto p = image.region.protect(off, static_cast<size_t>(s.vmsize), s.final_prot); !p.ok()) return p.error();
        log::debug(log::Category::Memory, "{} -> {}", s.name, memory::to_string(s.final_prot));
    }
    return {};
}

namespace {
template <class BindValue>
Status apply_impl(LoadedImage& image, const macho::Image& img, const std::vector<dyld::Fixup>& fixups, BindValue&& bind_value) {
    for (size_t i = 0; i < fixups.size(); ++i) {
        const auto& f = fixups[i];
        if (f.segment >= img.segments.size()) return Error{ErrorCode::Malformed, "fixup refers to a missing segment"};
        const auto& seg = img.segments[f.segment];
        uint8_t* seg_host = image.host_address(seg.vmaddr);
        if (!seg_host) return Error{ErrorCode::Malformed, std::format("fixup targets unmapped segment {}", seg.name)};
        if (f.offset > seg.vmsize || seg.vmsize - f.offset < 8) return Error{ErrorCode::OutOfBounds, "fixup outside its segment"};
        uint8_t* slot = seg_host + f.offset;
        uint64_t value = 0;
        std::memcpy(&value, slot, 8);
        if (f.kind == dyld::Fixup::Kind::Rebase) {
            value = static_cast<uint64_t>(static_cast<int64_t>(value) + image.slide);
        } else {
            auto v = bind_value(i, f);
            if (!v.ok()) return v.error();
            value = v.value() + static_cast<uint64_t>(f.addend);
        }
        std::memcpy(slot, &value, 8);
    }
    return {};
}
}  // namespace

Status apply_fixups(LoadedImage& image, const macho::Image& img, const std::vector<dyld::Fixup>& fixups,
                    const std::map<std::string, uint64_t>& symbols) {
    return apply_impl(image, img, fixups, [&](size_t, const dyld::Fixup& f) -> Result<uint64_t> {
        auto it = symbols.find(f.symbol);
        if (it == symbols.end()) return Error{ErrorCode::NotFound, "no address for symbol " + f.symbol};
        return it->second;
    });
}

Status apply_fixups_resolved(LoadedImage& image, const macho::Image& img, const std::vector<dyld::Fixup>& fixups,
                             const std::vector<uint64_t>& bind_values) {
    if (bind_values.size() != fixups.size()) return Error{ErrorCode::InvalidArgument, "one resolved value per fixup is required"};
    return apply_impl(image, img, fixups, [&](size_t i, const dyld::Fixup&) -> Result<uint64_t> { return bind_values[i]; });
}

}  // namespace cider::loader
