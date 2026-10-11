#pragma once
// Mach-O export trie (LC_DYLD_INFO(_ONLY) export range or LC_DYLD_EXPORTS_TRIE): the list of symbols an image
// defines, with their address offsets, and re-exports. Pure parsing: nothing is mapped or executed.
//
// Every offset, label and ULEB128 is bounds checked; a node may be reached only once (so a hostile trie cannot
// loop or blow up exponentially) and the number of symbols and the depth are limited.
#include <cstdint>
#include <string>
#include <vector>

#include "orvyn/bytes.hpp"
#include "orvyn/macho.hpp"
#include "orvyn/result.hpp"

namespace orvyn::dyld {

inline constexpr uint64_t kExportKindMask = 0x03, kExportKindRegular = 0, kExportKindThreadLocal = 1, kExportKindAbsolute = 2,
                          kExportWeakDefinition = 0x04, kExportReexport = 0x08, kExportStubAndResolver = 0x10;

struct ExportedSymbol {
    std::string name;                 // as stored (leading underscore included)
    uint64_t flags = 0;
    uint64_t address = 0;             // offset from the Mach-O header (absolute value for kExportKindAbsolute)
    uint64_t reexport_ordinal = 0;    // kExportReexport: 1-based index of the dylib that really defines it
    std::string reexport_name;        // kExportReexport: name in that dylib ("" = same name)
    bool is_reexport() const noexcept { return (flags & kExportReexport) != 0; }
    bool is_absolute() const noexcept { return (flags & kExportKindMask) == kExportKindAbsolute && !is_reexport(); }
};

struct ExportLimits {
    size_t max_symbols = 1u << 20;
    size_t max_depth = 512;           // longest chain of edges (symbol name length is bounded by it times the label size)
    size_t max_name_bytes = 4096;
};

Result<std::vector<ExportedSymbol>> parse_export_trie(ByteView trie, const ExportLimits& limits = {});

// Locates the trie of `img` inside the slice and parses it. An image without exports yields an empty list.
Result<std::vector<ExportedSymbol>> decode_exports(const macho::Image& img, ByteView slice, const ExportLimits& limits = {});

}  // namespace orvyn::dyld
