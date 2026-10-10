#include <format>
#include <set>

#include "cider/exports.hpp"

namespace cider::dyld {
namespace {

Error bad(std::string m) { return Error{ErrorCode::Malformed, "export trie: " + std::move(m)}; }

class Walker {
public:
    Walker(ByteView t, const ExportLimits& l) : t_(t), lim_(l) {}

    // Iterative (explicit stack): the depth of a hostile trie must never become native recursion.
    Result<std::vector<ExportedSymbol>> run() {
        if (t_.empty()) return std::vector<ExportedSymbol>{};
        std::string prefix;
        CIDER_CHECK(enter(0, prefix));
        while (!stack_.empty()) {
            Frame& top = stack_.back();
            if (top.remaining == 0) { stack_.pop_back(); continue; }
            --top.remaining;
            uint64_t pos = top.pos;
            const size_t prefix_len = top.prefix_len;
            CIDER_TRY(label, cstr(pos));
            CIDER_TRY(child, uleb(pos));
            top.pos = pos;                                   // `top` is still valid: nothing was pushed yet
            prefix.resize(prefix_len);
            if (prefix.size() + label.size() > lim_.max_name_bytes) return Error{ErrorCode::LimitExceeded, "export trie: symbol name too long"};
            prefix += label;
            if (stack_.size() >= lim_.max_depth) return Error{ErrorCode::LimitExceeded, "export trie: nesting too deep"};
            CIDER_CHECK(enter(child, prefix));
        }
        return std::move(out_);
    }

private:
    struct Frame { uint64_t pos; unsigned remaining; size_t prefix_len; };
    ByteView t_;
    const ExportLimits& lim_;
    std::vector<ExportedSymbol> out_;
    std::set<uint64_t> visited_;
    std::vector<Frame> stack_;

    Result<uint64_t> uleb(uint64_t& pos) {
        uint64_t value = 0;
        unsigned shift = 0;
        for (;;) {
            auto b = t_.u8(pos);
            if (!b.ok()) return bad("ULEB128 runs past the end of the trie");
            ++pos;
            if (shift >= 64 || (shift == 63 && (b.value() & 0x7e))) return bad("ULEB128 value does not fit in 64 bits");
            value |= static_cast<uint64_t>(b.value() & 0x7f) << shift;
            if (!(b.value() & 0x80)) return value;
            shift += 7;
        }
    }

    Result<std::string> cstr(uint64_t& pos) {
        auto s = t_.cstring(pos, t_.size());
        if (!s.ok()) return bad("unterminated string");
        if (s.value().size() > lim_.max_name_bytes) return Error{ErrorCode::LimitExceeded, "export trie: edge label too long"};
        pos += s.value().size() + 1;
        return std::move(s.value());
    }

    // Parses the node at `off` (terminal info -> an export, then the child count) and pushes its frame.
    Status enter(uint64_t off, const std::string& prefix) {
        if (off >= t_.size()) return bad(std::format("node offset {:#x} is outside the trie", off));
        if (!visited_.insert(off).second) return bad(std::format("node {:#x} is reachable twice (cycle or shared node)", off));
        uint64_t pos = off;
        CIDER_TRY(tsize, uleb(pos));
        if (tsize > t_.size() - pos) return bad("terminal information extends past the trie");
        const uint64_t children_at = pos + tsize;
        if (tsize) {
            if (out_.size() >= lim_.max_symbols) return Error{ErrorCode::LimitExceeded, "export trie: too many symbols"};
            ExportedSymbol e;
            e.name = prefix;
            CIDER_TRY(flags, uleb(pos));
            e.flags = flags;
            if (flags & kExportReexport) {
                CIDER_TRY(ord, uleb(pos));
                CIDER_TRY(imp, cstr(pos));
                e.reexport_ordinal = ord;
                e.reexport_name = imp;
            } else if (flags & kExportStubAndResolver) {
                CIDER_TRY(stub, uleb(pos));
                CIDER_TRY(resolver, uleb(pos));
                (void)resolver;
                e.address = stub;
            } else {
                CIDER_TRY(addr, uleb(pos));
                e.address = addr;
            }
            if (pos > children_at) return bad("terminal information is larger than its declared size");
            out_.push_back(std::move(e));
        }
        pos = children_at;
        auto count = t_.u8(pos);
        if (!count.ok()) return bad("missing child count");
        stack_.push_back({pos + 1, count.value(), prefix.size()});
        return {};
    }
};

}  // namespace

Result<std::vector<ExportedSymbol>> parse_export_trie(ByteView trie, const ExportLimits& limits) {
    return Walker(trie, limits).run();
}

Result<std::vector<ExportedSymbol>> decode_exports(const macho::Image& img, ByteView slice, const ExportLimits& limits) {
    macho::LinkeditRange r{};
    if (img.exports_trie) r = *img.exports_trie;
    else if (img.dyld_info) r = img.dyld_info->exports;
    if (r.size == 0) return std::vector<ExportedSymbol>{};
    auto bytes = slice.slice(r.offset, r.size);
    if (!bytes.ok()) return Error{ErrorCode::OutOfBounds, "export trie lies outside the image"};
    return parse_export_trie(bytes.value(), limits);
}

}  // namespace cider::dyld
