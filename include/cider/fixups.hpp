#pragma once
// Classic dyld fixups (LC_DYLD_INFO_ONLY): rebase, bind and lazy-bind opcode streams, and symbol
// resolution against a pluggable provider. Platform independent: nothing here touches process memory.
//
// Supported: x86_64 pointer rebases (type 1) and pointer binds (type 1), lazy binds (applied eagerly),
// special ordinals self/main-executable/flat-lookup, weak imports. NOT supported (rejected with a clear
// error): chained fixups, threaded binds, non-pointer bind types, weak-binding streams.
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cider/bytes.hpp"
#include "cider/macho.hpp"
#include "cider/result.hpp"

namespace cider::dyld {

struct Fixup {
    enum class Kind { Rebase, Bind } kind = Kind::Rebase;
    uint32_t segment = 0;        // index into Image::segments (LC_SEGMENT_64 order, __PAGEZERO is 0)
    uint64_t offset = 0;         // byte offset of the 8-byte pointer inside the segment
    std::string symbol;          // Bind only (leading underscore included, as stored)
    int64_t library_ordinal = 0; // Bind only: 1-based dylib ordinal, 0 = self, -1 = main executable, -2 = flat lookup
    int64_t addend = 0;          // Bind only
    bool weak_import = false;    // Bind only: BIND_SYMBOL_FLAGS_WEAK_IMPORT
    bool lazy = false;           // Bind only: came from the lazy-binding stream
};

struct FixupLimits {
    size_t max_fixups = 1u << 22;
};

// Decodes rebase + bind + lazy-bind streams of `img` (the slice bytes `slice` are the file image the
// offsets in the load commands refer to). Every segment index and offset is range checked against
// the segment's vmsize.
Result<std::vector<Fixup>> decode_fixups(const macho::Image& img, ByteView slice, const FixupLimits& limits = {});

// Raw stream decoders (exposed for tests).
Result<std::vector<Fixup>> decode_rebase_stream(ByteView stream, const std::vector<macho::Segment>& segs, const FixupLimits& limits = {});
Result<std::vector<Fixup>> decode_bind_stream(ByteView stream, const std::vector<macho::Segment>& segs, bool lazy, const FixupLimits& limits = {});

// ---- symbol resolution ----------------------------------------------------------------------
struct ResolvedSymbol {
    uint64_t address = 0;
    bool is_function = true;
};

class SymbolProvider {
public:
    virtual ~SymbolProvider() = default;
    // `library` is the install name of the dylib the binary asked for ("" for flat lookups). Return
    // nullopt when the symbol is not implemented.
    virtual std::optional<ResolvedSymbol> lookup(std::string_view library, std::string_view symbol) = 0;
    // True if the provider stands in for this dylib (e.g. "/usr/lib/libSystem.B.dylib").
    virtual bool provides_library(std::string_view install_name) const = 0;
};

struct Unresolved {
    std::string symbol;
    std::string library;  // install name (or a description for special ordinals)
    bool weak = false;
    std::string reason;   // "not implemented" / "library not available" / "unsupported ordinal"
};

struct ResolutionReport {
    std::map<std::string, uint64_t> addresses;  // symbol -> address for everything that resolved
    std::vector<Unresolved> unresolved;         // strong imports that could not be resolved
    std::vector<std::string> weak_missing;      // weak imports bound to 0
    std::vector<std::string> libraries;         // install names referenced by the binds (unique, in order)
};

// Resolves every Bind fixup. Never fails midway: all problems are collected so one run reports them all.
ResolutionReport resolve_symbols(const macho::Image& img, const std::vector<Fixup>& fixups, SymbolProvider& provider);

}  // namespace cider::dyld
