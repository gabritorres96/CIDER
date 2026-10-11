#pragma once
// Apple SDK text stubs (.tbd): a reusable, read-only description of what a system library exports. Nothing here loads or executes a library.
// Supported: TAPI "tbd-version: 4" documents (and the v3 spelling of the keys used below). One file may hold several documents.
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace orvyn::sdk {

struct TbdLibrary {
    std::string install_name;
    std::vector<std::string> targets;                  // file-level targets
    std::set<std::string> symbols;                     // exported C symbols for x86_64-macos (leading underscore kept)
    std::set<std::string> weak_symbols;
    std::set<std::string> thread_local_symbols;
    std::set<std::string> objc_classes;                // without the _OBJC_CLASS_$_ prefix
    std::set<std::string> objc_eh_types;
    std::set<std::string> objc_ivars;                  // "Class.ivar"
    std::vector<std::string> reexported_libraries;     // install names
    bool has_x86_64 = false;
};

struct TbdParse {
    std::vector<TbdLibrary> libraries;
    std::string error;                                 // empty on success
};

// Limits: input <= 64 MiB, <= 4 million names. Malformed input returns an error, never throws.
TbdParse parse_tbd(std::string_view text);

// Public symbols of a family: "Foundation" matches /System/Library/Frameworks/Foundation.framework/..., "libobjc" /usr/lib/libobjc.A.dylib.
std::string library_family(std::string_view install_name);

}  // namespace orvyn::sdk
