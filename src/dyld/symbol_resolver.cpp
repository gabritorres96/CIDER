#include <algorithm>

#include "orvyn/fixups.hpp"
#include "orvyn/log.hpp"

namespace orvyn::dyld {

ResolutionReport resolve_symbols(const macho::Image& img, const std::vector<Fixup>& fixups, SymbolProvider& provider) {
    ResolutionReport rep;
    auto note_library = [&](const std::string& lib) {
        if (std::find(rep.libraries.begin(), rep.libraries.end(), lib) == rep.libraries.end()) rep.libraries.push_back(lib);
    };
    std::map<std::pair<std::string, int64_t>, bool> seen;  // (symbol, ordinal) already handled

    for (const auto& f : fixups) {
        if (f.kind != Fixup::Kind::Bind) continue;
        if (!seen.emplace(std::make_pair(f.symbol, f.library_ordinal), true).second) continue;

        std::string library;
        Unresolved fail;
        fail.symbol = f.symbol;
        fail.weak = f.weak_import;
        bool ok = false;
        if (f.library_ordinal >= 1) {
            size_t idx = static_cast<size_t>(f.library_ordinal) - 1;
            if (idx >= img.dylibs.size()) {
                fail.library = "ordinal " + std::to_string(f.library_ordinal);
                fail.reason = "dylib ordinal outside the load commands";
            } else {
                library = img.dylibs[idx].path;
                fail.library = library;
                note_library(library);
                if (!provider.provides_library(library)) fail.reason = "library not available in ORVYN";
                else ok = true;
            }
        } else if (f.library_ordinal == -2) {
            fail.library = "(flat lookup)";
            ok = true;  // searched by name only, in the libraries ORVYN provides
        } else {
            fail.library = f.library_ordinal == 0 ? "(self)" : "(main executable)";
            fail.reason = "binding to the image itself is not supported yet";
        }

        if (ok) {
            if (auto r = provider.lookup(library, f.symbol)) {
                rep.addresses[f.symbol] = r->address;
                log::debug(log::Category::Symbols, "resolved {} -> {:#x}", f.symbol, r->address);
                continue;
            }
            fail.reason = "symbol not implemented by ORVYN";
        }
        if (f.weak_import) {
            rep.weak_missing.push_back(f.symbol);
            rep.addresses[f.symbol] = 0;
            log::warn(log::Category::Symbols, "weak import {} unavailable: bound to null", f.symbol);
        } else {
            rep.unresolved.push_back(std::move(fail));
        }
    }
    return rep;
}

}  // namespace orvyn::dyld
