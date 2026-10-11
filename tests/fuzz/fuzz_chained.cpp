// libFuzzer harness for the dynamic-linking structures: the export trie parser and the chained-fixups decoder.
// Input = a Mach-O image (the decoders locate their data through the load commands) or, if it is not one, a raw trie.
// Nothing is mapped or executed.
#include <cstddef>
#include <cstdint>

#include "orvyn/exports.hpp"
#include "orvyn/fixups.hpp"
#include "orvyn/macho.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    orvyn::ByteView view(data, size);
    (void)orvyn::dyld::parse_export_trie(view);                       // raw trie
    auto file = orvyn::macho::parse_file(view);
    if (!file.ok()) return 0;
    for (const auto& s : file.value().slices) {
        if (!s.image) continue;
        auto slice = view.slice(s.offset, s.size);
        if (!slice.ok()) continue;
        (void)orvyn::dyld::decode_exports(*s.image, slice.value());
        (void)orvyn::dyld::decode_fixups(*s.image, slice.value());    // classic or chained
        (void)orvyn::dyld::decode_chained_fixups(*s.image, slice.value());
    }
    return 0;
}
