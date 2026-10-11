#include "orvyn/analysis.hpp"

#include <deque>
#include <set>

#include "orvyn/dyld.hpp"
#include "orvyn/log.hpp"
#include "orvyn/util.hpp"

namespace fs = std::filesystem;

namespace orvyn::analysis {

const char* to_string(DepClass c) noexcept {
    switch (c) {
        case DepClass::System: return "system";
        case DepClass::Bundled: return "bundled";
        case DepClass::Missing: return "missing";
        case DepClass::External: return "external";
        case DepClass::OutsideBundle: return "outside-bundle";
    }
    return "?";
}

const char* to_string(CompatStatus s) noexcept {
    switch (s) {
        case CompatStatus::NotAnalyzed: return "not analyzed";
        case CompatStatus::AnalysisCompatible: return "analysis compatible";
        case CompatStatus::PendingDependencies: return "pending dependencies";
        case CompatStatus::PartialExecution: return "partial execution";
        case CompatStatus::Executable: return "executable";
        case CompatStatus::ExecutableWithIssues: return "executable with issues";
        case CompatStatus::NotCompatible: return "not compatible";
    }
    return "?";
}

size_t DependencyReport::count(DepClass c) const {
    size_t n = 0;
    for (auto& d : deps) n += d.cls == c;
    return n;
}

const macho::Slice* select_slice(const macho::File& f) noexcept {
    const macho::Slice* s = f.find_slice(macho::kCpuX86_64);
    return s && s->image ? s : nullptr;
}

namespace {
bool exists_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

struct Pending {
    macho::Image image;
    fs::path path;
    std::vector<std::string> inherited_rpaths;  // loader's own rpaths first, then ancestors'
    bool direct;
};
}  // namespace

DependencyReport analyze_dependencies(const macho::Image& exe, const fs::path& exe_path,
                                      const std::optional<fs::path>& bundle_root, const macho::Limits& limits) {
    DependencyReport rep;
    constexpr size_t kMaxImages = 512;
    std::set<std::string> visited;
    std::deque<Pending> queue;
    fs::path exe_dir = exe_path.parent_path();
    visited.insert(path_to_utf8(exe_path.lexically_normal()));
    queue.push_back({exe, exe_path, exe.rpaths, true});

    while (!queue.empty()) {
        Pending cur = std::move(queue.front());
        queue.pop_front();
        if (++rep.images_scanned > kMaxImages) {
            rep.notes.push_back("image limit reached; dependency closure is incomplete");
            break;
        }
        dyld::PathContext ctx{exe_dir, cur.path.parent_path(), cur.inherited_rpaths};
        std::string requirer = path_to_utf8(cur.path.filename());

        for (const auto& d : cur.image.dylibs) {
            Dependency dep;
            dep.kind = d.kind;
            dep.install_name = d.path;
            dep.required_by = requirer;
            dep.direct = cur.direct;

            if (dyld::is_system_path(d.path)) {
                dep.cls = DepClass::System;
            } else if (dyld::anchor_of(d.path) == dyld::Anchor::None && !d.path.empty() && d.path.front() == '/') {
                dep.cls = DepClass::External;
            } else {
                auto cands = dyld::candidates(d.path, ctx);
                dep.cls = DepClass::Missing;
                fs::path resolved_path;  // original path object (may be extended-length): no UTF-8 round trip
                for (auto& c : cands) {
                    dep.tried.push_back(path_to_utf8(c));
                    if (!exists_file(c)) continue;
                    dep.resolved = path_to_utf8(c);
                    resolved_path = c;
                    dep.cls = (bundle_root && !is_inside(*bundle_root, c)) ? DepClass::OutsideBundle : DepClass::Bundled;
                    break;
                }
                if (dep.cls == DepClass::Bundled || dep.cls == DepClass::OutsideBundle) {
                    fs::path rp = resolved_path;
                    std::string key = path_to_utf8(rp.lexically_normal());
                    if (visited.insert(key).second) {
                        auto bytes = read_file(rp, limits.max_file_size);
                        if (bytes.ok()) {
                            macho::ParseOptions po;
                            po.limits = limits;
                            auto f = macho::parse_file(ByteView(bytes.value()), po);
                            const macho::Slice* sl = f.ok() ? f.value().find_slice(exe.cputype) : nullptr;
                            if (sl && sl->image) {
                                std::vector<std::string> inh = sl->image->rpaths;
                                inh.insert(inh.end(), cur.inherited_rpaths.begin(), cur.inherited_rpaths.end());
                                queue.push_back({*sl->image, rp, std::move(inh), false});
                            } else {
                                rep.notes.push_back("could not analyse bundled dependency " + dep.resolved);
                            }
                        } else {
                            rep.notes.push_back("could not read bundled dependency " + dep.resolved + ": " + bytes.error().message);
                        }
                    }
                }
            }
            log::debug("dependency {} ({}) -> {}", d.path, macho::dylib_kind_name(d.kind), to_string(dep.cls));
            rep.deps.push_back(std::move(dep));
        }
    }
    return rep;
}

CompatReport assess(const macho::File& file, const DependencyReport* deps, const bundle::Info* bundle) {
    CompatReport r;
    r.status = CompatStatus::AnalysisCompatible;
    auto fail = [&](std::string why) {
        r.status = CompatStatus::NotCompatible;
        r.reasons.push_back(std::move(why));
    };

    const macho::Slice* x86 = file.find_slice(macho::kCpuX86_64);
    if (!x86) {
        std::string have;
        for (auto& s : file.slices) have += (have.empty() ? "" : ", ") + macho::cpu_name(s.cputype, s.cpusubtype);
        fail("no x86_64 slice (found: " + have + "); only x86_64 on Windows x86_64 is targeted in this release");
        return r;
    }
    if (!x86->image) {
        fail("x86_64 slice failed validation: " + (x86->error ? x86->error->message : std::string("unknown error")));
        return r;
    }
    const macho::Image& img = *x86->image;

    bool macos = img.build_versions.empty();
    for (auto& b : img.build_versions) macos |= b.platform == 1;
    if (!macos) fail(std::string("platform is ") + macho::platform_name(img.build_versions[0].platform) + ", not macOS");
    if (img.encrypted) fail("image is encrypted (LC_ENCRYPTION_INFO_64 cryptid != 0)");
    if (img.filetype != macho::kFileExecute) r.reasons.push_back(std::string("file type is '") + macho::filetype_name(img.filetype) + "', not an executable");
    if (bundle)
        for (auto& i : bundle->issues) {
            if (i.find("iOS") != std::string::npos) fail(i);
            else r.reasons.push_back("bundle: " + i);
        }
    if (r.status == CompatStatus::NotCompatible) return r;

    if (deps) {
        size_t missing = 0;
        for (auto& d : deps->deps)
            if (d.cls == DepClass::Missing && d.kind != macho::DylibKind::Weak) ++missing;
        if (missing) {
            r.status = CompatStatus::PendingDependencies;
            r.reasons.push_back(std::format("{} required dependenc{} could not be resolved", missing, missing == 1 ? "y" : "ies"));
        }
        if (deps->count(DepClass::System))
            r.reasons.push_back("system libraries (libSystem, frameworks) are not provided by ORVYN yet");
    }
    r.reasons.emplace_back("static analysis only: execution is not confirmed (no runtime in this release)");
    return r;
}

}  // namespace orvyn::analysis
