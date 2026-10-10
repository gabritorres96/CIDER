// libFuzzer harness for the UDIF (DMG) structure parser. The parser reads from files, so each
// input is written to a temporary file first.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>

#include "cider/dmg.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static const std::filesystem::path path = std::filesystem::temp_directory_path() / "cider-fuzz-udif.bin";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    (void)cider::dmg::inspect_file(path);
    return 0;
}
