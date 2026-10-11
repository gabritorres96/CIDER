#pragma once
// macOS .app bundle analysis (read-only; nothing is executed).
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "orvyn/macho.hpp"
#include "orvyn/plist.hpp"
#include "orvyn/result.hpp"

namespace orvyn::bundle {

struct Framework {
    std::string name;                         // e.g. "Sparkle.framework" or "libfoo.dylib"
    std::filesystem::path path;
    std::optional<std::filesystem::path> binary;
    std::vector<std::string> architectures;   // empty if the binary was not found/parsed
};

struct Info {
    std::filesystem::path root;
    bool has_contents = false, has_info_plist = false, has_macos_dir = false, has_resources = false,
         has_frameworks = false, has_code_signature_dir = false;
    std::optional<plist::Value> info_plist;
    std::string executable_name, identifier, name, version, short_version, package_type, min_system_version;
    std::filesystem::path executable_path;
    bool executable_found = false;
    std::vector<Framework> frameworks;
    std::vector<std::string> issues;  // structural problems / unsupported requirements
};

// True if `p` is a directory that looks like an application bundle (has Contents/ or ends in .app).
bool looks_like_bundle(const std::filesystem::path& p);

Result<Info> inspect(const std::filesystem::path& root, const macho::Limits& limits = {});

}  // namespace orvyn::bundle
