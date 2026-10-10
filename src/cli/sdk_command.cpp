// `cider sdk-scan <file.tbd | directory> [--json]`: reads Apple text stubs (.tbd) and compares what the libraries export with what CIDER
// implements. Exported is not functional: "implemented" means a host function / class exists, not that its behaviour matches Apple's.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "cider/json.hpp"
#include "cider/libsystem.hpp"
#include "cider/objc.hpp"
#include "cider/sdk.hpp"
#include "commands.hpp"

namespace fs = std::filesystem;

namespace cider::cli {
namespace {

struct Family {
    const char* name;
    libsystem::Lib lib;
};
const Family kFamilies[] = {{"libobjc", libsystem::Lib::Objc},           {"libSystem", libsystem::Lib::System},
                            {"Foundation", libsystem::Lib::Foundation}, {"CoreFoundation", libsystem::Lib::CoreFoundation},
                            {"AppKit", libsystem::Lib::AppKit},         {"CoreGraphics", libsystem::Lib::CoreGraphics},
                            {"libdispatch", libsystem::Lib::System}};

struct Row {
    std::string family, install_name;
    size_t symbols = 0, classes = 0, impl_symbols = 0, impl_classes = 0;
    size_t impl_extra_symbols = 0;                // implemented, but not exported by this SDK's tbd
};

bool read_text(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

int cmd_sdk_scan(const Options& o) {
    if (o.positional.empty()) { std::fprintf(stderr, "usage: cider sdk-scan <file.tbd | directory> [--json]\n"); return 2; }
    fs::path root = fs::path(std::u8string(o.positional[0].begin(), o.positional[0].end()));
#ifdef _WIN32
    {   // SDK trees contain paths longer than MAX_PATH: use the extended-length form
        std::error_code aec;
        fs::path abs = fs::absolute(root, aec).lexically_normal().make_preferred();
        if (!aec && abs.native().rfind(L"\\\\?\\", 0) != 0) root = fs::path(std::wstring(L"\\\\?\\") + abs.native());
    }
#endif
    std::error_code ec;
    std::vector<fs::path> files;
    if (fs::is_directory(root, ec)) {
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec))
            if (it->is_regular_file(ec) && !it->is_symlink(ec) && it->path().extension() == ".tbd") files.push_back(it->path());
    } else files.push_back(root);

    std::map<std::string, sdk::TbdLibrary> by_install;           // first document wins (Versions/A and Current stubs are duplicates)
    size_t parsed = 0, failed = 0;
    for (const auto& f : files) {
        std::string text;
        if (!read_text(f, text)) { ++failed; continue; }
        auto r = sdk::parse_tbd(text);
        if (!r.error.empty()) { ++failed; continue; }
        ++parsed;
        for (auto& l : r.libraries) {
            auto it = by_install.find(l.install_name);
            if (it == by_install.end()) by_install.emplace(l.install_name, std::move(l));
            else if (!it->second.has_x86_64 && l.has_x86_64) it->second = std::move(l);      // prefer the macOS x86_64 description over iOSSupport/macabi duplicates
        }
    }

    libsystem::RuntimeContext ctx;
    ctx.objc = std::make_unique<objc::Runtime>(ctx);
    objc::install_foundation(*ctx.objc);

    std::vector<Row> rows;
    for (const auto& fam : kFamilies) {
        const sdk::TbdLibrary* main = nullptr;
        for (auto& [inst, lib] : by_install)
            if (sdk::library_family(inst) == fam.name && lib.has_x86_64 && (!main || inst.size() < main->install_name.size())) main = &lib;
        if (!main) continue;
        std::set<std::string> syms = main->symbols, classes = main->objc_classes;
        std::vector<std::string> todo = main->reexported_libraries;
        std::set<std::string> seen{main->install_name};
        while (!todo.empty()) {
            std::string n = todo.back();
            todo.pop_back();
            if (!seen.insert(n).second) continue;
            auto it = by_install.find(n);
            if (it == by_install.end()) continue;
            syms.insert(it->second.symbols.begin(), it->second.symbols.end());
            classes.insert(it->second.objc_classes.begin(), it->second.objc_classes.end());
            for (auto& r : it->second.reexported_libraries) todo.push_back(r);
        }
        const bool dispatch_only = std::string_view(fam.name) == "libdispatch";
        if (dispatch_only) {
            std::set<std::string> d;
            for (auto& s : syms) if (s.rfind("_dispatch_", 0) == 0 || s.rfind("__dispatch_", 0) == 0) d.insert(s);
            syms = std::move(d);
            classes.clear();
        }
        Row r;
        r.family = fam.name;
        r.install_name = main->install_name;
        r.symbols = syms.size();
        r.classes = classes.size();
        for (const auto& h : libsystem::implemented_symbols()) {
            if (h.lib != fam.lib) continue;
            if (dispatch_only && std::string_view(h.name).rfind("_dispatch_", 0) != 0) continue;
            if (std::string_view(fam.name) == "libSystem" && std::string_view(h.name).rfind("_dispatch_", 0) == 0) continue;
            if (syms.count(h.name)) ++r.impl_symbols; else ++r.impl_extra_symbols;
        }
        for (const auto& c : ctx.objc->native_class_names(fam.name))
            if (classes.count(c) || std::string_view(fam.name) == "libobjc") ++r.impl_classes;
        rows.push_back(std::move(r));
    }

    if (o.json) {
        JsonWriter w;
        w.begin_object();
        w.field("tbd_files_parsed", static_cast<uint64_t>(parsed));
        w.field("tbd_files_failed", static_cast<uint64_t>(failed));
        w.field("libraries", static_cast<uint64_t>(by_install.size()));
        w.key("families");
        w.begin_array();
        for (auto& r : rows) {
            w.begin_object();
            w.field("family", r.family);
            w.field("install_name", r.install_name);
            w.field("exported_symbols", static_cast<uint64_t>(r.symbols));
            w.field("exported_classes", static_cast<uint64_t>(r.classes));
            w.field("implemented_symbols", static_cast<uint64_t>(r.impl_symbols));
            w.field("implemented_classes", static_cast<uint64_t>(r.impl_classes));
            w.field("implemented_not_in_sdk", static_cast<uint64_t>(r.impl_extra_symbols));
            w.end_object();
        }
        w.end_array();
        w.end_object();
        std::puts(w.str().c_str());
        return 0;
    }
    std::printf("CIDER - Darwin Executable Runtime\n\nSDK text stubs: %zu files parsed (%zu failed), %zu distinct libraries\n\n", parsed, failed, by_install.size());
    std::printf("%-15s %9s %9s %12s %12s  %s\n", "family", "symbols", "classes", "impl.symbols", "impl.classes", "install name");
    for (auto& r : rows)
        std::printf("%-15s %9zu %9zu %12zu %12zu  %s\n", r.family.c_str(), r.symbols, r.classes, r.impl_symbols, r.impl_classes, r.install_name.c_str());
    std::puts("\nExported by the SDK is not working in CIDER: 'impl.' counts host functions / classes that exist, not behavioural compatibility.");
    return 0;
}

}  // namespace cider::cli
