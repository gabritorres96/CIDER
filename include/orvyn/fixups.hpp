#pragma once
// Classic dyld fixups (LC_DYLD_INFO_ONLY): rebase, bind and lazy-bind opcode streams, and symbol
// resolution against a pluggable provider. Platform independent: nothing here touches process memory.
//
// Supported: x86_64 pointer rebases (type 1) and pointer binds (type 1), lazy binds (applied eagerly),
// special ordinals self/main-executable/flat-lookup, weak imports, weak-binding streams (decode_weak_binds; the linker coalesces them
// across the loaded images). NOT supported (rejected with a clear error): threaded binds, non-pointer bind types.
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "orvyn/bytes.hpp"
#include "orvyn/macho.hpp"
#include "orvyn/result.hpp"

namespace orvyn::dyld {

struct Fixup {
    // Rebase: classic opcode, adds the slide to the pointer already stored in the slot.
    // ChainedRebase: the slot holds a packed chain entry; the final value is (target + slide) with `addend` as the top 8 bits.
    // Bind: symbol import; the final value is the resolved address + addend.
    enum class Kind { Rebase, Bind, ChainedRebase } kind = Kind::Rebase;
    uint32_t segment = 0;        // index into Image::segments (LC_SEGMENT_64 order, __PAGEZERO is 0)
    uint64_t offset = 0;         // byte offset of the 8-byte pointer inside the segment
    std::string symbol;          // Bind only (leading underscore included, as stored)
    int64_t library_ordinal = 0; // Bind only: 1-based dylib ordinal, 0 = self, -1 = main executable, -2 = flat lookup
    uint64_t target = 0;         // ChainedRebase only: unslid virtual address
    int64_t addend = 0;          // Bind: added to the symbol address. ChainedRebase: the "high8" byte (bits 56-63 of the pointer)
    bool weak_import = false;    // Bind only: BIND_SYMBOL_FLAGS_WEAK_IMPORT
    bool lazy = false;           // Bind only: came from the lazy-binding stream
};

struct FixupLimits {
    size_t max_fixups = 1u << 22;
};

// Decodes rebase + bind + lazy-bind streams of `img` (the slice bytes `slice` are the file image the
// offsets in the load commands refer to). Every segment index and offset is range checked against
// the segment's vmsize.
// LC_DYLD_CHAINED_FIXUPS (src/dyld/chained_fixups.cpp): pointer formats DYLD_CHAINED_PTR_64 and _64_OFFSET only.
Result<std::vector<Fixup>> decode_chained_fixups(const macho::Image& img, ByteView slice, const FixupLimits& limits = {});

Result<std::vector<Fixup>> decode_fixups(const macho::Image& img, ByteView slice, const FixupLimits& limits = {});

// Raw stream decoders (exposed for tests).
Result<std::vector<Fixup>> decode_rebase_stream(ByteView stream, const std::vector<macho::Segment>& segs, const FixupLimits& limits = {});
Result<std::vector<Fixup>> decode_bind_stream(ByteView stream, const std::vector<macho::Segment>& segs, bool lazy, const FixupLimits& limits = {});

// Weak-binding stream (LC_DYLD_INFO weak_bind): `sites` are the pointers that must end up at the coalesced definition of `symbol` (+ addend);
// `definitions` are the symbols the image says it defines non-weakly. The linker picks the definition across the loaded images.
struct WeakBindInfo {
    std::vector<Fixup> sites;                 // Kind::Bind, library_ordinal unused
    std::vector<std::string> definitions;
};
Result<WeakBindInfo> decode_weak_bind_stream(ByteView stream, const std::vector<macho::Segment>& segs, const FixupLimits& limits = {});
Result<WeakBindInfo> decode_weak_binds(const macho::Image& img, ByteView slice, const FixupLimits& limits = {});

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

}  // namespace orvyn::dyld
