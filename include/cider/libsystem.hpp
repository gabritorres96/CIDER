#pragma once
// Minimal libSystem for CIDER 0.2: the handful of libc functions the controlled fixtures need,
// implemented on the host with the Windows ABI and exposed to guest code through System V thunks.
//
// IMPLEMENTED (exactly these): puts putchar write exit getpid getenv strlen strcmp strncmp memcpy memmove
// memset memcmp abs labs malloc calloc realloc free sqrt fabs floor ceil pow, plus a trapping
// dyld_stub_binder. Everything else (printf, fopen, fork, signal, ...) is NOT implemented and makes a
// binary that imports it fail to load with "unresolved symbol".
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cider/abi.hpp"
#include "cider/fixups.hpp"
#include "cider/result.hpp"

namespace cider::libsystem {

inline constexpr const char* kInstallName = "/usr/lib/libSystem.B.dylib";

// Per-guest-process state seen by the host functions (single guest at a time).
struct RuntimeContext {
    RuntimeContext() = default;
    ~RuntimeContext();                      // destroys the guest heap: guest allocations can never leak into CIDER
    RuntimeContext(const RuntimeContext&) = delete;
    RuntimeContext& operator=(const RuntimeContext&) = delete;
    void* heap = nullptr;          // private Windows heap behind malloc/calloc/realloc/free (created lazily)
    std::vector<std::string> env;  // "NAME=value" entries returned by getenv()
    // Receives everything the guest writes to fd 1 / fd 2. Default (empty): the real stdout/stderr.
    std::function<void(int fd, const char* data, size_t size)> sink;
    std::set<void*> allocations;   // live malloc blocks (free() of anything else is refused)
    std::string fault;             // set when the guest hits a runtime trap (e.g. lazy binding)
    uint64_t bytes_out = 0, bytes_err = 0, calls = 0;
};

void set_context(RuntimeContext* ctx);
RuntimeContext* context();

struct HostSymbol {
    const char* name;  // as it appears in the Mach-O (leading underscore)
    void* fn;          // Windows x64 function
};
const std::vector<HostSymbol>& implemented_symbols();
bool is_implemented(std::string_view mach_o_symbol);

// Resolves symbols of /usr/lib/libSystem.B.dylib to System V thunks. Owns the thunk pages: keep it alive
// for as long as the guest can run.
class LibSystemProvider final : public dyld::SymbolProvider {
public:
    static Result<std::unique_ptr<LibSystemProvider>> create();
    std::optional<dyld::ResolvedSymbol> lookup(std::string_view library, std::string_view symbol) override;
    bool provides_library(std::string_view install_name) const override;

private:
    abi::ThunkTable table_;
    std::map<std::string, size_t, std::less<>> index_;
};

}  // namespace cider::libsystem
