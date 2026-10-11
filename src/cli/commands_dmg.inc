// Included at the end of commands.cpp (inside namespace orvyn::cli): disk-image commands.
// They share the report helpers defined in commands.cpp's anonymous namespace.

namespace {

dmg::ExtractOptions extract_options(const Options& o) {
    dmg::ExtractOptions eo;
    if (o.max_size_mib) eo.limits.max_total_bytes = o.max_size_mib << 20;
    if (!o.temp_dir.empty()) eo.temp_base = path_from_utf8(o.temp_dir);
    else if (const char* t = getenv_compat("ORVYN_TEMP_DIR", "CIDER_TEMP_DIR"); t && *t) eo.temp_base = path_from_utf8(t);
    return eo;
}

Result<dmg::Backend> backend_for(const Options& o) {
    return dmg::locate_7zip(o.seven_zip.empty() ? fs::path{} : fs::absolute(path_from_utf8(o.seven_zip)));
}

std::string join(const std::vector<std::string>& v, const char* sep = ", ") {
    std::string s;
    for (auto& x : v) s += (s.empty() ? "" : sep) + x;
    return s;
}

std::string compression_list(const dmg::Info& i) {
    std::string s;
    for (auto c : i.compressions) s += (s.empty() ? "" : ", ") + std::string(dmg::to_string(c));
    return s.empty() ? "none (no data chunks)" : s;
}

void print_dmg_text(const dmg::Info& i, const std::string& name) {
    std::printf("Disk image: %s\n", sanitize_utf8(name).c_str());
    std::printf("Size: %llu bytes\n", static_cast<unsigned long long>(i.file_size));
    std::printf("Container: %s", dmg::to_string(i.kind));
    if (i.udif)
        std::printf(" (version %u, %s, %llu sectors)", i.udif->version, i.udif->flattened() ? "flattened" : "not flattened",
                    static_cast<unsigned long long>(i.udif->sector_count));
    if (!i.raw_description.empty()) std::printf(" (%s)", i.raw_description.c_str());
    std::puts("");
    if (i.kind == dmg::Kind::Udif) {
        std::printf("Compression: %s\n", compression_list(i).c_str());
        std::printf("Filesystem: %s\n", i.filesystems.empty() ? "not determined from the image structure" : join(i.filesystems).c_str());
        std::printf("Partitions: %zu\n", i.partitions.size());
        for (auto& p : i.partitions) {
            std::string c;
            for (auto& [k, st] : p.compression) c += (c.empty() ? "" : ", ") + std::string(dmg::to_string(k)) + " x" + std::to_string(st.chunks);
            std::string fsn = p.filesystem.empty() ? "" : "; " + p.filesystem + (p.filesystem_verified ? " verified" : " inferred");
            std::printf("- %s [%llu sectors; %s%s]\n", sanitize_utf8(p.name).c_str(), static_cast<unsigned long long>(p.sector_count), c.c_str(), fsn.c_str());
        }
    } else if (i.kind == dmg::Kind::RawImage) {
        std::printf("Filesystem: %s\n", i.filesystems.empty() ? "n/a" : join(i.filesystems).c_str());
    }
    for (auto& w : i.warnings) std::printf("Warning: %s\n", sanitize_utf8(w).c_str());
}

void write_dmg_json(JsonWriter& w, const dmg::Info& i) {
    w.begin_object();
    w.field("kind", dmg::to_string(i.kind));
    w.field("file_size", i.file_size);
    w.key("udif");
    if (i.udif) {
        w.begin_object();
        w.field("version", i.udif->version);
        w.field("flattened", i.udif->flattened());
        w.field("sector_count", i.udif->sector_count);
        w.field("data_fork_offset", i.udif->data_fork_offset);
        w.field("data_fork_length", i.udif->data_fork_length);
        w.field("xml_offset", i.udif->xml_offset);
        w.field("xml_length", i.udif->xml_length);
        w.end_object();
    } else w.null();
    w.field("raw_description", i.raw_description);
    w.key("compressions"); w.begin_array();
    for (auto c : i.compressions) w.value(dmg::to_string(c));
    w.end_array();
    w.key("filesystems"); w.begin_array();
    for (auto& f : i.filesystems) w.value(f);
    w.end_array();
    w.key("partitions"); w.begin_array();
    for (auto& p : i.partitions) {
        w.begin_object();
        w.field("name", p.name);
        w.field("declared_type", p.declared_type);
        w.field("sector_count", p.sector_count);
        w.field("filesystem", p.filesystem);
        w.field("filesystem_verified", p.filesystem_verified);
        w.key("chunks"); w.begin_array();
        for (auto& [k, st] : p.compression) {
            w.begin_object();
            w.field("compression", dmg::to_string(k));
            w.field("chunks", st.chunks);
            w.field("sectors", st.sectors);
            w.end_object();
        }
        w.end_array();
        w.end_object();
    }
    w.end_array();
    w.key("warnings"); w.begin_array();
    for (auto& s : i.warnings) w.value(s);
    w.end_array();
    w.end_object();
}

// Unsupported kinds -> user-facing explanation; empty when the kind can be processed.
std::string kind_problem(const dmg::Info& i) {
    switch (i.kind) {
        case dmg::Kind::Encrypted: return "encrypted disk image (password protected): not supported. Decrypt it first (e.g. with hdiutil on macOS).";
        case dmg::Kind::Sparse: return "sparse disk image: not supported. Convert it to a compressed image first (hdiutil convert).";
        case dmg::Kind::Unknown: return "not a recognised disk image, Mach-O file or application bundle";
        default: return {};
    }
}

struct AppResult {
    std::string rel;
    std::string error;
    std::optional<Target> target;
    analysis::DependencyReport deps;
    analysis::CompatReport compat;
};

AppResult analyze_app(const fs::path& path, const std::string& rel) {
    AppResult r;
    r.rel = rel;
    Options o;
    o.positional = {path_to_utf8(path)};
    auto t = load_target(o);
    if (!t.ok()) { r.error = t.error().message; return r; }
    r.target = std::move(t.value());
    const macho::Image* img = primary_image(*r.target);
    if (img) r.deps = deps_for(*r.target, *img);
    r.compat = analysis::assess(r.target->file, img ? &r.deps : nullptr, r.target->bundle ? &*r.target->bundle : nullptr);
    return r;
}

void print_app_text(const AppResult& r) {
    std::printf("\n== %s ==\n", sanitize_utf8(r.rel).c_str());
    if (!r.target) { std::printf("Cannot analyse: %s\n", sanitize_utf8(r.error).c_str()); return; }
    const Target& t = *r.target;
    if (t.bundle) {
        std::printf("Application: %s", sanitize_utf8(t.bundle->name).c_str());
        if (!t.bundle->short_version.empty()) std::printf(" %s", sanitize_utf8(t.bundle->short_version).c_str());
        std::puts("");
        if (!t.bundle->identifier.empty()) std::printf("Bundle Identifier: %s\n", sanitize_utf8(t.bundle->identifier).c_str());
        std::printf("Executable: Contents/MacOS/%s\n", sanitize_utf8(t.bundle->executable_name).c_str());
    }
    std::printf("Format: %s\nArchitecture: %s\n", t.file.fat ? "Universal (FAT)" : "Mach-O 64-bit", archs_summary(t.file).c_str());
    if (const macho::Image* i = primary_image(t))
        std::printf("Platform: %s\nEntry Point: %s\n", platform_text(*i).c_str(), entry_text(*i).c_str());
    if (t.bundle) {
        if (t.bundle->frameworks.empty()) {
            std::puts("Frameworks: none embedded");
        } else {
            std::puts("Frameworks:");
            for (auto& f : t.bundle->frameworks) {
                std::string a;
                for (auto& x : f.architectures) a += (a.empty() ? "" : ", ") + x;
                std::printf("- %s [%s]\n", sanitize_utf8(f.name).c_str(), f.binary ? (a.empty() ? "binary unreadable" : a.c_str()) : "binary not found");
            }
        }
        for (auto& i : t.bundle->issues) std::printf("Bundle issue: %s\n", sanitize_utf8(i).c_str());
    }
    std::puts("Dependencies:");
    print_deps_text(r.deps, t.bundle ? &t.bundle->root : nullptr);
    print_compat_text(r.compat);
}

void write_app_json(JsonWriter& w, const AppResult& r) {
    w.begin_object();
    w.field("path", r.rel);
    w.field("ok", r.target.has_value());
    if (!r.target) { w.field("error", r.error); w.end_object(); return; }
    const Target& t = *r.target;
    w.key("bundle"); if (t.bundle) write_bundle_json(w, *t.bundle); else w.null();
    w.key("slices"); w.begin_array();
    for (auto& s : t.file.slices) write_image_json(w, s, false);
    w.end_array();
    w.key("dependencies"); write_deps_json(w, r.deps);
    w.key("compatibility"); write_compat_json(w, r.compat);
    w.end_object();
}

void emit_json(const JsonWriter& w) { std::puts(w.str().c_str()); }

struct DmgContext {
    fs::path image;
    dmg::Info info;
};

Result<DmgContext> open_dmg(const Options& o) {
    DmgContext c;
    c.image = path_from_utf8(o.positional.at(0));
    std::error_code ec;
    if (!fs::exists(c.image, ec)) return Error{ErrorCode::NotFound, "path does not exist: " + o.positional.at(0)};
    if (fs::is_directory(c.image, ec)) return Error{ErrorCode::InvalidArgument, "expected a disk image file, got a directory: " + o.positional.at(0)};
    auto info = dmg::inspect_file(c.image);
    if (!info.ok()) return info.error();
    c.info = std::move(info.value());
    return c;
}

int inspect_dmg(const Options& o, DmgContext& c) {
    const std::string name = path_to_utf8(c.image.filename());
    const std::string problem = kind_problem(c.info);

    JsonWriter w;
    if (o.json) {
        w.begin_object();
        w.field("tool", "orvyn");
        w.field("version", kVersion);
        w.field("kind", "disk-image");
        w.field("path", o.positional[0]);
        w.key("image"); write_dmg_json(w, c.info);
    } else {
        banner();
        print_dmg_text(c.info, name);
    }
    auto finish = [&](int rc, const std::string& status, const std::string& message) {
        if (o.json) {
            w.field("status", status);
            w.field("message", message);
            w.end_object();
            emit_json(w);
        } else if (!message.empty()) {
            std::printf("\n%s\n", sanitize_utf8(message).c_str());
        }
        return rc;
    };
    if (!problem.empty()) return finish(1, "unsupported", problem);

    auto be = backend_for(o);
    if (!be.ok()) {
        if (o.json) { w.key("backend"); w.begin_object(); w.field("available", false); w.field("reason", be.error().message); w.end_object(); }
        else std::printf("Backend: 7-Zip unavailable (%s)\n", sanitize_utf8(be.error().message).c_str());
        return finish(3, "partial", "Application analysis skipped: extracting the image requires 7-Zip. The structure above was read directly from the file.");
    }
    if (o.json) { w.key("backend"); w.begin_object(); w.field("available", true); w.field("path", path_to_utf8(be.value().exe)); w.field("banner", be.value().banner); w.end_object(); }
    else std::printf("Backend: %s\n", sanitize_utf8(be.value().banner).c_str());

    auto eo = extract_options(o);
    dmg::TempDir tmp(eo.temp_base);
    if (!tmp.ok()) return finish(1, "error", "cannot create a private temporary directory");
    auto rep = dmg::extract_image(be.value(), c.image, tmp.path() / "content", eo);
    if (!rep.ok()) {
        bool clean = tmp.remove();
        if (o.json) w.field("cleanup", clean);
        return finish(1, "error", "Extraction failed: " + rep.error().message);
    }

    if (o.json) {
        w.key("extraction"); w.begin_object();
        w.field("partitions", static_cast<uint64_t>(rep.value().partitions.size()));
        w.field("files", rep.value().total_files);
        w.field("bytes", rep.value().total_bytes);
        w.key("warnings"); w.begin_array();
        for (auto& s : rep.value().warnings) w.value(s);
        w.end_array();
        w.end_object();
    } else {
        std::printf("Extracted (temporary): %zu partition(s), %llu file(s), %llu bytes\n", rep.value().partitions.size(),
                    static_cast<unsigned long long>(rep.value().total_files), static_cast<unsigned long long>(rep.value().total_bytes));
        for (auto& s : rep.value().warnings) std::printf("Warning: %s\n", sanitize_utf8(s).c_str());
        std::printf("Applications found: %zu\n", rep.value().apps.size());
    }

    if (o.json) { w.key("apps"); w.begin_array(); }
    for (auto& rel : rep.value().apps) {
        AppResult r = analyze_app(rep.value().output / path_from_utf8(rel), rel);
        if (o.json) write_app_json(w, r); else print_app_text(r);
    }
    if (o.json) w.end_array();

    bool clean = tmp.remove();
    log::info("Temporary files {}", clean ? "removed" : "NOT fully removed");
    if (o.json) w.field("cleanup", clean);
    return finish(0, "ok", rep.value().apps.empty() ? "No .app bundles were found in the image." : "");
}

}  // namespace

int cmd_inspect(const Options& o) {
    if (o.positional.empty()) return fail(o, "usage: orvyn inspect <program|App.app|image.dmg> [--json] [--symbols]", 2);
    fs::path p = path_from_utf8(o.positional[0]);
    std::error_code ec;
    if (!fs::exists(p, ec)) return fail(o, "path does not exist: " + o.positional[0]);
    if (fs::is_directory(p, ec)) return inspect_macho_target(o);

    // Detection is by content, never by extension.
    uint8_t magic[8] = {};
    {
        std::ifstream f(p, std::ios::binary);
        f.read(reinterpret_cast<char*>(magic), sizeof magic);
    }
    if (macho::identify(ByteView(magic, sizeof magic)) != macho::Format::Unknown) return inspect_macho_target(o);

    auto ctx = open_dmg(o);
    if (!ctx.ok()) return fail(o, ctx.error().message);
    if (ctx.value().info.kind == dmg::Kind::Unknown)
        return fail(o, "unrecognised file: not a Mach-O/FAT binary, an application bundle or a disk image");
    return inspect_dmg(o, ctx.value());
}

int cmd_dmg_info(const Options& o) {
    if (o.positional.empty()) return fail(o, "usage: orvyn dmg-info <image.dmg> [--json] [--7z <path>]", 2);
    auto ctx = open_dmg(o);
    if (!ctx.ok()) return fail(o, ctx.error().message);
    const dmg::Info& info = ctx.value().info;
    const std::string problem = kind_problem(info);

    auto be = backend_for(o);
    std::optional<dmg::Listing> listing;
    std::string open_error;
    if (be.ok() && problem.empty()) {
        auto l = dmg::list_archive(be.value(), ctx.value().image);
        if (l.ok()) listing = std::move(l.value());
        else open_error = l.error().message;
    }

    if (o.json) {
        JsonWriter w;
        w.begin_object();
        w.field("tool", "orvyn");
        w.field("version", kVersion);
        w.field("path", o.positional[0]);
        w.key("image"); write_dmg_json(w, info);
        w.field("supported", problem.empty());
        w.field("problem", problem);
        w.key("backend"); w.begin_object();
        w.field("name", "7-Zip");
        w.field("available", be.ok());
        if (be.ok()) { w.field("path", path_to_utf8(be.value().exe)); w.field("banner", be.value().banner); }
        else w.field("reason", be.error().message);
        w.field("can_open_image", listing.has_value());
        if (listing) { w.field("archive_type", listing->type); w.field("entries", static_cast<uint64_t>(listing->items.size())); w.field("listed_bytes", listing->total_size); }
        if (!open_error.empty()) w.field("open_error", open_error);
        w.end_object();
        w.end_object();
        emit_json(w);
    } else {
        banner();
        print_dmg_text(info, path_to_utf8(ctx.value().image.filename()));
        if (!problem.empty()) std::printf("\nStatus: %s\n", sanitize_utf8(problem).c_str());
        std::puts("\nBackend (7-Zip, optional, used only for extraction):");
        if (!be.ok()) {
            std::printf("- not available: %s\n", sanitize_utf8(be.error().message).c_str());
        } else {
            std::printf("- %s\n- path: %s\n", sanitize_utf8(be.value().banner).c_str(), sanitize_utf8(path_to_utf8(be.value().exe)).c_str());
            if (listing)
                std::printf("- can open this image: yes (7-Zip type %s, %zu entr%s listed)\n", listing->type.c_str(), listing->items.size(),
                            listing->items.size() == 1 ? "y" : "ies");
            else if (!problem.empty()) std::puts("- can open this image: not attempted");
            else std::printf("- can open this image: NO (%s)\n", sanitize_utf8(open_error).c_str());
        }
    }
    return problem.empty() ? 0 : 1;
}

int cmd_extract(const Options& o) {
    if (o.positional.empty() || o.output.empty())
        return fail(o, "usage: orvyn extract <image.dmg> --output <directory> [--7z <path>] [--max-size <MiB>] [--json]", 2);
    auto ctx = open_dmg(o);
    if (!ctx.ok()) return fail(o, ctx.error().message);
    if (std::string pr = kind_problem(ctx.value().info); !pr.empty()) return fail(o, pr);
    auto be = backend_for(o);
    if (!be.ok()) return fail(o, "cannot extract without 7-Zip: " + be.error().message);

    fs::path out = fs::absolute(path_from_utf8(o.output));
    auto rep = dmg::extract_image(be.value(), ctx.value().image, out, extract_options(o));
    if (!rep.ok()) return fail(o, "extraction failed: " + rep.error().message);
    const auto& r = rep.value();
    if (o.json) {
        JsonWriter w;
        w.begin_object();
        w.field("tool", "orvyn");
        w.field("version", kVersion);
        w.field("ok", true);
        w.field("output", path_to_utf8(r.output));
        w.field("files", r.total_files);
        w.field("bytes", r.total_bytes);
        w.key("partitions"); w.begin_array();
        for (auto& p : r.partitions) { w.begin_object(); w.field("directory", p.name); w.field("type", p.type); w.field("files", p.files); w.field("bytes", p.bytes); w.end_object(); }
        w.end_array();
        w.key("apps"); w.begin_array();
        for (auto& a : r.apps) w.value(a);
        w.end_array();
        w.key("warnings"); w.begin_array();
        for (auto& s : r.warnings) w.value(s);
        w.end_array();
        w.end_object();
        emit_json(w);
    } else {
        banner();
        std::printf("Extracted to: %s\n", sanitize_utf8(path_to_utf8(r.output)).c_str());
        for (auto& p : r.partitions)
            std::printf("- %s (%s): %llu file(s), %llu bytes\n", sanitize_utf8(p.name).c_str(), p.type.c_str(), static_cast<unsigned long long>(p.files), static_cast<unsigned long long>(p.bytes));
        for (auto& s : r.warnings) std::printf("Warning: %s\n", sanitize_utf8(s).c_str());
        std::printf("Applications found: %zu\n", r.apps.size());
        for (auto& a : r.apps) std::printf("- %s\n", sanitize_utf8(a).c_str());
        std::puts("\nNothing was executed. Analyse an application with: orvyn inspect <path-to-.app>");
    }
    return 0;
}
