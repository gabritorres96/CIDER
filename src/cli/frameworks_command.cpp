// `orvyn frameworks [program|App.app] [--json]`: without an argument, lists the Apple libraries ORVYN recognises and how far each goes; with a
// Mach-O, classifies every library it names and every symbol it imports WITHOUT running anything (stages B and C of an external-binary
// bring-up). "implemented" means a host function / class / data symbol exists, "stub" means the call stops the guest, "missing" means
// the link would fail. Whether Apple's library really exports a symbol needs the SDK stubs (`orvyn sdk-scan`).
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>

#include "orvyn/bundle.hpp"
#include "orvyn/fixups.hpp"
#include "orvyn/frameworks.hpp"
#include "orvyn/json.hpp"
#include "orvyn/libsystem.hpp"
#include "orvyn/loader.hpp"
#include "orvyn/macho.hpp"
#include "orvyn/objc.hpp"
#include "orvyn/util.hpp"
#include "commands.hpp"

namespace fs = std::filesystem;

namespace orvyn::cli {
namespace {

enum class State { Implemented, Stub, Missing, LibraryNotProvided };
const char* to_string(State s) {
    switch (s) {
        case State::Implemented: return "implemented";
        case State::Stub: return "stub";
        case State::Missing: return "missing";
        case State::LibraryNotProvided: return "library-not-provided";
    }
    return "?";
}

struct Import {
    std::string symbol, library;
    bool weak = false, lazy = false;
    State state = State::Missing;
};

struct LibRow {
    std::string path, kind, status, note;
    bool recognised = false;
};

const char* dylib_kind(macho::DylibKind k) {
    switch (k) {
        case macho::DylibKind::Load: return "load";
        case macho::DylibKind::Weak: return "weak";
        case macho::DylibKind::Reexport: return "reexport";
        case macho::DylibKind::Lazy: return "lazy";
        case macho::DylibKind::Upward: return "upward";
        default: return "other";
    }
}

int registry(const Options& o) {
    const auto& fw = libsystem::known_frameworks();
    std::map<libsystem::Lib, size_t> stubs;
    for (const auto& s : libsystem::stub_symbols()) ++stubs[s.lib];
    if (o.json) {
        JsonWriter w;
        w.begin_object();
        w.key("frameworks");
        w.begin_array();
        for (const auto& f : fw) {
            w.begin_object();
            w.field("name", f.name);
            w.field("install_name", f.install_name);
            w.field("status", libsystem::to_string(f.status));
            w.field("stubs", static_cast<uint64_t>(stubs[f.lib]));
            w.field("note", f.note);
            w.end_object();
        }
        w.end_array();
        w.end_object();
        std::puts(w.str().c_str());
        return 0;
    }
    std::puts("ORVYN - Darwin Executable Runtime\n\nRecognised Apple libraries (recognised is NOT the same as implemented):\n");
    std::printf("%-20s %-28s %5s  %s\n", "library", "status", "stubs", "note");
    for (const auto& f : fw) std::printf("%-20s %-28s %5zu  %s\n", f.name, libsystem::to_string(f.status), stubs[f.lib], f.note);
    std::puts("\nAnything not listed is absent (not recognised): an import from it is refused at load. A framework's status says nothing about each of its symbols:\nan implemented subset still refuses what it lacks, and a stub resolves a call to a diagnostic, never to a result.");
    return 0;
}

}  // namespace

int cmd_frameworks(const Options& o) {
    if (o.positional.empty()) return registry(o);
    fs::path target = path_from_utf8(o.positional[0]);
    std::error_code ec;
    if (!fs::exists(target, ec)) { std::fprintf(stderr, "error: path does not exist: %s\n", o.positional[0].c_str()); return 1; }
    if (fs::is_directory(target, ec)) {
        auto b = bundle::inspect(target);
        if (!b.ok() || !b.value().executable_found) { std::fprintf(stderr, "error: not an application bundle with a main executable\n"); return 1; }
        target = b.value().executable_path;
    }
    auto bytes = read_file(target, macho::Limits{}.max_file_size);
    if (!bytes.ok()) { std::fprintf(stderr, "error: %s\n", bytes.error().message.c_str()); return 1; }
    auto parsed = macho::parse_file(ByteView(bytes.value()));
    if (!parsed.ok()) { std::fprintf(stderr, "error: not a valid Mach-O file: %s\n", parsed.error().message.c_str()); return 1; }
    const macho::Slice* sl = parsed.value().find_slice(macho::kCpuX86_64);
    if (!sl || !sl->image) { std::fprintf(stderr, "error: no usable x86_64 slice\n"); return 1; }
    const macho::Image& img = *sl->image;

    // Provider with the Objective-C runtime attached, so classes and Foundation data are visible like in a real run.
    libsystem::RuntimeContext ctx;
    ctx.objc = std::make_unique<objc::Runtime>(ctx);
    objc::install_foundation(*ctx.objc);
    auto provider = libsystem::LibSystemProvider::create();
    if (!provider.ok()) { std::fprintf(stderr, "error: %s\n", provider.error().message.c_str()); return 1; }
    objc::Runtime* rt = ctx.objc.get();
    provider.value()->set_dynamic_data([rt](std::string_view s) { return rt->dynamic_symbol(s); });

    std::vector<LibRow> libs;
    for (const auto& d : img.dylibs) {
        LibRow r;
        r.path = d.path;
        r.kind = dylib_kind(d.kind);
        if (const auto* f = libsystem::find_framework(d.path)) {
            r.recognised = true;
            r.status = libsystem::to_string(f->status);
            r.note = f->note;
        } else {
            r.status = "not provided";
            r.note = d.path.rfind("@", 0) == 0 ? "searched next to the program (@rpath/@loader_path); otherwise the load fails"
                                                : "ORVYN never loads Apple system libraries from disk";
        }
        libs.push_back(std::move(r));
    }

    std::vector<Import> imports;
    std::string fixup_error;
    auto fx = dyld::decode_fixups(img, ByteView(bytes.value()).slice(sl->offset, sl->size).value());
    if (!fx.ok()) fixup_error = fx.error().message;
    else {
        std::set<std::pair<int64_t, std::string>> seen;
        for (const auto& f : fx.value()) {
            if (f.kind != dyld::Fixup::Kind::Bind || !seen.insert({f.library_ordinal, f.symbol}).second) continue;
            Import im;
            im.symbol = f.symbol;
            im.weak = f.weak_import;
            im.lazy = f.lazy;
            std::string lib;
            if (f.library_ordinal >= 1 && static_cast<size_t>(f.library_ordinal) <= img.dylibs.size()) lib = img.dylibs[static_cast<size_t>(f.library_ordinal) - 1].path;
            im.library = f.library_ordinal >= 1 ? lib : (f.library_ordinal == -2 ? "(flat lookup)" : f.library_ordinal == 0 ? "(self)" : "(main executable)");
            if (f.library_ordinal >= 1 && !provider.value()->provides_library(lib)) im.state = State::LibraryNotProvided;
            else if (f.library_ordinal < 1) im.state = provider.value()->lookup("", f.symbol) ? State::Implemented : State::Missing;
            else if (auto r = provider.value()->lookup(lib, f.symbol)) im.state = r->stub ? State::Stub : State::Implemented;
            else im.state = State::Missing;
            imports.push_back(std::move(im));
        }
    }
    size_t counts[4] = {};
    size_t blocking = 0;
    for (const auto& im : imports) {
        ++counts[static_cast<int>(im.state)];
        if ((im.state == State::Missing || im.state == State::LibraryNotProvided) && !im.weak) ++blocking;
    }
    bool exceptions = false;
    for (const auto& im : imports)
        if (im.symbol.find("_objc_begin_catch") == 0 || im.symbol.find("_Unwind_Resume") != std::string::npos || im.symbol.find("OBJC_EHTYPE_") != std::string::npos) exceptions = true;

    if (o.json) {
        JsonWriter w;
        w.begin_object();
        w.field("path", path_to_utf8(target));
        w.field("analysis_only", true);
        w.key("libraries");
        w.begin_array();
        for (const auto& l : libs) {
            w.begin_object();
            w.field("install_name", l.path);
            w.field("kind", l.kind);
            w.field("recognised", l.recognised);
            w.field("status", l.status);
            w.field("note", l.note);
            w.end_object();
        }
        w.end_array();
        w.key("imports");
        w.begin_array();
        for (const auto& im : imports) {
            w.begin_object();
            w.field("symbol", im.symbol);
            w.field("library", im.library);
            w.field("state", to_string(im.state));
            w.field("weak", im.weak);
            w.field("lazy", im.lazy);
            w.end_object();
        }
        w.end_array();
        w.key("summary");
        w.begin_object();
        w.field("implemented", static_cast<uint64_t>(counts[0]));
        w.field("stub", static_cast<uint64_t>(counts[1]));
        w.field("missing", static_cast<uint64_t>(counts[2]));
        w.field("library_not_provided", static_cast<uint64_t>(counts[3]));
        w.field("blocking_imports", static_cast<uint64_t>(blocking));
        w.field("references_objc_exceptions", exceptions);
        w.field("would_link", blocking == 0 && fixup_error.empty());
        w.end_object();
        w.end_object();
        std::puts(w.str().c_str());
        return blocking == 0 ? 0 : 1;
    }
    std::printf("ORVYN - Darwin Executable Runtime\n\nStatic analysis only: nothing was executed.\n\n%s\n\nLibraries:\n", path_to_utf8(target).c_str());
    for (const auto& l : libs) std::printf("  [%-8s] %-72s %s\n", l.kind.c_str(), l.path.c_str(), l.status.c_str());
    if (!fixup_error.empty()) std::printf("\nFixups could not be decoded: %s\n", fixup_error.c_str());
    std::printf("\nImports (%zu):\n", imports.size());
    for (const auto& im : imports)
        if (im.state != State::Implemented)
            std::printf("  %-22s %-44s %s%s\n", to_string(im.state), im.symbol.c_str(), im.library.c_str(), im.weak ? "  (weak)" : "");
    std::printf("\nimplemented %zu, stub %zu, missing %zu, library not provided %zu; blocking imports: %zu\n", counts[0], counts[1], counts[2], counts[3], blocking);
    if (exceptions) std::puts("references Objective-C exception symbols (@try/@catch): ORVYN has no unwinder; see docs/LIMITATIONS.md");
    std::printf("The link %s.\n", blocking == 0 && fixup_error.empty() ? "would succeed (this says nothing about running correctly)" : "would FAIL");
    return blocking == 0 ? 0 : 1;
}

}  // namespace orvyn::cli
