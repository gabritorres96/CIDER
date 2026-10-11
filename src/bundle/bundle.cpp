#include "orvyn/bundle.hpp"

#include <algorithm>

#include "orvyn/log.hpp"
#include "orvyn/util.hpp"

namespace fs = std::filesystem;

namespace orvyn::bundle {
namespace {

bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}
bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

// A bundle executable name must be a single path component: no separators, no "." / "..".
bool safe_component(const std::string& n) {
    return !n.empty() && n != "." && n != ".." && n.find('/') == std::string::npos &&
           n.find('\\') == std::string::npos && n.find(':') == std::string::npos &&
           n.find('\0') == std::string::npos;
}

std::vector<std::string> archs_of(const fs::path& bin, const macho::Limits& limits) {
    std::vector<std::string> out;
    auto bytes = read_file(bin, limits.max_file_size);
    if (!bytes.ok()) return out;
    macho::ParseOptions po;
    po.limits = limits;
    auto f = macho::parse_file(ByteView(bytes.value()), po);
    if (!f.ok()) return out;
    for (auto& s : f.value().slices) out.push_back(macho::cpu_name(s.cputype, s.cpusubtype));
    return out;
}

std::optional<fs::path> framework_binary(const fs::path& fw, const std::string& stem) {
    std::vector<fs::path> tries = {fw / "Versions" / "Current" / stem, fw / stem};
    std::error_code ec;
    if (is_dir(fw / "Versions"))
        for (auto& v : fs::directory_iterator(fw / "Versions", ec)) tries.push_back(v.path() / stem);
    for (auto& t : tries)
        if (is_file(t)) return t;
    return std::nullopt;
}

void scan_frameworks(Info& info, const macho::Limits& limits) {
    std::error_code ec;
    fs::path dir = info.root / "Contents" / "Frameworks";
    std::vector<fs::path> entries;
    for (auto& e : fs::directory_iterator(dir, ec)) entries.push_back(e.path());
    std::sort(entries.begin(), entries.end());
    for (auto& p : entries) {
        std::string fname = path_to_utf8(p.filename());
        Framework fw;
        fw.name = fname;
        fw.path = p;
        if (ends_with_icase(fname, ".framework") && is_dir(p)) {
            fw.binary = framework_binary(p, fname.substr(0, fname.size() - 10));
        } else if (ends_with_icase(fname, ".dylib") && is_file(p)) {
            fw.binary = p;
        } else continue;
        if (fw.binary) fw.architectures = archs_of(*fw.binary, limits);
        info.frameworks.push_back(std::move(fw));
    }
}

}  // namespace

bool looks_like_bundle(const fs::path& p) {
    if (!is_dir(p)) return false;
    std::string s = path_to_utf8(p);
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return is_dir(p / "Contents") || ends_with_icase(s, ".app");
}

Result<Info> inspect(const fs::path& root_in, const macho::Limits& limits) {
    if (!is_dir(root_in)) return Error{ErrorCode::NotFound, "not a directory: " + path_to_utf8(root_in)};
    Info info;
    info.root = root_in;
    // normalise trailing separators ("Foo.app/")
    if (!info.root.has_filename() && info.root.has_parent_path()) info.root = info.root.parent_path();
    log::info("Analysing bundle {}", path_to_utf8(info.root));

    fs::path contents = info.root / "Contents";
    info.has_contents = is_dir(contents);
    if (!info.has_contents) {
        info.issues.emplace_back("no Contents/ directory: not a macOS application bundle "
                                 "(flat iOS-style bundles are not supported)");
        return info;
    }
    info.has_macos_dir = is_dir(contents / "MacOS");
    info.has_resources = is_dir(contents / "Resources");
    info.has_frameworks = is_dir(contents / "Frameworks");
    info.has_code_signature_dir = is_dir(contents / "_CodeSignature");

    fs::path plist_path = contents / "Info.plist";
    info.has_info_plist = is_file(plist_path);
    if (!info.has_info_plist) {
        info.issues.emplace_back("Contents/Info.plist is missing");
    } else {
        auto bytes = read_file(plist_path, 16u << 20);
        if (!bytes.ok()) {
            info.issues.push_back("Info.plist unreadable: " + bytes.error().message);
        } else {
            auto pl = plist::parse(ByteView(bytes.value()));
            if (!pl.ok()) info.issues.push_back("Info.plist invalid: " + pl.error().message);
            else if (!pl.value().is_dict()) info.issues.emplace_back("Info.plist root is not a dictionary");
            else info.info_plist = std::move(pl.value());
        }
    }

    if (info.info_plist) {
        const auto& pl = *info.info_plist;
        auto s = [&](const char* k) { return pl.get_string(k).value_or(""); };
        info.executable_name = s("CFBundleExecutable");
        info.identifier = s("CFBundleIdentifier");
        info.name = s("CFBundleName");
        if (info.name.empty()) info.name = s("CFBundleDisplayName");
        info.version = s("CFBundleVersion");
        info.short_version = s("CFBundleShortVersionString");
        info.package_type = s("CFBundlePackageType");
        info.min_system_version = s("LSMinimumSystemVersion");
        if (info.package_type.size() && info.package_type != "APPL")
            info.issues.push_back("CFBundlePackageType is '" + info.package_type + "', expected APPL");
        if (const plist::Value* v = pl.get("LSRequiresIPhoneOS"); v && v->type == plist::Value::Type::Bool && v->b)
            info.issues.emplace_back("LSRequiresIPhoneOS is true: iOS application, not supported");
        if (pl.get("UIDeviceFamily")) info.issues.emplace_back("UIDeviceFamily present: iOS application, not supported");
    }
    if (info.name.empty()) info.name = path_to_utf8(info.root.stem());

    if (info.executable_name.empty()) {
        if (info.has_info_plist) info.issues.emplace_back("CFBundleExecutable missing from Info.plist");
    } else if (!safe_component(info.executable_name)) {
        info.issues.push_back("CFBundleExecutable is not a plain file name (rejected): " + sanitize_utf8(info.executable_name));
    } else {
        info.executable_path = contents / "MacOS" / path_from_utf8(info.executable_name);
        info.executable_found = is_file(info.executable_path);
        if (!info.executable_found) info.issues.push_back("main executable not found: Contents/MacOS/" + info.executable_name);
    }

    if (info.has_frameworks) scan_frameworks(info, limits);
    log::debug("Bundle id={} executable={} frameworks={}", info.identifier, info.executable_name, info.frameworks.size());
    return info;
}

}  // namespace orvyn::bundle
