#include "commands.hpp"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "cider/analysis.hpp"
#include "cider/bundle.hpp"
#include "cider/dmg.hpp"
#include "cider/json.hpp"
#include "cider/log.hpp"
#include "cider/macho.hpp"
#include "cider/util.hpp"

namespace fs = std::filesystem;

namespace cider::cli {
namespace {

using analysis::DepClass;

struct Target {
    std::string display_path;
    std::optional<bundle::Info> bundle;
    fs::path exe_path;
    std::vector<uint8_t> bytes;
    macho::File file;
};

void banner() { std::puts("CIDER - Darwin Executable Runtime\n"); }

int fail(const Options& o, const std::string& msg, int code = 1) {
    log::error("{}", msg);
    if (o.json) {
        JsonWriter w;
        w.begin_object();
        w.field("tool", "cider");
        w.field("version", kVersion);
        w.field("ok", false);
        w.field("error", msg);
        w.end_object();
        std::puts(w.str().c_str());
    } else {
        std::fprintf(stderr, "error: %s\n", sanitize_utf8(msg).c_str());
    }
    return code;
}

macho::ParseOptions parse_options(const Options& o) {
    macho::ParseOptions po;
    po.parse_symbols = o.symbols;
    return po;
}

Result<Target> load_target(const Options& o) {
    Target t;
    t.display_path = o.positional.at(0);
    fs::path p = path_from_utf8(t.display_path);
    std::error_code ec;
    if (!fs::exists(p, ec)) return Error{ErrorCode::NotFound, "path does not exist: " + t.display_path};

    if (fs::is_directory(p, ec)) {
        auto b = bundle::inspect(p);
        if (!b.ok()) return b.error();
        t.bundle = std::move(b.value());
        if (!t.bundle->has_contents) return Error{ErrorCode::Unsupported, t.bundle->issues.empty() ? "not an application bundle" : t.bundle->issues[0]};
        if (!t.bundle->executable_found) {
            std::string why = t.bundle->issues.empty() ? "main executable not found" : t.bundle->issues.back();
            return Error{ErrorCode::NotFound, why};
        }
        t.exe_path = t.bundle->executable_path;
    } else {
        t.exe_path = p;
    }
    log::info("Loading {}", path_to_utf8(t.exe_path));
    auto bytes = read_file(t.exe_path, macho::Limits{}.max_file_size);
    if (!bytes.ok()) return bytes.error();
    t.bytes = std::move(bytes.value());
    auto f = macho::parse_file(ByteView(t.bytes), parse_options(o));
    if (!f.ok()) return f.error();
    t.file = std::move(f.value());
    for (auto& s : t.file.slices) {
        if (s.image) log::info("Architecture: {}", macho::cpu_name(s.cputype, s.cpusubtype));
    }
    return t;
}

std::string prot_string(int32_t p) {
    std::string s = "---";
    if (p & 1) s[0] = 'r';
    if (p & 2) s[1] = 'w';
    if (p & 4) s[2] = 'x';
    return s;
}

std::string archs_summary(const macho::File& f) {
    std::string s;
    for (auto& sl : f.slices) s += (s.empty() ? "" : ", ") + macho::cpu_name(sl.cputype, sl.cpusubtype);
    return s;
}

std::string platform_text(const macho::Image& img) {
    if (img.build_versions.empty()) return "unknown (no build version command)";
    const auto& b = img.build_versions[0];
    return std::format("{} (minimum {}, SDK {})", macho::platform_name(b.platform), version_string(b.minos), version_string(b.sdk));
}

std::string entry_text(const macho::Image& img) {
    if (!img.entry) return "Not detected";
    const auto& e = *img.entry;
    if (e.kind == macho::EntryPoint::Kind::Main)
        return std::format("Detected (LC_MAIN, file offset {:#x}, vmaddr {:#x} in {})", e.file_offset, e.vmaddr, e.segment);
    return std::format("Detected (LC_UNIXTHREAD, pc {:#x} in {})", e.vmaddr, e.segment);
}

std::string signature_text(const macho::CodeSignature& c) {
    std::string s = std::format("present ({} bytes at {:#x}", c.datasize, c.dataoff);
    if (c.has_code_directory) {
        s += c.adhoc ? ", ad-hoc" : ", signed";
        if (!c.identifier.empty()) s += ", identifier " + c.identifier;
        if (!c.team_id.empty()) s += ", team " + c.team_id;
        if (c.hardened_runtime) s += ", hardened runtime";
    }
    return s + "); parsed only, NOT verified";
}

void write_image_json(JsonWriter& w, const macho::Slice& sl, bool symbols) {
    w.begin_object();
    w.field("architecture", macho::cpu_name(sl.cputype, sl.cpusubtype));
    w.field("offset", sl.offset);
    w.field("size", sl.size);
    if (!sl.image) {
        w.field("valid", false);
        w.field("error", sl.error ? sl.error->message : std::string("unknown"));
        w.field("error_code", sl.error ? to_string(sl.error->code) : "unknown");
        w.end_object();
        return;
    }
    const auto& i = *sl.image;
    w.field("valid", true);
    w.field("file_type", macho::filetype_name(i.filetype));
    w.key("flags"); w.begin_array();
    for (auto& f : macho::flag_names(i.flags)) w.value(f);
    w.end_array();
    w.key("build_versions"); w.begin_array();
    for (auto& b : i.build_versions) {
        w.begin_object();
        w.field("platform", macho::platform_name(b.platform));
        w.field("minos", version_string(b.minos));
        w.field("sdk", version_string(b.sdk));
        w.field("legacy_command", b.legacy);
        w.end_object();
    }
    w.end_array();
    w.key("entry_point");
    if (i.entry) {
        w.begin_object();
        w.field("kind", i.entry->kind == macho::EntryPoint::Kind::Main ? "LC_MAIN" : "LC_UNIXTHREAD");
        w.field("file_offset", i.entry->file_offset);
        w.field("vmaddr", i.entry->vmaddr);
        w.field("segment", i.entry->segment);
        w.end_object();
    } else w.null();
    w.key("uuid"); if (i.uuid) w.value(*i.uuid); else w.null();
    w.key("dylinker"); if (!i.dylinker.empty()) w.value(i.dylinker); else w.null();
    w.key("id_dylib"); if (i.id_dylib) w.value(i.id_dylib->path); else w.null();
    w.key("segments"); w.begin_array();
    for (auto& s : i.segments) {
        w.begin_object();
        w.field("name", s.name);
        w.field("vmaddr", s.vmaddr);
        w.field("vmsize", s.vmsize);
        w.field("fileoff", s.fileoff);
        w.field("filesize", s.filesize);
        w.field("initprot", prot_string(s.initprot));
        w.field("maxprot", prot_string(s.maxprot));
        w.key("sections"); w.begin_array();
        for (auto& sec : s.sections) {
            w.begin_object();
            w.field("name", sec.name);
            w.field("addr", sec.addr);
            w.field("size", sec.size);
            w.field("offset", sec.offset);
            w.end_object();
        }
        w.end_array();
        w.end_object();
    }
    w.end_array();
    w.key("dylibs"); w.begin_array();
    for (auto& d : i.dylibs) {
        w.begin_object();
        w.field("path", d.path);
        w.field("kind", macho::dylib_kind_name(d.kind));
        w.field("current_version", version_string(d.current_version));
        w.field("compatibility_version", version_string(d.compat_version));
        w.end_object();
    }
    w.end_array();
    w.key("rpaths"); w.begin_array();
    for (auto& r : i.rpaths) w.value(r);
    w.end_array();
    w.key("code_signature");
    if (i.code_signature) {
        const auto& c = *i.code_signature;
        w.begin_object();
        w.field("offset", c.dataoff);
        w.field("size", c.datasize);
        w.field("parsed", c.blob_parsed);
        w.field("adhoc", c.adhoc);
        w.field("identifier", c.identifier);
        w.field("team_id", c.team_id);
        w.field("hardened_runtime", c.hardened_runtime);
        w.field("has_entitlements", c.has_entitlements);
        w.field("verified", false);
        w.end_object();
    } else w.null();
    w.field("encrypted", i.encrypted);
    w.key("symtab");
    if (i.symtab) { w.begin_object(); w.field("symbols", i.symtab->nsyms); w.field("string_table_size", i.symtab->strsize); w.end_object(); }
    else w.null();
    if (symbols) {
        w.key("symbols"); w.begin_array();
        for (auto& s : i.symbols) {
            w.begin_object();
            w.field("name", s.name);
            w.field("value", s.value);
            w.field("external", s.external);
            w.field("undefined", s.undefined);
            w.end_object();
        }
        w.end_array();
    }
    w.key("warnings"); w.begin_array();
    for (auto& s : i.warnings) w.value(s);
    w.end_array();
    w.end_object();
}

void write_deps_json(JsonWriter& w, const analysis::DependencyReport& r) {
    w.begin_object();
    w.field("images_scanned", static_cast<uint64_t>(r.images_scanned));
    w.key("dependencies"); w.begin_array();
    for (auto& d : r.deps) {
        w.begin_object();
        w.field("install_name", d.install_name);
        w.field("kind", macho::dylib_kind_name(d.kind));
        w.field("class", analysis::to_string(d.cls));
        w.field("direct", d.direct);
        w.field("required_by", d.required_by);
        w.field("resolved", d.resolved);
        w.end_object();
    }
    w.end_array();
    w.key("notes"); w.begin_array();
    for (auto& n : r.notes) w.value(n);
    w.end_array();
    w.end_object();
}

void write_compat_json(JsonWriter& w, const analysis::CompatReport& c) {
    w.begin_object();
    w.field("status", analysis::to_string(c.status));
    w.field("execution_confirmed", c.execution_confirmed);
    w.key("reasons"); w.begin_array();
    for (auto& r : c.reasons) w.value(r);
    w.end_array();
    w.end_object();
}

void write_bundle_json(JsonWriter& w, const bundle::Info& b) {
    w.begin_object();
    w.field("name", b.name);
    w.field("identifier", b.identifier);
    w.field("executable", b.executable_name);
    w.field("version", b.version);
    w.field("short_version", b.short_version);
    w.field("minimum_system_version", b.min_system_version);
    w.field("has_resources", b.has_resources);
    w.field("has_code_signature_dir", b.has_code_signature_dir);
    w.key("frameworks"); w.begin_array();
    for (auto& f : b.frameworks) {
        w.begin_object();
        w.field("name", f.name);
        w.field("binary_found", f.binary.has_value());
        w.key("architectures"); w.begin_array();
        for (auto& a : f.architectures) w.value(a);
        w.end_array();
        w.end_object();
    }
    w.end_array();
    w.key("issues"); w.begin_array();
    for (auto& s : b.issues) w.value(s);
    w.end_array();
    w.end_object();
}

analysis::DependencyReport deps_for(const Target& t, const macho::Image& img) {
    std::optional<fs::path> root;
    if (t.bundle) root = t.bundle->root;
    return analysis::analyze_dependencies(img, t.exe_path, root);
}

const macho::Image* primary_image(const Target& t) {
    if (auto* s = analysis::select_slice(t.file)) return &*s->image;
    for (auto& s : t.file.slices)
        if (s.image) return &*s.image;
    return nullptr;
}

void print_deps_text(const analysis::DependencyReport& r, const fs::path* root = nullptr) {
    if (r.deps.empty()) { std::puts("  (none)"); return; }
    std::set<std::string> printed;
    size_t hidden = 0;
    for (auto& d : r.deps) {
        // Transitive duplicates (same install name already listed) are summarised; JSON keeps everything.
        if (!d.direct && !printed.insert(d.install_name).second) { ++hidden; continue; }
        printed.insert(d.install_name);
        std::string extra;
        switch (d.cls) {
            case DepClass::System: extra = "system library, not provided by CIDER yet"; break;
            case DepClass::Bundled: {
                std::string shown = d.resolved;
                if (root) {
                    auto rel = path_from_utf8(d.resolved).lexically_relative(*root);
                    if (!rel.empty()) shown = path_to_utf8(rel);
                }
                extra = "bundled: " + shown;
                break;
            }
            case DepClass::OutsideBundle: extra = "resolved OUTSIDE the bundle: " + d.resolved; break;
            case DepClass::Missing: extra = "NOT FOUND"; break;
            case DepClass::External: extra = "absolute path outside the system directories"; break;
        }
        std::string weak = d.kind == macho::DylibKind::Load ? "" : std::string(" [") + macho::dylib_kind_name(d.kind) + "]";
        std::printf("- %s%s -- %s%s\n", sanitize_utf8(d.install_name).c_str(), weak.c_str(), sanitize_utf8(extra).c_str(),
                    d.direct ? "" : (" (required by " + sanitize_utf8(d.required_by) + ")").c_str());
    }
    if (hidden) std::printf("  (%zu further transitive entries already listed; see --json)\n", hidden);
    for (auto& n : r.notes) std::printf("  note: %s\n", sanitize_utf8(n).c_str());
}

void print_compat_text(const analysis::CompatReport& c) {
    std::printf("Compatibility: %s\n", analysis::to_string(c.status));
    std::puts("- Static analysis completed");
    std::puts("- Execution support not yet confirmed");
    for (auto& r : c.reasons) std::printf("- %s\n", sanitize_utf8(r).c_str());
}

}  // namespace

static int inspect_macho_target(const Options& o) {
    auto t = load_target(o);
    if (!t.ok()) return fail(o, t.error().message);
    const Target& tg = t.value();
    const macho::Image* img = primary_image(tg);
    analysis::DependencyReport deps;
    if (img) deps = deps_for(tg, *img);
    auto compat = analysis::assess(tg.file, img ? &deps : nullptr, tg.bundle ? &*tg.bundle : nullptr);
    bool any_valid = img != nullptr;

    if (o.json) {
        JsonWriter w;
        w.begin_object();
        w.field("tool", "cider");
        w.field("version", kVersion);
        w.field("ok", any_valid);
        w.field("path", tg.display_path);
        w.field("kind", tg.bundle ? "app-bundle" : "mach-o");
        w.key("bundle"); if (tg.bundle) write_bundle_json(w, *tg.bundle); else w.null();
        w.field("format", tg.file.fat ? (tg.file.fat64 ? "FAT64" : "FAT") : "Mach-O 64-bit");
        w.key("slices"); w.begin_array();
        for (auto& s : tg.file.slices) write_image_json(w, s, o.symbols);
        w.end_array();
        w.key("file_warnings"); w.begin_array();
        for (auto& s : tg.file.warnings) w.value(s);
        w.end_array();
        w.key("dependencies"); write_deps_json(w, deps);
        w.key("compatibility"); write_compat_json(w, compat);
        w.end_object();
        std::puts(w.str().c_str());
        return any_valid ? 0 : 1;
    }

    banner();
    if (tg.bundle) {
        std::printf("Application: %s\n", sanitize_utf8(tg.bundle->name).c_str());
        if (!tg.bundle->identifier.empty()) std::printf("Bundle Identifier: %s\n", sanitize_utf8(tg.bundle->identifier).c_str());
        if (!tg.bundle->short_version.empty() || !tg.bundle->version.empty())
            std::printf("Version: %s (build %s)\n", sanitize_utf8(tg.bundle->short_version).c_str(), sanitize_utf8(tg.bundle->version).c_str());
        std::printf("Executable: Contents/MacOS/%s\n", sanitize_utf8(tg.bundle->executable_name).c_str());
        if (!tg.bundle->min_system_version.empty()) std::printf("Minimum macOS (Info.plist): %s\n", sanitize_utf8(tg.bundle->min_system_version).c_str());
    } else {
        std::printf("File: %s\n", sanitize_utf8(path_to_utf8(tg.exe_path.filename())).c_str());
    }
    std::printf("Format: %s\n", tg.file.fat ? (tg.file.fat64 ? "Universal (FAT64) containing Mach-O 64-bit" : "Universal (FAT) containing Mach-O 64-bit") : "Mach-O 64-bit");
    std::printf("Architecture: %s\n", archs_summary(tg.file).c_str());
    for (auto& w : tg.file.warnings) std::printf("Warning: %s\n", sanitize_utf8(w).c_str());

    for (auto& s : tg.file.slices) {
        if (tg.file.fat) std::printf("\n== Slice %s (offset %#llx, size %llu) ==\n", macho::cpu_name(s.cputype, s.cpusubtype).c_str(),
                                     static_cast<unsigned long long>(s.offset), static_cast<unsigned long long>(s.size));
        if (!s.image) {
            std::printf("Invalid or unsupported slice: %s\n", sanitize_utf8(s.error ? s.error->message : "unknown").c_str());
            continue;
        }
        const auto& i = *s.image;
        std::printf("Type: %s\n", macho::filetype_name(i.filetype));
        std::printf("Platform: %s\n", platform_text(i).c_str());
        std::printf("Entry Point: %s\n", entry_text(i).c_str());
        if (i.uuid) std::printf("UUID: %s\n", i.uuid->c_str());
        if (i.id_dylib) std::printf("Install name: %s\n", sanitize_utf8(i.id_dylib->path).c_str());
        auto flags = macho::flag_names(i.flags);
        if (!flags.empty()) {
            std::string f;
            for (auto& x : flags) f += (f.empty() ? "" : " ") + x;
            std::printf("Flags: %s\n", f.c_str());
        }
        std::printf("Segments:\n");
        for (auto& sg : i.segments) {
            std::printf("  %-12s vm %#llx +%#llx  file %#llx +%#llx  %s/%s\n", sanitize_utf8(sg.name).c_str(),
                        static_cast<unsigned long long>(sg.vmaddr), static_cast<unsigned long long>(sg.vmsize),
                        static_cast<unsigned long long>(sg.fileoff), static_cast<unsigned long long>(sg.filesize),
                        prot_string(sg.initprot).c_str(), prot_string(sg.maxprot).c_str());
            for (auto& sec : sg.sections)
                std::printf("    %-16s addr %#llx size %#llx\n", sanitize_utf8(sec.name).c_str(),
                            static_cast<unsigned long long>(sec.addr), static_cast<unsigned long long>(sec.size));
        }
        if (!i.rpaths.empty()) {
            std::puts("Runpath search paths:");
            for (auto& r : i.rpaths) std::printf("- %s\n", sanitize_utf8(r).c_str());
        }
        if (i.code_signature) std::printf("Code signature: %s\n", sanitize_utf8(signature_text(*i.code_signature)).c_str());
        else std::puts("Code signature: none");
        if (i.encrypted) std::puts("Encryption: image is ENCRYPTED");
        if (i.symtab) std::printf("Symbols: %u (string table %u bytes)\n", i.symtab->nsyms, i.symtab->strsize);
        if (o.symbols)
            for (auto& sym : i.symbols) std::printf("  %016llx %c %s\n", static_cast<unsigned long long>(sym.value), sym.undefined ? 'U' : (sym.external ? 'T' : 't'), sanitize_utf8(sym.name).c_str());
        for (auto& w : i.warnings) std::printf("Warning: %s\n", sanitize_utf8(w).c_str());
    }

    if (tg.bundle) {
        std::printf("\nBundle layout: Info.plist %s, MacOS %s, Resources %s, Frameworks %s, _CodeSignature %s\n",
                    tg.bundle->has_info_plist ? "yes" : "no", tg.bundle->has_macos_dir ? "yes" : "no", tg.bundle->has_resources ? "yes" : "no",
                    tg.bundle->has_frameworks ? "yes" : "no", tg.bundle->has_code_signature_dir ? "yes" : "no");
        for (auto& f : tg.bundle->frameworks) {
            std::string a;
            for (auto& x : f.architectures) a += (a.empty() ? "" : ", ") + x;
            std::printf("- embedded %s [%s]\n", sanitize_utf8(f.name).c_str(), f.binary ? (a.empty() ? "binary unreadable" : a.c_str()) : "binary not found");
        }
        for (auto& i : tg.bundle->issues) std::printf("Bundle issue: %s\n", sanitize_utf8(i).c_str());
    }
    std::puts("\nDependencies:");
    print_deps_text(deps, tg.bundle ? &tg.bundle->root : nullptr);
    std::puts("");
    print_compat_text(compat);
    return any_valid ? 0 : 1;
}

int cmd_dependencies(const Options& o) {
    if (o.positional.empty()) return fail(o, "usage: cider dependencies <program|App.app> [--json]", 2);
    auto t = load_target(o);
    if (!t.ok()) return fail(o, t.error().message);
    const macho::Image* img = primary_image(t.value());
    if (!img) return fail(o, "no valid Mach-O slice to analyse");
    auto deps = deps_for(t.value(), *img);
    if (o.json) {
        JsonWriter w;
        write_deps_json(w, deps);
        std::puts(w.str().c_str());
    } else {
        banner();
        std::puts("Dependencies:");
        print_deps_text(deps);
    }
    return 0;
}

int cmd_compatibility(const Options& o) {
    if (o.positional.empty()) return fail(o, "usage: cider compatibility <program|App.app> [--json]", 2);
    auto t = load_target(o);
    if (!t.ok()) return fail(o, t.error().message);
    const macho::Image* img = primary_image(t.value());
    analysis::DependencyReport deps;
    if (img) deps = deps_for(t.value(), *img);
    auto c = analysis::assess(t.value().file, img ? &deps : nullptr, t.value().bundle ? &*t.value().bundle : nullptr);
    if (o.json) {
        JsonWriter w;
        write_compat_json(w, c);
        std::puts(w.str().c_str());
    } else {
        banner();
        print_compat_text(c);
    }
    return c.status == analysis::CompatStatus::NotCompatible ? 1 : 0;
}

int cmd_logs(const Options& o) {
    const char* env = std::getenv("CIDER_LOG_FILE");
    std::string path = !o.positional.empty() ? o.positional[0] : (env ? env : "");
    if (path.empty()) return fail(o, "no log file configured: pass a path, set CIDER_LOG_FILE, or run other commands with --log-file <path>", 2);
    std::ifstream f(path_from_utf8(path));
    if (!f) return fail(o, "cannot open log file: " + path);
    std::deque<std::string> tail;
    for (std::string line; std::getline(f, line);) {
        tail.push_back(line);
        if (tail.size() > 200) tail.pop_front();
    }
    for (auto& l : tail) std::printf("%s\n", sanitize_utf8(l).c_str());
    return 0;
}

int cmd_doctor(const Options& o) {
    struct Check { std::string name, status, detail; };
    std::vector<Check> checks;
    checks.push_back({"version", "ok", kVersion});
#if defined(_MSC_VER)
    checks.push_back({"compiler", "ok", std::format("MSVC {}", _MSC_VER)});
#elif defined(__clang__)
    checks.push_back({"compiler", "ok", "clang " __clang_version__});
#else
    checks.push_back({"compiler", "ok", "gcc-compatible"});
#endif
#if defined(_M_X64) || defined(__x86_64__)
    checks.push_back({"host architecture", "ok", "x86_64 (the only architecture targeted in 0.1)"});
#elif defined(_M_ARM64) || defined(__aarch64__)
    checks.push_back({"host architecture", "warn", "ARM64: parser works, runtime is not planned for this host yet"});
#else
    checks.push_back({"host architecture", "warn", "unknown"});
#endif
#ifdef _WIN32
    {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW vi{};
        vi.dwOSVersionInfoSize = sizeof vi;
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        auto fn = nt ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion"))) : nullptr;
        if (fn && fn(&vi) == 0)
            checks.push_back({"operating system", vi.dwBuildNumber >= 10240 ? "ok" : "warn",
                              std::format("Windows {}.{} build {}", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber)});
        else checks.push_back({"operating system", "warn", "Windows (version unknown)"});
    }
#else
    checks.push_back({"operating system", "warn", "not Windows: CIDER targets Windows 10/11"});
#endif
    checks.push_back({"mach-o parser", "ok", "available (static analysis)"});
    checks.push_back({"plist reader", "ok", "available (XML + binary)"});
    const char* lf = std::getenv("CIDER_LOG_FILE");
    checks.push_back({"log file", "info", lf ? lf : "not configured (use --log-file or CIDER_LOG_FILE)"});
    checks.push_back({"loader / dyld / darwin runtime", "missing", "not implemented in 0.1: 'cider run' unavailable"});

    if (o.json) {
        JsonWriter w;
        w.begin_object();
        w.field("tool", "cider");
        w.field("version", kVersion);
        w.key("checks"); w.begin_array();
        for (auto& c : checks) { w.begin_object(); w.field("name", c.name); w.field("status", c.status); w.field("detail", c.detail); w.end_object(); }
        w.end_array();
        w.end_object();
        std::puts(w.str().c_str());
    } else {
        banner();
        for (auto& c : checks) std::printf("[%-7s] %-32s %s\n", c.status.c_str(), c.name.c_str(), c.detail.c_str());
    }
    return 0;
}

#include "commands_dmg.inc"

}  // namespace cider::cli
