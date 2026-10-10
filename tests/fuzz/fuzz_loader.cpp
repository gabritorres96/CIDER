// libFuzzer harness for the Mach-O LOADER: validation, layout, mapping, fixups, protections. The guest
// is NEVER executed (mutated machine code would be arbitrary); only the loading pipeline is exercised, and
// the mapping is released at the end of every iteration.
#include <cstddef>
#include <cstdint>
#include <memory>

#include "cider/libsystem.hpp"
#include "cider/loader.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static std::unique_ptr<cider::libsystem::LibSystemProvider> provider = [] {
        auto p = cider::libsystem::LibSystemProvider::create();
        return p.ok() ? std::move(p.value()) : nullptr;
    }();
    if (!provider) return 0;
    auto r = cider::loader::load_macho(cider::ByteView(data, size), *provider);
    (void)r;
    return 0;
}
