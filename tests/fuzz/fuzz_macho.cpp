// libFuzzer harness for the Mach-O/FAT parser. Build with -DORVYN_BUILD_FUZZERS=ON using clang
// (-fsanitize=fuzzer,address) or MSVC with the AddressSanitizer component installed.
#include <cstddef>
#include <cstdint>

#include "orvyn/macho.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    orvyn::macho::ParseOptions opts;
    opts.parse_symbols = true;
    (void)orvyn::macho::parse_file(orvyn::ByteView(data, size), opts);
    return 0;
}
