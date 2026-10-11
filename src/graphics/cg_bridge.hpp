#pragma once
#include <vector>

#include "orvyn/libsystem.hpp"

namespace orvyn::graphics {
void add_cg_symbols(std::vector<libsystem::HostSymbol>& table);   // _CGRect*, _NSPointInRect... (raw handlers)
void install_cg(objc::Runtime& rt);                                  // constants: _CGRectZero, _NSZeroRect...
}  // namespace orvyn::graphics
