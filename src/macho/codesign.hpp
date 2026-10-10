#pragma once
#include "cider/bytes.hpp"
#include "cider/macho.hpp"

namespace cider::macho {

// Parses the embedded code-signature SuperBlob (big-endian). Structural problems do not fail
// the image; they are reported through `warnings`. No cryptographic verification is done.
void parse_code_signature(ByteView blob, CodeSignature& out, std::vector<std::string>& warnings);

}  // namespace cider::macho
