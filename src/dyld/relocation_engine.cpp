#include <format>

#include "cider/fixups.hpp"
#include "cider/log.hpp"

namespace cider::dyld {
namespace {

constexpr uint8_t kOpMask = 0xF0, kImmMask = 0x0F;

// Rebase opcodes
constexpr uint8_t REBASE_DONE = 0x00, REBASE_SET_TYPE_IMM = 0x10, REBASE_SET_SEGMENT_AND_OFFSET_ULEB = 0x20,
                  REBASE_ADD_ADDR_ULEB = 0x30, REBASE_ADD_ADDR_IMM_SCALED = 0x40, REBASE_DO_REBASE_IMM_TIMES = 0x50,
                  REBASE_DO_REBASE_ULEB_TIMES = 0x60, REBASE_DO_REBASE_ADD_ADDR_ULEB = 0x70,
                  REBASE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB = 0x80;
constexpr uint8_t kRebaseTypePointer = 1;

// Bind opcodes
constexpr uint8_t BIND_DONE = 0x00, BIND_SET_DYLIB_ORDINAL_IMM = 0x10, BIND_SET_DYLIB_ORDINAL_ULEB = 0x20,
                  BIND_SET_DYLIB_SPECIAL_IMM = 0x30, BIND_SET_SYMBOL_TRAILING_FLAGS_IMM = 0x40, BIND_SET_TYPE_IMM = 0x50,
                  BIND_SET_ADDEND_SLEB = 0x60, BIND_SET_SEGMENT_AND_OFFSET_ULEB = 0x70, BIND_ADD_ADDR_ULEB = 0x80,
                  BIND_DO_BIND = 0x90, BIND_DO_BIND_ADD_ADDR_ULEB = 0xA0, BIND_DO_BIND_ADD_ADDR_IMM_SCALED = 0xB0,
                  BIND_DO_BIND_ULEB_TIMES_SKIPPING_ULEB = 0xC0, BIND_THREADED = 0xD0;
constexpr uint8_t kBindTypePointer = 1;
constexpr uint8_t kBindFlagWeakImport = 0x1;

Error bad(std::string m) { return Error{ErrorCode::Malformed, std::move(m)}; }

struct Reader {
    ByteView d;
    size_t pos = 0;
    bool done() const { return pos >= d.size(); }
    Result<uint8_t> byte() {
        if (pos >= d.size()) return bad("fixup stream: truncated");
        return d.data()[pos++];
    }
    Result<uint64_t> uleb() {
        uint64_t v = 0;
        unsigned shift = 0;
        for (;;) {
            if (pos >= d.size()) return bad("fixup stream: truncated ULEB128");
            uint8_t b = d.data()[pos++];
            if (shift >= 64 || (shift == 63 && (b & 0x7e))) return bad("fixup stream: ULEB128 overflows 64 bits");
            v |= static_cast<uint64_t>(b & 0x7f) << shift;
            shift += 7;
            if (!(b & 0x80)) return v;
        }
    }
    Result<int64_t> sleb() {
        int64_t v = 0;
        unsigned shift = 0;
        uint8_t b = 0;
        do {
            if (pos >= d.size()) return bad("fixup stream: truncated SLEB128");
            b = d.data()[pos++];
            if (shift >= 64) return bad("fixup stream: SLEB128 overflows 64 bits");
            v |= static_cast<int64_t>(b & 0x7f) << shift;
            shift += 7;
        } while (b & 0x80);
        if (shift < 64 && (b & 0x40)) v |= -(int64_t{1} << shift);
        return v;
    }
    Result<std::string> cstring() {
        size_t start = pos;
        while (pos < d.size() && d.data()[pos] != 0) ++pos;
        if (pos >= d.size()) return bad("fixup stream: unterminated symbol name");
        std::string s(reinterpret_cast<const char*>(d.data() + start), pos - start);
        ++pos;
        return s;
    }
};

// Validates "pointer at segment:offset" and returns nothing; errors name the stream kind.
Status check_slot(const std::vector<macho::Segment>& segs, uint64_t seg, uint64_t off, const char* what) {
    if (seg >= segs.size()) return bad(std::format("{}: segment index {} out of range ({} segments)", what, seg, segs.size()));
    const auto& s = segs[seg];
    if (off > s.vmsize || s.vmsize - off < 8) return bad(std::format("{}: offset {:#x} + 8 outside segment {} (vmsize {:#x})", what, off, s.name, s.vmsize));
    return {};
}

std::optional<uint64_t> add(uint64_t a, uint64_t b) { return checked_add(a, b); }

}  // namespace

Result<std::vector<Fixup>> decode_rebase_stream(ByteView stream, const std::vector<macho::Segment>& segs, const FixupLimits& lim) {
    std::vector<Fixup> out;
    Reader r{stream};
    uint8_t type = 0;
    uint64_t seg = 0, off = 0;
    bool have_seg = false;
    auto emit = [&]() -> Status {
        if (!have_seg) return bad("rebase: DO_REBASE before SET_SEGMENT_AND_OFFSET");
        if (type != kRebaseTypePointer) return Error{ErrorCode::Unsupported, std::format("rebase type {} is not supported (only pointers)", type)};
        CIDER_CHECK(check_slot(segs, seg, off, "rebase"));
        if (out.size() >= lim.max_fixups) return Error{ErrorCode::LimitExceeded, "too many rebase fixups"};
        Fixup f;
        f.kind = Fixup::Kind::Rebase;
        f.segment = static_cast<uint32_t>(seg);
        f.offset = off;
        out.push_back(std::move(f));
        return {};
    };
    auto advance = [&](uint64_t n) -> Status {
        auto v = add(off, n);
        if (!v) return Error{ErrorCode::Overflow, "rebase: offset overflows"};
        off = *v;
        return {};
    };
    while (!r.done()) {
        CIDER_TRY(b, r.byte());
        uint8_t op = b & kOpMask, imm = b & kImmMask;
        switch (op) {
            case REBASE_DONE: return out;
            case REBASE_SET_TYPE_IMM: type = imm; break;
            case REBASE_SET_SEGMENT_AND_OFFSET_ULEB: {
                CIDER_TRY(o, r.uleb());
                seg = imm; off = o; have_seg = true;
                break;
            }
            case REBASE_ADD_ADDR_ULEB: { CIDER_TRY(v, r.uleb()); CIDER_CHECK(advance(v)); break; }
            case REBASE_ADD_ADDR_IMM_SCALED: CIDER_CHECK(advance(static_cast<uint64_t>(imm) * 8)); break;
            case REBASE_DO_REBASE_IMM_TIMES:
                for (uint8_t i = 0; i < imm; ++i) { CIDER_CHECK(emit()); CIDER_CHECK(advance(8)); }
                break;
            case REBASE_DO_REBASE_ULEB_TIMES: {
                CIDER_TRY(n, r.uleb());
                if (n > lim.max_fixups) return Error{ErrorCode::LimitExceeded, "rebase: repeat count too large"};
                for (uint64_t i = 0; i < n; ++i) { CIDER_CHECK(emit()); CIDER_CHECK(advance(8)); }
                break;
            }
            case REBASE_DO_REBASE_ADD_ADDR_ULEB: {
                CIDER_TRY(v, r.uleb());
                CIDER_CHECK(emit());
                CIDER_CHECK(advance(v));
                CIDER_CHECK(advance(8));
                break;
            }
            case REBASE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB: {
                CIDER_TRY(n, r.uleb());
                CIDER_TRY(skip, r.uleb());
                if (n > lim.max_fixups) return Error{ErrorCode::LimitExceeded, "rebase: repeat count too large"};
                for (uint64_t i = 0; i < n; ++i) { CIDER_CHECK(emit()); CIDER_CHECK(advance(8)); CIDER_CHECK(advance(skip)); }
                break;
            }
            default: return Error{ErrorCode::Unsupported, std::format("unknown rebase opcode {:#x}", b)};
        }
    }
    return out;
}

Result<std::vector<Fixup>> decode_bind_stream(ByteView stream, const std::vector<macho::Segment>& segs, bool lazy, const FixupLimits& lim) {
    std::vector<Fixup> out;
    Reader r{stream};
    int64_t ordinal = 0, addend = 0;
    std::string symbol;
    bool weak = false, have_symbol = false, have_seg = false;
    uint8_t type = kBindTypePointer;
    uint64_t seg = 0, off = 0;
    const char* what = lazy ? "lazy bind" : "bind";

    auto emit = [&]() -> Status {
        if (!have_seg) return bad(std::format("{}: DO_BIND before SET_SEGMENT_AND_OFFSET", what));
        if (!have_symbol) return bad(std::format("{}: DO_BIND before a symbol name", what));
        if (type != kBindTypePointer) return Error{ErrorCode::Unsupported, std::format("{}: bind type {} is not supported (only pointers)", what, type)};
        CIDER_CHECK(check_slot(segs, seg, off, what));
        if (out.size() >= lim.max_fixups) return Error{ErrorCode::LimitExceeded, "too many bind fixups"};
        Fixup f;
        f.kind = Fixup::Kind::Bind;
        f.segment = static_cast<uint32_t>(seg);
        f.offset = off;
        f.symbol = symbol;
        f.library_ordinal = ordinal;
        f.addend = addend;
        f.weak_import = weak;
        f.lazy = lazy;
        out.push_back(std::move(f));
        return {};
    };
    auto advance = [&](uint64_t n) -> Status {
        auto v = add(off, n);
        if (!v) return Error{ErrorCode::Overflow, "bind: offset overflows"};
        off = *v;
        return {};
    };
    while (!r.done()) {
        CIDER_TRY(b, r.byte());
        uint8_t op = b & kOpMask, imm = b & kImmMask;
        switch (op) {
            case BIND_DONE:
                if (!lazy) return out;  // lazy streams separate entries with DONE and continue
                have_symbol = false; ordinal = 0; addend = 0; weak = false; have_seg = false;
                break;
            case BIND_SET_DYLIB_ORDINAL_IMM: ordinal = imm; break;
            case BIND_SET_DYLIB_ORDINAL_ULEB: { CIDER_TRY(v, r.uleb()); ordinal = static_cast<int64_t>(v); break; }
            case BIND_SET_DYLIB_SPECIAL_IMM: ordinal = imm == 0 ? 0 : static_cast<int8_t>(imm | kOpMask); break;
            case BIND_SET_SYMBOL_TRAILING_FLAGS_IMM: {
                CIDER_TRY(name, r.cstring());
                symbol = name;
                weak = (imm & kBindFlagWeakImport) != 0;
                have_symbol = true;
                break;
            }
            case BIND_SET_TYPE_IMM: type = imm; break;
            case BIND_SET_ADDEND_SLEB: { CIDER_TRY(v, r.sleb()); addend = v; break; }
            case BIND_SET_SEGMENT_AND_OFFSET_ULEB: {
                CIDER_TRY(o, r.uleb());
                seg = imm; off = o; have_seg = true;
                break;
            }
            case BIND_ADD_ADDR_ULEB: { CIDER_TRY(v, r.uleb()); CIDER_CHECK(advance(v)); break; }
            case BIND_DO_BIND: CIDER_CHECK(emit()); CIDER_CHECK(advance(8)); break;
            case BIND_DO_BIND_ADD_ADDR_ULEB: {
                CIDER_TRY(v, r.uleb());
                CIDER_CHECK(emit());
                CIDER_CHECK(advance(v));
                CIDER_CHECK(advance(8));
                break;
            }
            case BIND_DO_BIND_ADD_ADDR_IMM_SCALED:
                CIDER_CHECK(emit());
                CIDER_CHECK(advance(static_cast<uint64_t>(imm) * 8 + 8));
                break;
            case BIND_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: {
                CIDER_TRY(n, r.uleb());
                CIDER_TRY(skip, r.uleb());
                if (n > lim.max_fixups) return Error{ErrorCode::LimitExceeded, "bind: repeat count too large"};
                for (uint64_t i = 0; i < n; ++i) { CIDER_CHECK(emit()); CIDER_CHECK(advance(8)); CIDER_CHECK(advance(skip)); }
                break;
            }
            case BIND_THREADED: return Error{ErrorCode::Unsupported, std::format("{}: threaded binds are not supported", what)};
            default: return Error{ErrorCode::Unsupported, std::format("unknown {} opcode {:#x}", what, b)};
        }
    }
    return out;
}

Result<std::vector<Fixup>> decode_fixups(const macho::Image& img, ByteView slice, const FixupLimits& lim) {
    if (img.chained_fixups)
        return Error{ErrorCode::Unsupported, "LC_DYLD_CHAINED_FIXUPS is not supported yet (only classic LC_DYLD_INFO_ONLY fixups)"};
    std::vector<Fixup> all;
    if (!img.dyld_info) return all;  // nothing to fix up
    const auto& di = *img.dyld_info;
    if (di.weak_bind.size != 0) return Error{ErrorCode::Unsupported, "weak-binding information (weak_bind) is not supported yet"};

    auto stream = [&](const macho::LinkeditRange& rg, const char* what) -> Result<ByteView> {
        if (rg.size == 0) return ByteView();
        auto s = slice.slice(rg.offset, rg.size);
        if (!s.ok()) return Error{ErrorCode::OutOfBounds, std::string(what) + " stream outside the file"};
        return s.value();
    };
    CIDER_TRY(rb, stream(di.rebase, "rebase"));
    CIDER_TRY(bd, stream(di.bind, "bind"));
    CIDER_TRY(lz, stream(di.lazy_bind, "lazy bind"));
    CIDER_TRY(rebases, decode_rebase_stream(rb, img.segments, lim));
    CIDER_TRY(binds, decode_bind_stream(bd, img.segments, false, lim));
    CIDER_TRY(lazies, decode_bind_stream(lz, img.segments, true, lim));
    all.insert(all.end(), rebases.begin(), rebases.end());
    all.insert(all.end(), binds.begin(), binds.end());
    all.insert(all.end(), lazies.begin(), lazies.end());
    if (all.size() > lim.max_fixups) return Error{ErrorCode::LimitExceeded, "too many fixups"};
    log::debug(log::Category::Dyld, "decoded {} rebase and {} bind fixup(s) ({} lazy)", rebases.size(), binds.size() + lazies.size(), lazies.size());
    return all;
}

}  // namespace cider::dyld
