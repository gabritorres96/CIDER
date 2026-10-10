// libFuzzer harness for the TAPI text stub reader (cider::sdk::parse_tbd). Pure parsing; nothing is loaded or executed.
#include <string_view>

#include "cider/sdk.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    (void)cider::sdk::parse_tbd(std::string_view(reinterpret_cast<const char*>(data), size));
    return 0;
}
