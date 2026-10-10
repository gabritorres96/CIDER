// libFuzzer harness for the Mach-O/FAT parser. Build with -DCIDER_BUILD_FUZZERS=ON using clang
// (-fsanitize=fuzzer,address) or MSVC with the AddressSanitizer component installed.
#include <cstddef>
#include <cstdint>

#include "cider/macho.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    cider::macho::ParseOptions opts;
    opts.parse_symbols = true;
    (void)cider::macho::parse_file(cider::ByteView(data, size), opts);
    return 0;
}
