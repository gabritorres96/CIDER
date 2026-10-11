#pragma once
// Mach-O x86_64 image loader (ORVYN 0.2): pre-execution validation, layout planning, segment mapping,
// fixups and final W^X protections. Loading never runs guest code.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "orvyn/bytes.hpp"
#include "orvyn/fixups.hpp"
#include "orvyn/macho.hpp"
#include "orvyn/memory.hpp"
#include "orvyn/result.hpp"

namespace orvyn::loader {

// ---- pre-execution validation (security/execution_validator.cpp) ---------------------------------
struct Issue {
    enum class Kind { Incompatible, Unsupported, MissingDependency } kind = Kind::Incompatible;
    std::string message;
};

struct ValidationReport {
    std::vector<Issue> issues;                 // empty => acceptable for execution
    std::vector<std::string> required_libs;    // LC_LOAD_* install names
    bool ok() const noexcept { return issues.empty(); }
};

struct ValidatorOptions {
    // Dylibs the runtime provides. Anything else is a missing dependency.
    std::vector<std::string> provided_libraries = {"/usr/lib/libSystem.B.dylib"};
    uint64_t max_image_span = 1ull << 30;     // 1 GiB of address space for the image
    uint64_t max_stack_size = 64ull << 20;
    // Dynamic-linking mode (ORVYN 0.3). In the single-image mode of 0.2 (all false) any dependency other than
    // `provided_libraries` is a missing dependency and Mach-O initializers are rejected.
    bool linker_resolves_dependencies = false;  // dependencies are searched, loaded and checked by the dynamic linker
    bool allow_initializers = false;            // __mod_init_func / __init_offsets are run by the dynamic linker
    bool allow_tls = false;                     // S_THREAD_LOCAL_{REGULAR,ZEROFILL,VARIABLES,VARIABLE_POINTERS}: served by ORVYN's tlv_get_addr
    bool as_dependency = false;                 // the image is a dylib loaded for another image (MH_DYLIB, no LC_MAIN)
};

// Checks everything that can be decided from the parsed image alone. Never touches memory.
ValidationReport validate_for_execution(const macho::Image& img, const ValidatorOptions& opts = {});

// ---- layout (loader/image_layout.cpp) -------------------------------------------------------------
struct SegmentPlan {
    uint32_t index = 0;               // index in macho::Image::segments
    std::string name;
    uint64_t vmaddr = 0, vmsize = 0;  // vmsize page aligned
    uint64_t fileoff = 0, filesize = 0;
    memory::Prot final_prot = memory::Prot::None;
    bool read_only_after_fixups = false;  // __DATA_CONST: writable while fixups are applied
};

struct ImageLayout {
    uint64_t preferred_base = 0;   // lowest mapped vmaddr
    uint64_t span = 0;             // bytes from preferred_base to the end of the highest mapped segment
    std::vector<SegmentPlan> segments;  // mapped segments only (no __PAGEZERO, no __LINKEDIT)
    uint64_t entry_vmaddr = 0;     // LC_MAIN target (virtual address at the preferred base)
    uint64_t stack_size = 0;       // LC_MAIN stacksize or the default
};

constexpr uint64_t kDefaultStackSize = 8ull << 20;

// Computes and validates the layout (overflow, alignment, overlap, permissions, entry inside an
// executable segment). Pure computation: no memory is reserved.
Result<ImageLayout> plan_layout(const macho::Image& img, const ValidatorOptions& opts = {});

// ---- mapping (loader/segment_mapper.cpp, macho_loader.cpp) -----------------------------------------
struct LoadedImage {
    memory::Region region;
    ImageLayout layout;
    int64_t slide = 0;                       // actual_base - preferred_base
    std::vector<uint32_t> mapped_indices;    // segment indices that are mapped

    uint8_t* base() const noexcept { return region.base(); }
    uint64_t actual_base() const noexcept { return reinterpret_cast<uint64_t>(region.base()); }
    // Host address of a virtual address given at the preferred base (nullptr if outside a mapped segment).
    uint8_t* host_address(uint64_t vmaddr) const noexcept;
    uint64_t entry_address() const noexcept { return static_cast<uint64_t>(static_cast<int64_t>(layout.entry_vmaddr) + slide); }
    uint64_t header_address() const noexcept { return actual_base(); }
};

// Reserves the image span (preferred base first, then relocated), commits every mapped segment as
// ReadWrite, copies the file contents and zero-fills the rest. Nothing is executable yet.
Result<LoadedImage> map_segments(const ImageLayout& layout, ByteView slice);

// Same, for images whose binds are resolved per fixup (several libraries may export the same name):
// bind_values[i] is the value for fixups[i] when it is a Bind (entries of Rebase fixups are ignored).
Status apply_fixups_resolved(LoadedImage& image, const macho::Image& img, const std::vector<dyld::Fixup>& fixups,
                             const std::vector<uint64_t>& bind_values);

// Applies rebases (adding the slide) and binds (writing resolved addresses). `symbols` maps symbol
// name -> final host address.
Status apply_fixups(LoadedImage& image, const macho::Image& img, const std::vector<dyld::Fixup>& fixups,
                    const std::map<std::string, uint64_t>& symbols);

// Final permissions: __TEXT r-x, data rw-, __DATA_CONST r--; flushes the instruction cache.
Status finalize_protections(LoadedImage& image);

struct LoadReport {
    std::vector<std::string> required_libs;
    std::vector<dyld::Unresolved> unresolved;
    size_t rebases = 0, binds = 0;
};

enum class LoadFailure { None, Incompatible, Unsupported, MissingDependency, UnresolvedSymbol, Internal };

struct LoadError {
    LoadFailure kind = LoadFailure::Internal;
    std::string message;
    LoadReport report;
};

struct LoadResult {
    std::optional<LoadedImage> image;
    std::optional<LoadError> error;
    LoadReport report;
    bool ok() const noexcept { return image.has_value(); }
};

// Whole pipeline for a file already in memory: parse (x86_64 slice), validate, plan, map, decode fixups,
// resolve symbols with `provider`, apply fixups, finalize protections. On any failure everything
// reserved so far is released and the error says which stage failed.
LoadResult load_macho(ByteView file, dyld::SymbolProvider& provider, const ValidatorOptions& opts = {});

}  // namespace orvyn::loader
