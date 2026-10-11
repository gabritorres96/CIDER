// libFuzzer harness for the Mach-O LOADER: validation, layout, mapping, fixups, protections. The guest
// is NEVER executed (mutated machine code would be arbitrary); only the loading pipeline is exercised, and
// the mapping is released at the end of every iteration.
#include <cstddef>
#include <cstdint>
#include <memory>

#include "orvyn/libsystem.hpp"
#include "orvyn/loader.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static std::unique_ptr<orvyn::libsystem::LibSystemProvider> provider = [] {
        auto p = orvyn::libsystem::LibSystemProvider::create();
        return p.ok() ? std::move(p.value()) : nullptr;
    }();
    if (!provider) return 0;
    auto r = orvyn::loader::load_macho(orvyn::ByteView(data, size), *provider);
    (void)r;
    return 0;
}
