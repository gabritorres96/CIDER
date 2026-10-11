// Test helper: rewrites the Objective-C method lists of a linked x86_64 Mach-O into the "small" (relative) format that ld64 emits for
// macOS 11+ deployment targets. clang and ld64.lld never produce that format, so the fixtures are built normally and converted here:
//   absolute entry: { char* name; char* types; IMP imp; }                              (24 bytes, three rebased pointers)
//   small entry:    { int32 name; int32 types; int32 imp; }                            (12 bytes, each relative to its own field)
// Indirect selectors: `name` points to the __objc_selrefs slot that holds the selector string pointer; with direct selectors (flag
// 0x40000000) it points to the string itself. The rebase opcodes of the old pointer slots are removed (otherwise the loader would slide
// the new 32-bit data). Only classic dyld_info rebase streams are handled (the fixtures are linked for macOS 10.15).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace orvyn::testing::relmeth {

enum class Mode { Direct, Indirect };

namespace detail {

struct Seg { std::string name; uint64_t vmaddr = 0, vmsize = 0, fileoff = 0, filesize = 0; };
struct Sect { std::string seg, name; uint64_t addr = 0, size = 0; };

struct File {
    std::vector<uint8_t>& b;
    std::vector<Seg> segs;
    std::vector<Sect> sects;
    uint32_t rebase_off = 0, rebase_size = 0;
    mutable std::string why;

    template <class T> T get(uint64_t off) const { T v{}; std::memcpy(&v, b.data() + off, sizeof(T)); return v; }
    template <class T> void put(uint64_t off, T v) { std::memcpy(b.data() + off, &v, sizeof(T)); }

    bool parse() {
        if (b.size() < 32 || get<uint32_t>(0) != 0xFEEDFACF) { why = "not a thin 64-bit Mach-O"; return false; }
        const uint32_t ncmds = get<uint32_t>(16);
        uint64_t off = 32;
        for (uint32_t i = 0; i < ncmds; ++i) {
            const uint32_t cmd = get<uint32_t>(off), size = get<uint32_t>(off + 4);
            if (cmd == 0x19) {
                Seg s;
                s.name = std::string(reinterpret_cast<const char*>(b.data() + off + 8), strnlen(reinterpret_cast<const char*>(b.data() + off + 8), 16));
                s.vmaddr = get<uint64_t>(off + 24); s.vmsize = get<uint64_t>(off + 32);
                s.fileoff = get<uint64_t>(off + 40); s.filesize = get<uint64_t>(off + 48);
                const uint32_t nsects = get<uint32_t>(off + 64);
                for (uint32_t k = 0; k < nsects; ++k) {
                    const uint64_t so = off + 72 + 80ull * k;
                    Sect t;
                    t.name = std::string(reinterpret_cast<const char*>(b.data() + so), strnlen(reinterpret_cast<const char*>(b.data() + so), 16));
                    t.seg = std::string(reinterpret_cast<const char*>(b.data() + so + 16), strnlen(reinterpret_cast<const char*>(b.data() + so + 16), 16));
                    t.addr = get<uint64_t>(so + 32); t.size = get<uint64_t>(so + 40);
                    sects.push_back(t);
                }
                segs.push_back(std::move(s));
            } else if (cmd == 0x22 || cmd == 0x80000022) {
                rebase_off = get<uint32_t>(off + 8); rebase_size = get<uint32_t>(off + 12);
            } else if (cmd == 0x80000034 || cmd == 0x80000033) {
                why = "chained fixups are not handled by this helper"; return false;
            }
            off += size;
        }
        return rebase_size != 0;
    }
    std::optional<uint64_t> file_offset(uint64_t vm, uint64_t len = 1) const {
        for (const auto& s : segs)
            if (vm >= s.vmaddr && vm + len <= s.vmaddr + s.filesize) return s.fileoff + (vm - s.vmaddr);
        return std::nullopt;
    }
    template <class T> std::optional<T> read(uint64_t vm) const {
        auto o = file_offset(vm, sizeof(T));
        if (!o) return std::nullopt;
        return get<T>(*o);
    }
    const Sect* section(const char* name) const {
        for (const auto& s : sects) if (s.name == name) return &s;
        return nullptr;
    }

    static uint64_t uleb(const uint8_t*& p, const uint8_t* end) {
        uint64_t r = 0; int sh = 0;
        while (p < end) { const uint8_t c = *p++; r |= static_cast<uint64_t>(c & 0x7F) << sh; sh += 7; if (!(c & 0x80)) break; }
        return r;
    }
    static void put_uleb(std::vector<uint8_t>& o, uint64_t v) {
        do { uint8_t c = v & 0x7F; v >>= 7; if (v) c |= 0x80; o.push_back(c); } while (v);
    }
    std::vector<uint64_t> decode_rebases() const {
        std::vector<uint64_t> out;
        const uint8_t* p = b.data() + rebase_off;
        const uint8_t* end = p + rebase_size;
        uint64_t addr = 0;
        while (p < end) {
            const uint8_t op = *p & 0xF0, imm = *p & 0x0F;
            ++p;
            if (op == 0x00) break;
            else if (op == 0x10) {}
            else if (op == 0x20) { addr = segs.at(imm).vmaddr + uleb(p, end); }
            else if (op == 0x30) addr += uleb(p, end);
            else if (op == 0x40) addr += imm * 8ull;
            else if (op == 0x50) { for (int i = 0; i < imm; ++i) { out.push_back(addr); addr += 8; } }
            else if (op == 0x60) { const uint64_t n = uleb(p, end); for (uint64_t i = 0; i < n; ++i) { out.push_back(addr); addr += 8; } }
            else if (op == 0x70) { out.push_back(addr); addr += uleb(p, end) + 8; }
            else if (op == 0x80) { const uint64_t n = uleb(p, end), skip = uleb(p, end); for (uint64_t i = 0; i < n; ++i) { out.push_back(addr); addr += 8 + skip; } }
            else { why = "unknown rebase opcode"; return {}; }
        }
        return out;
    }
    bool encode_rebases(std::vector<uint64_t> addrs) {
        std::sort(addrs.begin(), addrs.end());
        std::vector<uint8_t> o;
        o.push_back(0x11);                                        // REBASE_TYPE_POINTER
        int cur_seg = -1;
        uint64_t cursor = 0;                                      // address dyld's state machine is at
        for (size_t i = 0; i < addrs.size();) {
            int seg = -1;
            for (size_t s = 0; s < segs.size(); ++s)
                if (addrs[i] >= segs[s].vmaddr && addrs[i] < segs[s].vmaddr + segs[s].vmsize) seg = static_cast<int>(s);
            if (seg < 0) { why = "rebase address outside every segment"; return false; }
            size_t n = 1;
            while (i + n < addrs.size() && addrs[i + n] == addrs[i] + 8 * n) ++n;
            if (seg != cur_seg) {
                o.push_back(static_cast<uint8_t>(0x20 | seg));
                put_uleb(o, addrs[i] - segs[seg].vmaddr);
                cur_seg = seg;
            } else if (addrs[i] != cursor) {
                const uint64_t gap = addrs[i] - cursor;           // addresses are sorted: never negative
                if (gap % 8 == 0 && gap / 8 < 16) o.push_back(static_cast<uint8_t>(0x40 | (gap / 8)));     // ADD_ADDR_IMM_SCALED
                else { o.push_back(0x30); put_uleb(o, gap); }                                              // ADD_ADDR_ULEB
            }
            if (n < 16) o.push_back(static_cast<uint8_t>(0x50 | n));
            else { o.push_back(0x60); put_uleb(o, n); }
            cursor = addrs[i] + 8 * n;
            i += n;
        }
        o.push_back(0x00);
        if (o.size() > rebase_size) { why = "re-encoded rebase stream does not fit: " + std::to_string(o.size()) + " > " + std::to_string(rebase_size) + " bytes for " + std::to_string(addrs.size()) + " pointers"; return false; }
        std::memset(b.data() + rebase_off, 0, rebase_size);
        std::memcpy(b.data() + rebase_off, o.data(), o.size());
        return true;
    }
};

}  // namespace detail

// Returns the converted image, or std::nullopt (and why) when the binary has a shape this helper does not handle.
inline std::optional<std::vector<uint8_t>> to_relative(std::vector<uint8_t> bytes, Mode mode, std::string* why = nullptr) {
    detail::File f{bytes, {}, {}, 0, 0, {}};
    auto fail = [&](std::string m) -> std::optional<std::vector<uint8_t>> { if (why) *why = std::move(m); return std::nullopt; };
    if (!f.parse()) return fail(f.why.empty() ? "no rebase info" : f.why);

    std::set<uint64_t> lists;
    auto ptr_list = [&](const char* name, auto&& fn) -> bool {
        const auto* s = f.section(name);
        if (!s) return true;
        for (uint64_t a = s->addr; a < s->addr + s->size; a += 8) {
            auto v = f.read<uint64_t>(a);
            if (!v || !fn(*v)) return false;
        }
        return true;
    };
    auto add_ro = [&](uint64_t cls) -> bool {
        auto data = f.read<uint64_t>(cls + 32);
        if (!data) return false;
        auto ml = f.read<uint64_t>((*data & ~7ull) + 32);
        if (!ml) return false;
        if (*ml) lists.insert(*ml);
        return true;
    };
    if (!ptr_list("__objc_classlist", [&](uint64_t c) {
            auto isa = f.read<uint64_t>(c);
            return isa && add_ro(c) && add_ro(*isa);
        }))
        return fail("class list is not readable");
    if (!ptr_list("__objc_catlist", [&](uint64_t cat) {
            for (uint64_t k : {16ull, 24ull}) {
                auto ml = f.read<uint64_t>(cat + k);
                if (!ml) return false;
                if (*ml) lists.insert(*ml);
            }
            return true;
        }))
        return fail("category list is not readable");

    std::map<uint64_t, uint64_t> selref_of;                        // selector string address -> selref slot address
    if (mode == Mode::Indirect) {
        const auto* sr = f.section("__objc_selrefs");
        if (!sr) return fail("no __objc_selrefs");
        for (uint64_t a = sr->addr; a < sr->addr + sr->size; a += 8)
            if (auto v = f.read<uint64_t>(a)) selref_of.emplace(*v, a);
    }

    std::set<uint64_t> drop;
    for (uint64_t list : lists) {
        auto ef = f.read<uint32_t>(list), count = f.read<uint32_t>(list + 4);
        if (!ef || !count) return fail("method list is not readable");
        if (*ef & 0x80000000) continue;                            // already relative
        if ((*ef & 0xFFFC) != 24) return fail("unexpected absolute entry size");
        struct E { uint64_t name, types, imp; };
        std::vector<E> es;
        for (uint32_t i = 0; i < *count; ++i) {
            auto n = f.read<uint64_t>(list + 8 + 24ull * i), t = f.read<uint64_t>(list + 16 + 24ull * i), m = f.read<uint64_t>(list + 24 + 24ull * i);
            if (!n || !t || !m) return fail("method entry is not readable");
            es.push_back({*n, *t, *m});
            for (int k = 0; k < 3; ++k) drop.insert(list + 8 + 24ull * i + 8 * k);
        }
        const uint64_t base = *f.file_offset(list, 8 + 24ull * *count);
        std::memset(bytes.data() + base + 8, 0, 24ull * *count);
        f.put<uint32_t>(base, 0x80000000u | (mode == Mode::Direct ? 0x40000000u : 0u) | (*ef & 3u) | 12u);
        for (uint32_t i = 0; i < *count; ++i) {
            uint64_t name_target = es[i].name;
            if (mode == Mode::Indirect) {
                auto it = selref_of.find(es[i].name);
                if (it == selref_of.end()) return fail("method name has no selector reference");
                name_target = it->second;
            }
            const uint64_t field = list + 8 + 12ull * i;
            const uint64_t targets[3] = {name_target, es[i].types, es[i].imp};
            for (int k = 0; k < 3; ++k) {
                const int64_t d = static_cast<int64_t>(targets[k]) - static_cast<int64_t>(field + 4 * k);
                f.put<int32_t>(base + 8 + 12ull * i + 4 * k, static_cast<int32_t>(d));
            }
        }
    }
    if (drop.empty()) return fail("no absolute method list found");
    auto rebases = f.decode_rebases();
    if (rebases.empty()) return fail(f.why.empty() ? "empty rebase stream" : f.why);
    std::vector<uint64_t> keep;
    for (uint64_t a : rebases) if (!drop.count(a)) keep.push_back(a);
    if (keep.size() + drop.size() != rebases.size()) return fail("a method pointer slot had no rebase entry");
    if (!f.encode_rebases(keep)) return fail(f.why);
    return bytes;
}

// Address (in the unslid image) of the first method list of `class_name`'s class_ro_t, or 0 - for tests that corrupt a list.
inline uint64_t first_method_list(std::vector<uint8_t> bytes) {
    detail::File f{bytes, {}, {}, 0, 0, {}};
    if (!f.parse()) return 0;
    const auto* s = f.section("__objc_classlist");
    if (!s) return 0;
    auto cls = f.read<uint64_t>(s->addr);
    if (!cls) return 0;
    auto data = f.read<uint64_t>(*cls + 32);
    if (!data) return 0;
    auto ml = f.read<uint64_t>((*data & ~7ull) + 32);
    return ml ? *ml : 0;
}

// File offset of an unslid virtual address inside a (possibly converted) image, or 0.
inline uint64_t file_offset_of(std::vector<uint8_t> bytes, uint64_t vm) {
    detail::File f{bytes, {}, {}, 0, 0, {}};
    if (!f.parse()) return 0;
    auto o = f.file_offset(vm, 1);
    return o ? *o : 0;
}

}  // namespace orvyn::testing::relmeth
