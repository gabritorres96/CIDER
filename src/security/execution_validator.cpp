#include <algorithm>
#include <format>

#include "cider/loader.hpp"
#include "cider/log.hpp"

namespace cider::loader {
namespace {
void add(ValidationReport& r, Issue::Kind k, std::string m) { r.issues.push_back({k, std::move(m)}); }
}  // namespace

ValidationReport validate_for_execution(const macho::Image& img, const ValidatorOptions& opts) {
    ValidationReport r;
    using K = Issue::Kind;

    if (img.cputype != macho::kCpuX86_64)
        add(r, K::Incompatible, std::format("architecture is {}, CIDER 0.2 executes x86_64 only", macho::cpu_name(img.cputype, img.cpusubtype)));
    if (opts.as_dependency) {
        if (img.filetype != macho::kFileDylib)
            add(r, K::Incompatible, std::format("file type is '{}', not a dynamic library (MH_DYLIB)", macho::filetype_name(img.filetype)));
    } else if (img.filetype != macho::kFileExecute) {
        add(r, K::Incompatible, std::format("file type is '{}', not an executable (MH_EXECUTE)", macho::filetype_name(img.filetype)));
    }

    if (!img.build_versions.empty()) {
        bool macos = false;
        for (auto& b : img.build_versions) macos |= b.platform == 1;
        if (!macos) add(r, K::Incompatible, std::string("platform is ") + macho::platform_name(img.build_versions[0].platform) + ", not macOS");
    }
    if (img.encrypted) add(r, K::Incompatible, "image is encrypted (LC_ENCRYPTION_INFO_64 cryptid != 0)");

    if (opts.as_dependency) {
        // A dylib has no entry point; it is reached through its exports and its initializers.
    } else if (!img.entry) {
        add(r, K::Incompatible, "no entry point (LC_MAIN)");
    } else if (img.entry->kind != macho::EntryPoint::Kind::Main) {
        add(r, K::Unsupported, "LC_UNIXTHREAD entry protocol is not supported (only LC_MAIN)");
    }
    if (img.chained_fixups)
        add(r, K::Unsupported, "chained fixups (LC_DYLD_CHAINED_FIXUPS) are not supported yet; link with classic dyld info (-no_fixup_chains)");
    if (img.dyld_info && img.dyld_info->weak_bind.size)
        add(r, K::Unsupported, "weak binding information is not supported yet");

    for (const auto& s : img.segments) {
        for (const auto& sec : s.sections) {
            const uint32_t type = sec.flags & 0xff;
            if (type == 0x11 || type == 0x12 || type == 0x13 || type == 0x14 || type == 0x15)
                add(r, K::Unsupported, std::format("thread-local storage section {},{} is not supported", s.name, sec.name));
            if ((type == 0x9 || type == 0x16) && sec.size && !opts.allow_initializers)
                add(r, K::Unsupported, std::format("static initializers ({},{}) are not supported yet", s.name, sec.name));
            if (type == 0xa && sec.size)
                add(r, K::Unsupported, std::format("static terminators ({},{}) are not supported", s.name, sec.name));
        }
    }

    for (const auto& d : img.dylibs) {
        r.required_libs.push_back(d.path);
        if (opts.linker_resolves_dependencies) continue;   // the dynamic linker searches, loads and checks it
        bool provided = std::find(opts.provided_libraries.begin(), opts.provided_libraries.end(), d.path) != opts.provided_libraries.end();
        if (provided) continue;
        if (d.kind == macho::DylibKind::Weak) {
            log::warn(log::Category::Loader, "weak dependency {} is not provided; symbols bound to it become null", d.path);
            continue;
        }
        add(r, K::MissingDependency, std::format("required library {} is not available in CIDER", d.path));
    }
    if (!img.rpaths.empty() && !opts.linker_resolves_dependencies) log::debug(log::Category::Loader, "ignoring {} LC_RPATH entr(ies): no bundled libraries are loaded", img.rpaths.size());

    // Geometry checks (overflow, alignment, permissions, entry placement) live in plan_layout.
    if (r.issues.empty()) {
        auto plan = plan_layout(img, opts);
        if (!plan.ok()) {
            add(r, plan.error().code == ErrorCode::Unsupported ? K::Unsupported : K::Incompatible, plan.error().message);
        }
    }
    return r;
}

}  // namespace cider::loader
