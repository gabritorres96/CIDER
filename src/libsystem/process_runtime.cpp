#include <algorithm>
#include <cstdio>
#include <cstring>

#include "cider/libsystem.hpp"
#include "cider/log.hpp"

namespace cider::libsystem {
namespace {
RuntimeContext* g_ctx = nullptr;

constexpr size_t kFileObjectSize = 64;       // opaque FILE objects: [0] magic, [8] descriptor number
constexpr uint64_t kFileMagic = 0x43494445522d4649ull;   // "CIDER-FI"
}  // namespace

void set_context(RuntimeContext* ctx) { g_ctx = ctx; }
RuntimeContext* context() { return g_ctx; }

Result<std::unique_ptr<LibSystemProvider>> LibSystemProvider::create() {
    auto p = std::unique_ptr<LibSystemProvider>(new LibSystemProvider());
    std::vector<abi::ThunkSpec> specs;
    for (const auto& s : implemented_symbols()) {
        if (p->index_.count(s.name)) return Error{ErrorCode::Malformed, std::string("duplicate libSystem symbol ") + s.name};
        p->index_[s.name] = specs.size();
        specs.push_back({s.fn, s.raw, s.preserve_all});
    }
    auto t = abi::ThunkTable::build_specs(specs);
    if (!t.ok()) return t.error();
    p->table_ = std::move(t.value());

    // Data page: [3 stream variables (8 bytes each)] [3 FILE objects]. Written once, then read-only.
    const size_t bytes = 64 + 3 * kFileObjectSize;
    auto region = memory::Region::reserve(0, memory::page_size(), false);
    if (!region.ok()) return region.error();
    p->data_ = std::move(region.value());
    if (auto c = p->data_.commit(0, p->data_.size()); !c.ok()) return c.error();
    uint8_t* base = p->data_.base();
    std::memset(base, 0, bytes);
    for (int i = 0; i < 3; ++i) {
        uint8_t* file = base + 64 + static_cast<size_t>(i) * kFileObjectSize;
        std::memcpy(file, &kFileMagic, 8);
        const uint64_t fd = static_cast<uint64_t>(i);
        std::memcpy(file + 8, &fd, 8);
        p->files_[i] = reinterpret_cast<uint64_t>(file);
        const uint64_t ptr = p->files_[i];
        std::memcpy(base + static_cast<size_t>(i) * 8, &ptr, 8);
    }
    p->data_index_["___stdinp"] = reinterpret_cast<uint64_t>(base);
    p->data_index_["___stdoutp"] = reinterpret_cast<uint64_t>(base + 8);
    p->data_index_["___stderrp"] = reinterpret_cast<uint64_t>(base + 16);
    if (auto pr = p->data_.protect(0, p->data_.size(), memory::Prot::Read); !pr.ok()) return pr.error();
    return p;
}

std::optional<dyld::ResolvedSymbol> LibSystemProvider::lookup(std::string_view library, std::string_view symbol) {
    if (!library.empty() && library != kInstallName) return std::nullopt;
    if (auto it = index_.find(symbol); it != index_.end()) return dyld::ResolvedSymbol{table_.address(it->second), true};
    if (auto it = data_index_.find(symbol); it != data_index_.end()) return dyld::ResolvedSymbol{it->second, false};
    return std::nullopt;
}

bool LibSystemProvider::provides_library(std::string_view install_name) const { return install_name == kInstallName; }

void LibSystemProvider::install_stdio(RuntimeContext& ctx) const {
    ctx.file_stdin = files_[0];
    ctx.file_stdout = files_[1];
    ctx.file_stderr = files_[2];
}

bool is_implemented_data(std::string_view s) {
    for (const auto& d : implemented_data_symbols())
        if (s == d.name) return true;
    return false;
}

}  // namespace cider::libsystem
