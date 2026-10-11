// Structure-aware libFuzzer harness for Objective-C metadata with RELATIVE method lists. The raw-bytes harness (fuzz_objc) almost never reaches the
// method lists because a random Mach-O is rejected long before; this one starts from valid linked fixtures - the absolute original, the same program
// converted to small method lists with direct and with indirect selectors, and the category program - and applies mutations ONLY to the Objective-C
// data (__objc_const, __objc_data, __objc_selrefs, class/category lists). The input is: 1 byte (seed + region), then 6-byte records
// {u16 position inside the region, u32 value} that overwrite 4 bytes (odd records overwrite 1 byte, so single-bit/flag changes happen too).
// Only linking and metadata registration are exercised; no guest code runs.
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "orvyn/libsystem.hpp"
#include "orvyn/linker.hpp"
#include "orvyn/macho.hpp"
#include "orvyn/objc.hpp"
#include "relmeth.hpp"

namespace {

struct Seed {
    std::vector<uint8_t> bytes;
    std::vector<std::pair<size_t, size_t>> regions;      // file ranges that hold Objective-C data
};

std::vector<uint8_t> slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void add_seed(std::vector<Seed>& out, std::vector<uint8_t> bytes) {
    if (bytes.empty()) return;
    auto parsed = orvyn::macho::parse_file(orvyn::ByteView(bytes));
    if (!parsed.ok() || parsed.value().slices.empty() || !parsed.value().slices[0].image) return;
    Seed s;
    for (const auto& seg : parsed.value().slices[0].image->segments)
        for (const auto& sec : seg.sections)
            if (sec.name == "__objc_const" || sec.name == "__objc_data" || sec.name == "__objc_selrefs" || sec.name == "__objc_classlist" ||
                sec.name == "__objc_catlist" || sec.name == "__objc_methname" || sec.name == "__objc_protolist")
                if (sec.size && sec.offset && sec.offset + sec.size <= bytes.size()) s.regions.emplace_back(sec.offset, static_cast<size_t>(sec.size));
    if (s.regions.empty()) return;
    s.bytes = std::move(bytes);
    out.push_back(std::move(s));
}

const std::vector<Seed>& seeds() {
    static const std::vector<Seed> all = [] {
        std::vector<Seed> v;
        const std::filesystem::path dir = std::filesystem::path(ORVYN_OBJC_FIXTURES_DIR) / "bin";
        for (const char* name : {"objc_relative", "objc_category"}) {
            auto b = slurp(dir / name / name);
            add_seed(v, b);
            if (std::string(name) == "objc_relative")
                for (auto mode : {orvyn::testing::relmeth::Mode::Direct, orvyn::testing::relmeth::Mode::Indirect})
                    if (auto r = orvyn::testing::relmeth::to_relative(b, mode)) add_seed(v, *r);
        }
        return v;
    }();
    return all;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static std::unique_ptr<orvyn::libsystem::LibSystemProvider> provider = [] {
        auto p = orvyn::libsystem::LibSystemProvider::create();
        return p.ok() ? std::move(p.value()) : nullptr;
    }();
    if (!provider || seeds().empty() || size < 1) return 0;
    const Seed& seed = seeds()[data[0] % seeds().size()];
    const auto& region = seed.regions[(data[0] / 7) % seed.regions.size()];
    std::vector<uint8_t> bytes = seed.bytes;
    int n = 0;
    for (size_t i = 1; i + 6 <= size && n < 64; i += 6, ++n) {
        const size_t pos = region.first + (static_cast<size_t>(data[i]) | (static_cast<size_t>(data[i + 1]) << 8)) % region.second;
        if (n & 1) bytes[pos] = data[i + 2];
        else if (pos + 4 <= bytes.size()) std::memcpy(bytes.data() + pos, data + i + 2, 4);
    }
    auto ctx = std::make_unique<orvyn::libsystem::RuntimeContext>();
    ctx->objc = std::make_unique<orvyn::objc::Runtime>(*ctx);
    orvyn::objc::Runtime* rt = ctx->objc.get();
    orvyn::objc::install_foundation(*rt);
    std::string why;
    rt->build_native_thunks(why);
    provider->set_dynamic_data([rt](std::string_view s) { return rt->dynamic_symbol(s); });
    orvyn::linker::LinkOptions opts;
    opts.executable_path = "fuzz-objc-relative";
    opts.before_protect = [rt](orvyn::linker::ImageRecord& image, std::string& error) { return rt->prepare_image(image, error); };
    orvyn::linker::LinkError err;
    auto program = orvyn::linker::Program::link(orvyn::ByteView(bytes.data(), bytes.size()), opts, *provider, err);
    if (program) rt->finish_images(why);
    program.reset();
    provider->set_dynamic_data(nullptr);
    return 0;
}
