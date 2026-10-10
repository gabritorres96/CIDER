// libFuzzer harness for the XML/binary plist reader.
#include <cstddef>
#include <cstdint>

#include "cider/plist.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    (void)cider::plist::parse(cider::ByteView(data, size));
    return 0;
}
