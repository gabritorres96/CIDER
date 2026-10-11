// libFuzzer harness for the Objective-C metadata parser: the input is a Mach-O executable that is linked with the runtime's
// before_protect hook (selector uniquing, class/category/protocol parsing) and then realized (superclass resolution, categories,
// ivar offset sliding). Only linking and metadata registration are exercised; guest code is never run.
#include <memory>

#include "orvyn/libsystem.hpp"
#include "orvyn/linker.hpp"
#include "orvyn/objc.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static std::unique_ptr<orvyn::libsystem::LibSystemProvider> provider = [] {
        auto p = orvyn::libsystem::LibSystemProvider::create();
        return p.ok() ? std::move(p.value()) : nullptr;
    }();
    if (!provider) return 0;
    auto ctx = std::make_unique<orvyn::libsystem::RuntimeContext>();
    ctx->objc = std::make_unique<orvyn::objc::Runtime>(*ctx);
    orvyn::objc::Runtime* rt = ctx->objc.get();
    orvyn::objc::install_foundation(*rt);
    std::string why;
    rt->build_native_thunks(why);
    provider->set_dynamic_data([rt](std::string_view s) { return rt->dynamic_symbol(s); });
    orvyn::linker::LinkOptions opts;
    opts.executable_path = "fuzz-objc";
    opts.before_protect = [rt](orvyn::linker::ImageRecord& image, std::string& error) { return rt->prepare_image(image, error); };
    orvyn::linker::LinkError err;
    auto program = orvyn::linker::Program::link(orvyn::ByteView(data, size), opts, *provider, err);
    if (program) rt->finish_images(why);
    program.reset();
    provider->set_dynamic_data(nullptr);
    return 0;
}
