// libFuzzer harness for the XML/binary plist reader.
#include <cstddef>
#include <cstdint>

#include "orvyn/plist.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    (void)orvyn::plist::parse(orvyn::ByteView(data, size));
    return 0;
}
