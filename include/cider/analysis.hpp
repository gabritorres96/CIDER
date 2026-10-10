#pragma once
// Static dependency and compatibility analysis. Nothing here executes code.
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "cider/bundle.hpp"
#include "cider/macho.hpp"

namespace cider::analysis {

enum class DepClass {
    System,         // lives in macOS (dyld shared cache); CIDER does not provide it yet
    Bundled,        // resolved to a file inside the bundle / next to the executable
    Missing,        // anchored/relative path with no matching file
    External,       // absolute non-system path (expected on a real macOS install)
    OutsideBundle,  // resolved, but outside the bundle root
};
const char* to_string(DepClass c) noexcept;

struct Dependency {
    macho::DylibKind kind = macho::DylibKind::Load;
    std::string install_name;
    std::string required_by;  // file name of the requiring image
    bool direct = true;       // required by the main executable itself
    DepClass cls = DepClass::Missing;
    std::string resolved;     // UTF-8 path, empty unless Bundled/OutsideBundle
    std::vector<std::string> tried;
};

struct DependencyReport {
    std::vector<Dependency> deps;
    std::vector<std::string> notes;
    size_t images_scanned = 0;
    size_t count(DepClass c) const;
};

// Picks the slice CIDER 0.1 could target (x86_64). nullptr if none.
const macho::Slice* select_slice(const macho::File& f) noexcept;

DependencyReport analyze_dependencies(const macho::Image& exe, const std::filesystem::path& exe_path,
                                      const std::optional<std::filesystem::path>& bundle_root,
                                      const macho::Limits& limits = {});

enum class CompatStatus {
    NotAnalyzed,
    AnalysisCompatible,
    PendingDependencies,
    PartialExecution,        // reserved: needs a runtime (not in 0.1)
    Executable,              // reserved
    ExecutableWithIssues,    // reserved
    NotCompatible,
};
const char* to_string(CompatStatus s) noexcept;

struct CompatReport {
    CompatStatus status = CompatStatus::NotAnalyzed;
    std::vector<std::string> reasons;
    bool execution_confirmed = false;  // always false in 0.1: there is no runtime
};

CompatReport assess(const macho::File& file, const DependencyReport* deps, const bundle::Info* bundle);

}  // namespace cider::analysis
