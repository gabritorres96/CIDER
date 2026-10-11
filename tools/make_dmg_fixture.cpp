// Writes the synthetic DMG fixtures used by the tests to a directory (for manual experiments).
//   make_dmg_fixture <output-dir>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "dmg_builder.hpp"

using namespace orvyn::testing;

static void save(const std::filesystem::path& p, const std::vector<uint8_t>& b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    std::cout << "wrote " << p.string() << " (" << b.size() << " bytes)\n";
}

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: make_dmg_fixture <output-dir>\n"; return 2; }
    std::filesystem::path out = argv[1];
    std::filesystem::create_directories(out);
    auto iso = build_iso(example_app_tree());
    save(out / "example-raw.dmg", build_dmg(iso).bytes);
    DmgOptions z;
    z.codec = DmgCodec::ZlibStored;
    save(out / "example-zlib.dmg", build_dmg(iso, z).bytes);
    save(out / "example.iso", iso);
    // seeds for the Mach-O / plist fuzzers
    MachoSpec full;
    full.code_signature = true;
    full.rpaths = {"@loader_path/lib"};
    full.weak_dylibs = {"@rpath/w.dylib"};
    MachoSpec arm;
    arm.cputype = 0x0100000c;
    arm.cpusubtype = 0;
    save(out / "macho-thin.bin", build_macho(full).bytes);
    save(out / "macho-fat.bin", build_fat({{0x01000007, 3, build_macho(full).bytes}, {0x0100000c, 0, build_macho(arm).bytes}}));
    const std::string xml = xml_plist({{"CFBundleExecutable", "Example"}});
    save(out / "info.plist", std::vector<uint8_t>(xml.begin(), xml.end()));
    save(out / "info-binary.plist", binary_plist({{"Name", "Example"}, {"Id", "com.x"}}));
    return 0;
}
