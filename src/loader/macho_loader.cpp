#include <format>

#include "orvyn/loader.hpp"
#include "orvyn/log.hpp"

namespace orvyn::loader {
namespace {

LoadResult fail(LoadFailure k, std::string msg, LoadReport rep = {}) {
    LoadResult r;
    r.report = rep;
    r.error = LoadError{k, std::move(msg), std::move(rep)};
    return r;
}

LoadFailure kind_of(const ValidationReport& v) {
    bool missing = false, unsupported = false;
    for (auto& i : v.issues) {
        missing |= i.kind == Issue::Kind::MissingDependency;
        unsupported |= i.kind == Issue::Kind::Unsupported;
    }
    if (missing && !unsupported) return LoadFailure::MissingDependency;
    if (unsupported) return LoadFailure::Unsupported;
    return LoadFailure::Incompatible;
}

}  // namespace

LoadResult load_macho(ByteView file, dyld::SymbolProvider& provider, const ValidatorOptions& opts) {
    // 1. static analysis (parser from ORVYN 0.1)
    macho::ParseOptions po;
    auto parsed = macho::parse_file(file, po);
    if (!parsed.ok()) return fail(LoadFailure::Incompatible, "not a valid Mach-O file: " + parsed.error().message);
    const macho::Slice* slice = parsed.value().find_slice(macho::kCpuX86_64);
    if (!slice) {
        std::string have;
        for (auto& s : parsed.value().slices) have += (have.empty() ? "" : ", ") + macho::cpu_name(s.cputype, s.cpusubtype);
        return fail(LoadFailure::Incompatible, "no x86_64 slice (found: " + have + ")");
    }
    if (!slice->image) return fail(LoadFailure::Incompatible, "x86_64 slice failed validation: " + (slice->error ? slice->error->message : std::string("unknown")));
    const macho::Image& img = *slice->image;
    auto bytes = file.slice(slice->offset, slice->size);
    if (!bytes.ok()) return fail(LoadFailure::Internal, "slice outside the file");
    log::info(log::Category::Loader, "Mach-O x86_64 detected ({} segment(s), {} load command(s))", img.segments.size(), img.ncmds);

    // 2. validation before execution
    LoadReport report;
    ValidationReport v = validate_for_execution(img, opts);
    report.required_libs = v.required_libs;
    if (!v.ok()) {
        std::string msg;
        for (auto& i : v.issues) msg += (msg.empty() ? "" : "; ") + i.message;
        return fail(kind_of(v), msg, report);
    }
    auto layout = plan_layout(img, opts);
    if (!layout.ok()) return fail(LoadFailure::Incompatible, layout.error().message, report);

    // 3. fixups and symbols (decided before any memory is reserved)
    auto fixups = dyld::decode_fixups(img, bytes.value());
    if (!fixups.ok())
        return fail(fixups.error().code == ErrorCode::Unsupported ? LoadFailure::Unsupported : LoadFailure::Incompatible, fixups.error().message, report);
    for (auto& f : fixups.value()) (f.kind == dyld::Fixup::Kind::Bind ? report.binds : report.rebases)++;
    dyld::ResolutionReport res = dyld::resolve_symbols(img, fixups.value(), provider);
    for (auto& l : res.libraries) {
        bool dup = false;
        for (auto& x : report.required_libs) dup |= x == l;
        if (!dup) report.required_libs.push_back(l);
    }
    if (!res.unresolved.empty()) {
        report.unresolved = res.unresolved;
        std::string msg;
        for (auto& u : res.unresolved) msg += (msg.empty() ? "" : "; ") + std::format("unresolved symbol {} (library {}: {})", u.symbol, u.library, u.reason);
        return fail(LoadFailure::UnresolvedSymbol, msg, report);
    }

    // 4. mapping
    auto mapped = map_segments(layout.value(), bytes.value());
    if (!mapped.ok()) return fail(LoadFailure::Internal, "mapping failed: " + mapped.error().message, report);
    LoadedImage image = std::move(mapped.value());

    // 5. apply fixups, then final W^X protections. A failure from here on releases the region (RAII).
    if (auto a = apply_fixups(image, img, fixups.value(), res.addresses); !a.ok())
        return fail(LoadFailure::Internal, "applying fixups failed: " + a.error().message, report);
    log::info(log::Category::Dyld, "applied {} rebase(s) and {} bind(s)", report.rebases, report.binds);
    if (auto p = finalize_protections(image); !p.ok())
        return fail(LoadFailure::Internal, "setting final protections failed: " + p.error().message, report);

    LoadResult ok;
    ok.report = report;
    ok.image = std::move(image);
    return ok;
}

}  // namespace orvyn::loader
