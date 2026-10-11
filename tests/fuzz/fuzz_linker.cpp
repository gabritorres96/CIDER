// libFuzzer harness for the multi-image dynamic linker: the input is a main executable that is linked together with
// the dylibs that sit next to the seed fixtures (ORVYN_FUZZ_DIR, default: the current directory). Only linking is
// exercised (dependency search, validation, mapping, binding, TLS/initializer collection); guest code is never run.
#include <cstdlib>
#include <filesystem>
#include <memory>

#include "orvyn/libsystem.hpp"
#include "orvyn/linker.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static std::unique_ptr<orvyn::libsystem::LibSystemProvider> provider = [] {
        auto p = orvyn::libsystem::LibSystemProvider::create();
        return p.ok() ? std::move(p.value()) : nullptr;
    }();
    if (!provider) return 0;
    static const std::filesystem::path dir = [] {
        const char* d = std::getenv("ORVYN_FUZZ_DIR");
        return std::filesystem::absolute(d ? d : ".");
    }();
    orvyn::linker::LinkOptions opts;
    opts.executable_path = dir / "fuzz-main";
    orvyn::linker::LinkError err;
    auto program = orvyn::linker::Program::link(orvyn::ByteView(data, size), opts, *provider, err);
    (void)program;
    return 0;
}
