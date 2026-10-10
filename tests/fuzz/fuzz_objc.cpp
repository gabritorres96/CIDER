// libFuzzer harness for the Objective-C metadata parser: the input is a Mach-O executable that is linked with the runtime's
// before_protect hook (selector uniquing, class/category/protocol parsing) and then realized (superclass resolution, categories,
// ivar offset sliding). Only linking and metadata registration are exercised; guest code is never run.
#include <memory>

#include "cider/libsystem.hpp"
#include "cider/linker.hpp"
#include "cider/objc.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static std::unique_ptr<cider::libsystem::LibSystemProvider> provider = [] {
        auto p = cider::libsystem::LibSystemProvider::create();
        return p.ok() ? std::move(p.value()) : nullptr;
    }();
    if (!provider) return 0;
    auto ctx = std::make_unique<cider::libsystem::RuntimeContext>();
    ctx->objc = std::make_unique<cider::objc::Runtime>(*ctx);
    cider::objc::Runtime* rt = ctx->objc.get();
    cider::objc::install_foundation(*rt);
    std::string why;
    rt->build_native_thunks(why);
    provider->set_dynamic_data([rt](std::string_view s) { return rt->dynamic_symbol(s); });
    cider::linker::LinkOptions opts;
    opts.executable_path = "fuzz-objc";
    opts.before_protect = [rt](cider::linker::ImageRecord& image, std::string& error) { return rt->prepare_image(image, error); };
    cider::linker::LinkError err;
    auto program = cider::linker::Program::link(cider::ByteView(data, size), opts, *provider, err);
    if (program) rt->finish_images(why);
    program.reset();
    provider->set_dynamic_data(nullptr);
    return 0;
}
