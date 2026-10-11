#include "orvyn/dyld.hpp"

#include <optional>

#include "orvyn/util.hpp"

namespace orvyn::dyld {
namespace {
constexpr std::string_view kExec = "@executable_path", kLoader = "@loader_path", kRpath = "@rpath";

bool has_anchor_prefix(std::string_view n, std::string_view a) {
    return starts_with(n, a) && (n.size() == a.size() || n[a.size()] == '/');
}

std::filesystem::path tail_join(const std::filesystem::path& base, std::string_view rest) {
    if (!rest.empty() && rest.front() == '/') rest.remove_prefix(1);
    return (base / path_from_utf8(rest)).lexically_normal();
}

// Expands @executable_path / @loader_path inside an rpath entry. Entries that still start
// with @rpath are invalid and ignored.
std::optional<std::filesystem::path> expand_rpath_entry(std::string_view e, const PathContext& ctx) {
    if (has_anchor_prefix(e, kExec)) return tail_join(ctx.executable_dir, e.substr(kExec.size()));
    if (has_anchor_prefix(e, kLoader)) return tail_join(ctx.loader_dir, e.substr(kLoader.size()));
    if (starts_with(e, "@")) return std::nullopt;
    return path_from_utf8(e).lexically_normal();
}
}  // namespace

bool is_system_path(std::string_view n) noexcept {
    return starts_with(n, "/usr/lib/") || starts_with(n, "/System/Library/") || starts_with(n, "/System/iOSSupport/");
}

Anchor anchor_of(std::string_view n) noexcept {
    if (has_anchor_prefix(n, kExec)) return Anchor::ExecutablePath;
    if (has_anchor_prefix(n, kLoader)) return Anchor::LoaderPath;
    if (has_anchor_prefix(n, kRpath)) return Anchor::Rpath;
    return Anchor::None;
}

std::vector<std::filesystem::path> candidates(std::string_view n, const PathContext& ctx) {
    std::vector<std::filesystem::path> out;
    switch (anchor_of(n)) {
        case Anchor::ExecutablePath: out.push_back(tail_join(ctx.executable_dir, n.substr(kExec.size()))); break;
        case Anchor::LoaderPath: out.push_back(tail_join(ctx.loader_dir, n.substr(kLoader.size()))); break;
        case Anchor::Rpath:
            for (auto& r : ctx.rpaths)
                if (auto base = expand_rpath_entry(r, ctx)) out.push_back(tail_join(*base, n.substr(kRpath.size())));
            break;
        case Anchor::None:
            if (!n.empty() && n.front() == '/') out.push_back(path_from_utf8(n));
            break;
    }
    return out;
}

std::vector<std::filesystem::path> search_candidates(std::string_view n, const std::filesystem::path& executable_dir,
                                                     const std::filesystem::path& loader_dir, const std::vector<RpathEntry>& rpaths) {
    if (anchor_of(n) != Anchor::Rpath) return candidates(n, PathContext{executable_dir, loader_dir, {}});
    std::vector<std::filesystem::path> out;
    for (const auto& r : rpaths) {
        PathContext ctx{executable_dir, r.owner_dir, {}};
        if (auto base = expand_rpath_entry(r.path, ctx)) out.push_back(tail_join(*base, n.substr(kRpath.size())));
    }
    return out;
}

}  // namespace orvyn::dyld
