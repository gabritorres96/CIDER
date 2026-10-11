#include <algorithm>
#include <cstdio>
#include <cstring>

#include "internal.hpp"
#include "orvyn/frameworks.hpp"
#include "orvyn/libsystem.hpp"
#include "orvyn/log.hpp"

namespace orvyn::libsystem {
namespace {
RuntimeContext* g_ctx = nullptr;

constexpr uint64_t kFileMagic = 0x43494445522d4649ull;   // "ORVYN-FI"
}  // namespace

void set_context(RuntimeContext* ctx) {
    g_ctx = ctx;
    abi::set_callback_validator(ctx ? &is_valid_callback : nullptr);
}
RuntimeContext* context() { return g_ctx; }

Result<std::unique_ptr<LibSystemProvider>> LibSystemProvider::create() {
    auto p = std::unique_ptr<LibSystemProvider>(new LibSystemProvider());
    std::vector<abi::ThunkSpec> specs;
    for (const auto& s : implemented_symbols()) {
        if (p->index_.count(s.name) || p->direct_index_.count(s.name)) return Error{ErrorCode::Malformed, std::string("duplicate libSystem symbol ") + s.name};
        p->lib_of_[s.name] = s.lib;
        if (s.direct) { p->direct_index_[s.name] = reinterpret_cast<uint64_t>(s.fn); continue; }
        p->index_[s.name] = specs.size();
        specs.push_back({s.fn, s.raw, s.preserve_all});
    }
    for (const auto& s : detail::stub_host_symbols()) {
        if (p->index_.count(s.name) || p->direct_index_.count(s.name)) return Error{ErrorCode::Malformed, std::string("stub collides with an implemented symbol: ") + s.name};
        p->lib_of_[s.name] = s.lib;
        p->stubs_.insert(s.name);
        p->index_[s.name] = specs.size();
        specs.push_back({s.fn, s.raw, s.preserve_all});
    }
    auto t = abi::ThunkTable::build_specs(specs);
    if (!t.ok()) return t.error();
    p->table_ = std::move(t.value());

    // Data page: [3 stream variables (8 bytes each)] [3 FILE objects]. Written once, then read-only.
    const size_t bytes = 64 + kMaxStreams * kFileObjectSize;
    auto region = memory::Region::reserve(0, static_cast<size_t>(*memory::align_up(bytes, memory::page_size())), false);
    if (!region.ok()) return region.error();
    p->data_ = std::move(region.value());
    if (auto c = p->data_.commit(0, p->data_.size()); !c.ok()) return c.error();
    uint8_t* base = p->data_.base();
    std::memset(base, 0, bytes);
    for (size_t i = 0; i < kMaxStreams; ++i) {
        uint8_t* file = base + 64 + i * kFileObjectSize;
        std::memcpy(file, &kFileMagic, 8);
        const uint64_t slot = i;
        std::memcpy(file + 8, &slot, 8);
        if (i < 3) {
            p->files_[i] = reinterpret_cast<uint64_t>(file);
            const uint64_t ptr = p->files_[i];
            std::memcpy(base + i * 8, &ptr, 8);
        }
    }
    p->data_index_["___stdinp"] = reinterpret_cast<uint64_t>(base);
    p->data_index_["___stdoutp"] = reinterpret_cast<uint64_t>(base + 8);
    p->data_index_["___stderrp"] = reinterpret_cast<uint64_t>(base + 16);
    if (auto pr = p->data_.protect(0, p->data_.size(), memory::Prot::Read); !pr.ok()) return pr.error();
    return p;
}

namespace {
// Group a library install name belongs to; nullopt when ORVYN does not provide it.
std::optional<Lib> group_of(std::string_view n) {
    if (const FrameworkInfo* f = find_framework(n)) return f->lib;
    return std::nullopt;
}
}  // namespace

std::optional<dyld::ResolvedSymbol> LibSystemProvider::lookup(std::string_view library, std::string_view symbol) {
    std::optional<Lib> want;
    if (!library.empty()) {
        want = group_of(library);
        if (!want) return std::nullopt;
    }
    auto owns = [&](std::string_view name) {
        if (!want) return true;
        auto it = lib_of_.find(name);
        const Lib have = it == lib_of_.end() ? Lib::System : it->second;    // data symbols and dynamic ones: resolved below by group
        // the frameworks re-export each other (AppKit re-exports Foundation, Cocoa re-exports AppKit...): see lib_visible
        return lib_visible(*want, have);
    };
    if (auto it = index_.find(symbol); it != index_.end()) {
        if (!owns(symbol)) return std::nullopt;
        dyld::ResolvedSymbol r{table_.address(it->second), true};
        r.stub = stubs_.count(std::string(symbol)) != 0;
        return r;
    }
    if (auto it = direct_index_.find(symbol); it != direct_index_.end()) return owns(symbol) ? std::optional<dyld::ResolvedSymbol>(dyld::ResolvedSymbol{it->second, true}) : std::nullopt;
    if (auto it = data_index_.find(symbol); it != data_index_.end()) return dyld::ResolvedSymbol{it->second, false};
    if (dynamic_) {
        // Objective-C classes and Foundation data are only visible through libobjc / Foundation, never through libSystem
        // (ApplicationServices and Carbon re-export CoreGraphics, whose data constants are dynamic; the other recognised-only libraries export none)
        const bool hosts_dynamic = !want || *want == Lib::Objc || *want == Lib::Foundation || *want == Lib::CoreFoundation || *want == Lib::CoreGraphics || *want == Lib::AppKit ||
                                   *want == Lib::Cocoa || *want == Lib::ApplicationServices || *want == Lib::Carbon;
        if (hosts_dynamic || symbol.rfind("__NSConcrete", 0) == 0 || symbol.rfind("__dispatch_", 0) == 0)       // block isa and dispatch objects come from libSystem
            if (auto a = dynamic_(symbol)) return dyld::ResolvedSymbol{*a, false};
    }
    return std::nullopt;
}

bool LibSystemProvider::provides_library(std::string_view install_name) const { return group_of(install_name).has_value(); }

void LibSystemProvider::install_stdio(RuntimeContext& ctx) const {
    ctx.file_stdin = files_[0];
    ctx.file_stdout = files_[1];
    ctx.file_stderr = files_[2];
    ctx.stream_base = files_[0];
    ctx.streams.assign(kMaxStreams, Stream{});
    for (int i = 0; i < 3; ++i) {
        Stream& s = ctx.streams[static_cast<size_t>(i)];
        s.in_use = true;
        s.fd = i;
        s.readable = i == 0;
        s.writable = i != 0;
    }
}

bool is_implemented_data(std::string_view s) {
    for (const auto& d : implemented_data_symbols())
        if (s == d.name) return true;
    return false;
}

}  // namespace orvyn::libsystem
