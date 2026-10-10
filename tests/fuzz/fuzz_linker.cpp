// libFuzzer harness for the multi-image dynamic linker: the input is a main executable that is linked together with
// the dylibs that sit next to the seed fixtures (CIDER_FUZZ_DIR, default: the current directory). Only linking is
// exercised (dependency search, validation, mapping, binding, TLS/initializer collection); guest code is never run.
#include <cstdlib>
#include <filesystem>
#include <memory>

#include "cider/libsystem.hpp"
#include "cider/linker.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static std::unique_ptr<cider::libsystem::LibSystemProvider> provider = [] {
        auto p = cider::libsystem::LibSystemProvider::create();
        return p.ok() ? std::move(p.value()) : nullptr;
    }();
    if (!provider) return 0;
    static const std::filesystem::path dir = [] {
        const char* d = std::getenv("CIDER_FUZZ_DIR");
        return std::filesystem::absolute(d ? d : ".");
    }();
    cider::linker::LinkOptions opts;
    opts.executable_path = dir / "fuzz-main";
    cider::linker::LinkError err;
    auto program = cider::linker::Program::link(cider::ByteView(data, size), opts, *provider, err);
    (void)program;
    return 0;
}
