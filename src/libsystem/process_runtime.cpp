#include <algorithm>
#include <cstdio>

#include "cider/libsystem.hpp"
#include "cider/log.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace cider::libsystem {
namespace {
RuntimeContext* g_ctx = nullptr;
}

RuntimeContext::~RuntimeContext() {
#ifdef _WIN32
    if (heap) HeapDestroy(heap);
#endif
}

void set_context(RuntimeContext* ctx) { g_ctx = ctx; }
RuntimeContext* context() { return g_ctx; }

Result<std::unique_ptr<LibSystemProvider>> LibSystemProvider::create() {
    auto p = std::unique_ptr<LibSystemProvider>(new LibSystemProvider());
    std::vector<void*> fns;
    for (const auto& s : implemented_symbols()) {
        p->index_[s.name] = fns.size();
        fns.push_back(s.fn);
    }
    auto t = abi::ThunkTable::build(fns);
    if (!t.ok()) return t.error();
    p->table_ = std::move(t.value());
    return p;
}

std::optional<dyld::ResolvedSymbol> LibSystemProvider::lookup(std::string_view library, std::string_view symbol) {
    if (!library.empty() && library != kInstallName) return std::nullopt;
    auto it = index_.find(symbol);
    if (it == index_.end()) return std::nullopt;
    return dyld::ResolvedSymbol{table_.address(it->second), true};
}

bool LibSystemProvider::provides_library(std::string_view install_name) const { return install_name == kInstallName; }

}  // namespace cider::libsystem
