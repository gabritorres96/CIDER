#pragma once
// orvyn-dyld (0.1): path-level analysis only. No library is loaded or executed.
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace orvyn::dyld {

struct PathContext {
    std::filesystem::path executable_dir;  // directory of the main executable (@executable_path)
    std::filesystem::path loader_dir;      // directory of the image that names the dependency (@loader_path)
    std::vector<std::string> rpaths;       // LC_RPATH entries, unexpanded, loader first
};

// Install names shipped inside macOS itself (dyld shared cache); never present on disk.
bool is_system_path(std::string_view install_name) noexcept;

// One LC_RPATH entry plus the directory of the image that contains it (needed to expand its @loader_path).
struct RpathEntry {
    std::string path;
    std::filesystem::path owner_dir;
};

enum class Anchor { None, ExecutablePath, LoaderPath, Rpath };
Anchor anchor_of(std::string_view install_name) noexcept;

// Expands one install name to the ordered list of candidate filesystem paths.
// @rpath expands once per rpath entry; unresolvable anchors produce no candidates.
std::vector<std::filesystem::path> candidates(std::string_view install_name, const PathContext& ctx);

// Like candidates(), for the dynamic linker: every rpath entry is expanded relative to the image that owns it.
std::vector<std::filesystem::path> search_candidates(std::string_view install_name, const std::filesystem::path& executable_dir,
                                                     const std::filesystem::path& loader_dir, const std::vector<RpathEntry>& rpaths);

}  // namespace orvyn::dyld
