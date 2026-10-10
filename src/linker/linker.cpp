#include "cider/linker.hpp"

#include <algorithm>
#include <cstring>
#include <format>

#include "cider/log.hpp"
#include "cider/util.hpp"

namespace fs = std::filesystem;

namespace cider::linker {

struct Program::Lookup {
    std::optional<uint64_t> address;
    std::string reason;  // why it was not found (only meaningful when address is empty)
};

namespace {

std::string version_string(uint32_t v) { return std::format("{}.{}.{}", v >> 16, (v >> 8) & 0xff, v & 0xff); }

constexpr uint32_t kSecTypeMask = 0xff, kSecModInitFuncPointers = 0x9, kSecInitFuncOffsets = 0x16;
constexpr size_t kMaxInitializersPerImage = 4096;

// Install names and rpaths come from the (untrusted) image: only sane text is turned into a filesystem path.
bool sane_path_text(const std::string& s) {
    if (s.empty() || s.size() > 1024) return false;
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;   // drive letters, streams, UNC, wildcards: not Darwin paths
    if (s.rfind("//", 0) == 0) return false;                  // would be a UNC path on Windows
    return sanitize_utf8(s) == s;
}

}  // namespace

class Linker {
public:
    Linker(Program& p, const LinkOptions& o, dyld::SymbolProvider& libsystem, LinkError& e) : prog_(p), opts_(o), libsystem_(libsystem), err_(e) {
        vopts_ = o.validator;
        vopts_.linker_resolves_dependencies = true;
        vopts_.allow_initializers = true;
        vopts_.allow_tls = true;
        if (!o.executable_path.empty()) {
            std::error_code ec;
            exe_dir_ = fs::absolute(o.executable_path, ec).parent_path();
        }
    }

    bool run(ByteView main_file) {
        log::info(log::Category::Dyld, "Loading main executable");
        auto idx = add_image(main_file, opts_.executable_path, "main executable", true, nullptr);
        if (!idx) return false;
        std::vector<size_t> stack{*idx};
        if (!load_dependencies(*idx, stack, {})) return false;
        if (!missing_.empty()) {
            std::string msg;
            for (auto& m : missing_) msg += (msg.empty() ? "" : "; ") + m;
            return fail(loader::LoadFailure::MissingDependency, msg);
        }
        log::info(log::Category::Dyld, "Dependencies resolved ({} image(s))", prog_.images_.size());

        if (!map_all()) return false;
        if (!bind_all()) return false;
        if (opts_.before_protect)
            for (auto& r : prog_.images_) {
                std::string why;
                if (!opts_.before_protect(*r, why)) return fail(loader::LoadFailure::Incompatible, std::format("{}: {}", r->leaf, why));
            }
        if (!protect_all()) return false;
        if (!collect_initializers()) return false;
        if (!collect_tls()) return false;
        for (auto& r : prog_.images_) { r->owned_bytes.clear(); r->owned_bytes.shrink_to_fit(); r->slice = {}; }
        return true;
    }

private:
    Program& prog_;
    const LinkOptions& opts_;
    dyld::SymbolProvider& libsystem_;
    LinkError& err_;
    loader::ValidatorOptions vopts_;
    fs::path exe_dir_;
    std::vector<std::string> missing_;

    bool fail(loader::LoadFailure k, std::string m) {
        err_.kind = k;
        err_.message = std::move(m);
        err_.required_libs = prog_.required_libs_;
        return false;
    }

    void note_lib(const std::string& n) {
        if (std::find(prog_.required_libs_.begin(), prog_.required_libs_.end(), n) == prog_.required_libs_.end()) prog_.required_libs_.push_back(n);
    }

    // ---- 1. discovery ---------------------------------------------------------------------------
    // Parses and validates one image and appends it to the registry. `owned` holds the bytes for dependencies.
    std::optional<size_t> add_image(ByteView bytes, const fs::path& path, const std::string& label, bool is_main, std::vector<uint8_t>* owned) {
        auto parsed = macho::parse_file(bytes, macho::ParseOptions{});
        if (!parsed.ok()) {
            fail(loader::LoadFailure::Incompatible, std::format("{}: not a valid Mach-O file: {}", label, parsed.error().message));
            return std::nullopt;
        }
        const macho::Slice* sl = parsed.value().find_slice(macho::kCpuX86_64);
        if (!sl) {
            std::string have;
            for (auto& s : parsed.value().slices) have += (have.empty() ? "" : ", ") + macho::cpu_name(s.cputype, s.cpusubtype);
            fail(loader::LoadFailure::Incompatible, std::format("{}: no x86_64 slice (found: {})", label, have));
            return std::nullopt;
        }
        if (!sl->image) {
            fail(loader::LoadFailure::Incompatible, std::format("{}: x86_64 slice failed validation: {}", label, sl->error ? sl->error->message : std::string("unknown")));
            return std::nullopt;
        }
        auto slice = bytes.slice(sl->offset, sl->size);
        if (!slice.ok()) { fail(loader::LoadFailure::Internal, label + ": slice outside the file"); return std::nullopt; }

        loader::ValidatorOptions v = vopts_;
        v.as_dependency = !is_main;
        loader::ValidationReport rep = loader::validate_for_execution(*sl->image, v);
        if (!rep.ok()) {
            std::string msg;
            loader::LoadFailure kind = loader::LoadFailure::Incompatible;
            for (auto& i : rep.issues) {
                msg += (msg.empty() ? "" : "; ") + i.message;
                if (i.kind == loader::Issue::Kind::Unsupported) kind = loader::LoadFailure::Unsupported;
            }
            fail(kind, std::format("{}: {}", label, msg));
            return std::nullopt;
        }

        log::info(log::Category::Loader, "Mach-O x86_64 detected ({} segment(s), {} load command(s))", sl->image->segments.size(), sl->image->ncmds);
        auto rec = std::make_unique<ImageRecord>();
        rec->index = prog_.images_.size();
        rec->is_main = is_main;
        rec->path = path;
        rec->leaf = path.empty() ? label : path_to_utf8(path.filename());
        rec->name = (!is_main && sl->image->id_dylib) ? sl->image->id_dylib->path : rec->leaf;
        rec->info = *sl->image;
        if (owned) rec->owned_bytes = std::move(*owned);
        rec->slice = slice.value();
        if (owned) {
            // the view must point into the record's own storage (the vector was moved)
            auto again = ByteView(rec->owned_bytes).slice(sl->offset, sl->size);
            rec->slice = again.value();
        }
        auto exports = dyld::decode_exports(rec->info, rec->slice);
        if (!exports.ok()) {
            fail(loader::LoadFailure::Incompatible, std::format("{}: {}", rec->leaf, exports.error().message));
            return std::nullopt;
        }
        rec->exports = std::move(exports.value());
        for (size_t i = 0; i < rec->exports.size(); ++i) rec->export_index.emplace(rec->exports[i].name, i);
        rec->deps.assign(rec->info.dylibs.size(), kAbsentDep);
        for (auto& d : rec->info.dylibs) note_lib(d.path);
        prog_.images_.push_back(std::move(rec));
        return prog_.images_.size() - 1;
    }

    std::optional<size_t> find_loaded(const fs::path& file) const {
        std::error_code ec;
        for (auto& r : prog_.images_)
            if (!r->path.empty() && fs::equivalent(r->path, file, ec) && !ec) return r->index;
        return std::nullopt;
    }

    bool load_dependencies(size_t idx, std::vector<size_t>& stack, std::vector<dyld::RpathEntry> inherited) {
        if (stack.size() > opts_.max_depth) return fail(loader::LoadFailure::Incompatible, std::format("dependency chain deeper than {}", opts_.max_depth));
        // The record's address is stable (unique_ptr); the vector may grow while we recurse.
        ImageRecord& me = *prog_.images_[idx];
        fs::path my_dir = me.is_main ? exe_dir_ : me.path.parent_path();

        std::vector<dyld::RpathEntry> rpaths;   // own entries first, then the loaders' (dyld semantics)
        for (auto& r : me.info.rpaths) {
            if (!sane_path_text(r)) return fail(loader::LoadFailure::Incompatible, std::format("{}: LC_RPATH entry is not valid path text", me.leaf));
            rpaths.push_back({r, my_dir});
        }
        rpaths.insert(rpaths.end(), inherited.begin(), inherited.end());

        for (size_t di = 0; di < me.info.dylibs.size(); ++di) {
            const macho::Dylib dep = me.info.dylibs[di];
            const bool weak = dep.kind == macho::DylibKind::Weak;
            if (!sane_path_text(dep.path)) return fail(loader::LoadFailure::Incompatible, std::format("{}: dependency install name is not valid path text", me.leaf));
            if (libsystem_.provides_library(dep.path)) { me.deps[di] = kProvidedDep; continue; }

            std::vector<fs::path> cands;
            std::string how;
            if (dyld::is_system_path(dep.path)) {
                how = "macOS system library; CIDER does not load system libraries and never substitutes Windows DLLs";
            } else if (dyld::anchor_of(dep.path) != dyld::Anchor::None && me.is_main && exe_dir_.empty()) {
                how = "the path of the main executable is unknown, so the anchor cannot be expanded";
            } else {
                cands = dyld::search_candidates(dep.path, exe_dir_, my_dir, rpaths);
                if (cands.empty()) how = dyld::anchor_of(dep.path) == dyld::Anchor::Rpath ? "no LC_RPATH entry is usable" : "relative install names are not searched";
            }

            fs::path found;
            std::string searched;
            for (auto& c : cands) {
                std::error_code ec;
                searched += (searched.empty() ? "" : "; ") + path_to_utf8(c);
                if (fs::is_regular_file(c, ec) && !ec) { found = c; break; }
            }
            if (found.empty()) {
                if (weak) {
                    log::warn(log::Category::Dyld, "weak library {} not found: its symbols bind to NULL", dep.path);
                    me.deps[di] = kAbsentDep;
                    continue;
                }
                if (err_.missing_library.empty()) err_.missing_library = dep.path;
                log::error("Missing library: {}", dep.path);
                missing_.push_back(std::format("library not found: {} (required by {}; {})", dep.path, me.leaf, searched.empty() ? how : "searched: " + searched));
                continue;   // keep going: every missing library is reported in one go
            }

            size_t target;
            if (auto loaded = find_loaded(found)) {
                target = *loaded;
                if (std::find(stack.begin(), stack.end(), target) != stack.end()) {
                    std::string chain;
                    for (size_t s : stack) chain += prog_.images_[s]->leaf + " -> ";
                    chain += prog_.images_[target]->leaf;
                    prog_.cycles_.push_back(chain);
                    if (opts_.reject_cycles) return fail(loader::LoadFailure::Incompatible, "dependency cycle: " + chain);
                    log::warn(log::Category::Dyld, "dependency cycle (legal in Mach-O): {}", chain);
                }
            } else {
                if (prog_.images_.size() >= opts_.max_images) return fail(loader::LoadFailure::Incompatible, std::format("more than {} images", opts_.max_images));
                auto bytes = read_file(found, opts_.max_file_size);
                if (!bytes.ok()) return fail(loader::LoadFailure::Incompatible, std::format("cannot read {}: {}", path_to_utf8(found), bytes.error().message));
                log::info(log::Category::Dyld, "Loading {}", path_to_utf8(found.filename()));
                std::vector<uint8_t> owned = std::move(bytes.value());
                ByteView view(owned);
                auto added = add_image(view, found, path_to_utf8(found.filename()), false, &owned);
                if (!added) return false;
                target = *added;
                stack.push_back(target);
                bool ok = load_dependencies(target, stack, rpaths);
                stack.pop_back();
                if (!ok) return false;
            }
            me.deps[di] = static_cast<int>(target);

            const ImageRecord& t = *prog_.images_[target];
            if (t.info.id_dylib && dep.compat_version > t.info.id_dylib->current_version)
                return fail(loader::LoadFailure::Incompatible,
                            std::format("{} requires {} compatibility version {} but {} is version {}", me.leaf, dep.path, version_string(dep.compat_version), t.leaf,
                                        version_string(t.info.id_dylib->current_version)));
        }
        return true;
    }

    // ---- 2. mapping -------------------------------------------------------------------------------
    bool map_all() {
        for (auto& r : prog_.images_) {
            auto layout = loader::plan_layout(r->info, vopts_);
            if (!layout.ok()) return fail(loader::LoadFailure::Incompatible, std::format("{}: {}", r->leaf, layout.error().message));
            log::info(log::Category::Memory, "mapping {}", r->leaf);
            auto mapped = loader::map_segments(layout.value(), r->slice);
            if (!mapped.ok()) return fail(loader::LoadFailure::Internal, std::format("{}: mapping failed: {}", r->leaf, mapped.error().message));
            r->mapped = std::move(mapped.value());
            const macho::Segment* text = nullptr;
            for (auto& s : r->info.segments)
                if (s.name != "__PAGEZERO" && s.fileoff == 0 && s.filesize > 0) { text = &s; break; }
            uint8_t* h = text ? r->mapped.host_address(text->vmaddr) : nullptr;
            if (!h) return fail(loader::LoadFailure::Incompatible, std::format("{}: no mapped segment contains the Mach-O header", r->leaf));
            r->header = reinterpret_cast<uint64_t>(h);
        }
        return true;
    }

    // ---- 3. symbols and fixups ----------------------------------------------------------------------
    bool bind_all() {
        std::vector<dyld::Unresolved> unresolved;
        struct Pending { ImageRecord* rec; std::vector<dyld::Fixup> fixups; std::vector<uint64_t> values; };
        std::vector<Pending> pending;

        for (auto& r : prog_.images_) {
            auto fx = dyld::decode_fixups(r->info, r->slice);
            if (!fx.ok())
                return fail(fx.error().code == ErrorCode::Unsupported ? loader::LoadFailure::Unsupported : loader::LoadFailure::Incompatible,
                            std::format("{}: {}", r->leaf, fx.error().message));
            Pending p{r.get(), std::move(fx.value()), {}};
            p.values.assign(p.fixups.size(), 0);
            std::map<std::pair<int64_t, std::string>, std::pair<bool, uint64_t>> cache;
            for (size_t i = 0; i < p.fixups.size(); ++i) {
                const dyld::Fixup& f = p.fixups[i];
                if (f.kind != dyld::Fixup::Kind::Bind) { ++r->rebases; continue; }
                ++r->binds;
                auto key = std::make_pair(f.library_ordinal, f.symbol);
                auto hit = cache.find(key);
                if (hit != cache.end()) {
                    if (hit->second.first) p.values[i] = hit->second.second;
                    continue;
                }
                dyld::Unresolved u;
                auto addr = resolve(*r, f, u);
                if (addr) {
                    p.values[i] = *addr;
                    cache[key] = {true, *addr};
                    log::debug(log::Category::Dyld, "Binding {} in {} -> {:#x}", f.symbol, r->leaf, *addr);
                } else if (f.weak_import) {
                    log::warn(log::Category::Symbols, "weak import {} of {} unavailable: bound to NULL", f.symbol, r->leaf);
                    cache[key] = {true, 0};
                } else {
                    cache[key] = {false, 0};
                    unresolved.push_back(std::move(u));
                }
            }
            pending.push_back(std::move(p));
        }
        if (!unresolved.empty()) {
            err_.unresolved = unresolved;
            std::string msg;
            for (auto& u : unresolved) {
                log::error("Unresolved symbol: {}", u.symbol);
                msg += (msg.empty() ? "" : "; ") + std::format("unresolved symbol {} (library {}: {})", u.symbol, u.library, u.reason);
            }
            return fail(loader::LoadFailure::UnresolvedSymbol, msg);
        }
        for (auto& p : pending) {
            if (auto a = loader::apply_fixups_resolved(p.rec->mapped, p.rec->info, p.fixups, p.values); !a.ok())
                return fail(loader::LoadFailure::Internal, std::format("{}: applying fixups failed: {}", p.rec->leaf, a.error().message));
            log::info(log::Category::Dyld, "{}: applied {} rebase(s) and {} bind(s)", p.rec->leaf, p.rec->rebases, p.rec->binds);
        }
        return true;
    }

    std::optional<uint64_t> resolve(const ImageRecord& rec, const dyld::Fixup& f, dyld::Unresolved& u) {
        u.symbol = f.symbol;
        u.weak = f.weak_import;
        log::debug(log::Category::Dyld, "Resolving {} for {}", f.symbol, rec.leaf);
        std::vector<bool> visited(prog_.images_.size(), false);
        if (f.library_ordinal >= 1) {
            const size_t di = static_cast<size_t>(f.library_ordinal) - 1;
            if (di >= rec.deps.size()) {
                u.library = "ordinal " + std::to_string(f.library_ordinal);
                u.reason = "dylib ordinal outside the load commands";
                return std::nullopt;
            }
            const std::string& libname = rec.info.dylibs[di].path;
            u.library = libname;
            const int dep = rec.deps[di];
            if (dep == kAbsentDep) { u.reason = "weak library is not present"; return std::nullopt; }
            if (dep == kProvidedDep) return provided(libname, f.symbol, u);
            auto l = prog_.lookup(static_cast<size_t>(dep), f.symbol, visited, 0);
            if (!l.address) u.reason = l.reason.empty() ? "symbol not exported by the library" : l.reason;
            return l.address;
        }
        if (f.library_ordinal == 0) {
            u.library = "(self)";
            auto l = prog_.lookup(rec.index, f.symbol, visited, 0);
            if (!l.address) u.reason = l.reason.empty() ? "symbol not defined by the image itself" : l.reason;
            return l.address;
        }
        if (f.library_ordinal == -1) {
            u.library = "(main executable)";
            auto l = prog_.lookup(0, f.symbol, visited, 0);
            if (!l.address) u.reason = l.reason.empty() ? "symbol not defined by the main executable" : l.reason;
            return l.address;
        }
        if (f.library_ordinal == -2) {   // flat namespace: main executable, then every library, then libSystem
            u.library = "(flat lookup)";
            if (auto a = prog_.find_symbol_anywhere(f.symbol)) return a;
            for (auto& l : prog_.required_libs_)
                if (libsystem_.provides_library(l))
                    if (auto r = libsystem_.lookup(l, f.symbol)) return r->address;
            u.reason = "symbol not found in any loaded image";
            return std::nullopt;
        }
        u.library = "ordinal " + std::to_string(f.library_ordinal);
        u.reason = "unsupported special library ordinal";
        return std::nullopt;
    }

    std::optional<uint64_t> provided(const std::string& lib, const std::string& symbol, dyld::Unresolved& u) {
        if (auto r = libsystem_.lookup(lib, symbol)) return r->address;
        u.reason = "symbol not implemented by CIDER";
        return std::nullopt;
    }

    // ---- 4. protections and initializers --------------------------------------------------------------
    bool protect_all() {
        for (auto& r : prog_.images_)
            if (auto p = loader::finalize_protections(r->mapped); !p.ok())
                return fail(loader::LoadFailure::Internal, std::format("{}: setting final protections failed: {}", r->leaf, p.error().message));
        return true;
    }

    bool collect_initializers() {
        for (auto& r : prog_.images_) {
            for (auto& seg : r->info.segments) {
                for (auto& sec : seg.sections) {
                    const uint32_t type = sec.flags & kSecTypeMask;
                    if ((type != kSecModInitFuncPointers && type != kSecInitFuncOffsets) || sec.size == 0) continue;
                    const uint64_t unit = type == kSecModInitFuncPointers ? 8 : 4;
                    if (sec.size % unit != 0) return fail(loader::LoadFailure::Incompatible, std::format("{}: initializer section {} has a size that is not a multiple of {}", r->leaf, sec.name, unit));
                    const uint64_t count = sec.size / unit;
                    if (count + r->initializers.size() > kMaxInitializersPerImage) return fail(loader::LoadFailure::Incompatible, std::format("{}: too many initializers", r->leaf));
                    // The section must lie inside its mapped segment.
                    const uint8_t* data = r->mapped.host_address(sec.addr);
                    if (!data || !r->mapped.host_address(sec.addr + sec.size - 1))
                        return fail(loader::LoadFailure::Incompatible, std::format("{}: initializer section {} is outside the mapped image", r->leaf, sec.name));
                    for (uint64_t k = 0; k < count; ++k) {
                        uint64_t target;
                        if (type == kSecModInitFuncPointers) {
                            std::memcpy(&target, data + k * 8, 8);        // already rebased
                        } else {
                            uint32_t off;
                            std::memcpy(&off, data + k * 4, 4);
                            target = r->header + off;
                        }
                        if (!in_executable(*r, target))
                            return fail(loader::LoadFailure::Incompatible, std::format("{}: initializer {:#x} is not inside an executable segment", r->leaf, target));
                        r->initializers.push_back(target);
                    }
                }
            }
        }
        return true;
    }

    // Thread-local storage: template layout and descriptor validation. The descriptors themselves are served at run time by
    // libSystem's tlv_get_addr (the first quad of every descriptor is bound to __tlv_bootstrap by the image's own fixups).
    bool collect_tls() {
        constexpr uint32_t kRegular = 0x11, kZerofill = 0x12, kVariables = 0x13;
        constexpr uint64_t kMaxTemplate = 16ull << 20;
        auto bootstrap = libsystem_.lookup("", "__tlv_bootstrap");
        for (auto& r : prog_.images_) {
            bool have_template = false;
            uint64_t start = 0, end = 0, data_end = 0, align = 1;
            std::vector<const macho::Section*> vars;
            for (auto& seg : r->info.segments) {
                for (auto& sec : seg.sections) {
                    const uint32_t type = sec.flags & kSecTypeMask;
                    if (type == kRegular || type == kZerofill) {
                        if (sec.size == 0) continue;
                        if (!have_template) { start = sec.addr; have_template = true; }
                        else if (sec.addr < start) return fail(loader::LoadFailure::Incompatible, std::format("{}: thread-local sections are not in address order", r->leaf));
                        end = std::max(end, sec.addr + sec.size);
                        if (type == kRegular) data_end = std::max(data_end, sec.addr + sec.size);
                        align = std::max<uint64_t>(align, uint64_t{1} << std::min<uint32_t>(sec.align, 20));
                    } else if (type == kVariables && sec.size) {
                        vars.push_back(&sec);
                    }
                }
            }
            if (!have_template && vars.empty()) continue;
            const uint64_t size = have_template ? end - start : 0;
            if (size > kMaxTemplate) return fail(loader::LoadFailure::Incompatible, std::format("{}: thread-local template larger than {} bytes", r->leaf, kMaxTemplate));
            if (align > 4096) return fail(loader::LoadFailure::Unsupported, std::format("{}: thread-local alignment {} is not supported", r->leaf, align));
            if (have_template && (!r->mapped.host_address(start) || !r->mapped.host_address(end - 1)))
                return fail(loader::LoadFailure::Incompatible, std::format("{}: thread-local template is outside the mapped image", r->leaf));
            // every descriptor: {thunk, key, offset}
            for (auto* sec : vars) {
                if (sec->size % 24 != 0) return fail(loader::LoadFailure::Incompatible, std::format("{}: {} is not a whole number of 24-byte descriptors", r->leaf, sec->name));
                const uint8_t* d = r->mapped.host_address(sec->addr);
                if (!d || !r->mapped.host_address(sec->addr + sec->size - 1)) return fail(loader::LoadFailure::Incompatible, std::format("{}: {} is outside the mapped image", r->leaf, sec->name));
                for (uint64_t k = 0; k < sec->size / 24; ++k) {
                    uint64_t thunk, offset;
                    std::memcpy(&thunk, d + 24 * k, 8);
                    std::memcpy(&offset, d + 24 * k + 16, 8);
                    if (!bootstrap || thunk != bootstrap->address)
                        return fail(loader::LoadFailure::Unsupported, std::format("{}: a thread-local descriptor has a thunk other than _tlv_bootstrap", r->leaf));
                    if (offset >= size) return fail(loader::LoadFailure::Incompatible, std::format("{}: a thread-local descriptor points outside the template", r->leaf));
                }
                r->tls.descriptors += sec->size / 24;
            }
            r->tls.tmpl = static_cast<uint64_t>(static_cast<int64_t>(start) + r->mapped.slide);
            r->tls.tmpl_size = size;
            r->tls.init_size = have_template ? data_end - start : 0;
            r->tls.align = align;
            log::info(log::Category::Dyld, "{}: thread-local storage: {} variable(s), template {} bytes", r->leaf, r->tls.descriptors, size);
        }
        return true;
    }

    static bool in_executable(const ImageRecord& r, uint64_t addr) {
        for (auto& s : r.mapped.layout.segments) {
            if (s.final_prot != memory::Prot::ReadExecute) continue;
            const uint64_t lo = static_cast<uint64_t>(static_cast<int64_t>(s.vmaddr) + r.mapped.slide);
            if (addr >= lo && addr - lo < s.vmsize) return true;
        }
        return false;
    }
};

// ---- Program ----------------------------------------------------------------------------------------

std::unique_ptr<Program> Program::link(ByteView main_file, const LinkOptions& opts, dyld::SymbolProvider& libsystem, LinkError& error) {
    std::unique_ptr<Program> p(new Program());
    p->libsystem_ = &libsystem;
    Linker linker(*p, opts, libsystem, error);
    bool ok = false;
    try {
        ok = linker.run(main_file);
    } catch (const std::exception& e) {   // e.g. the filesystem layer refusing a hostile path
        error.kind = loader::LoadFailure::Internal;
        error.message = std::string("unexpected failure while linking: ") + e.what();
    }
    if (!ok) {
        if (error.required_libs.empty()) error.required_libs = p->required_libs_;
        p->unload();
        return nullptr;
    }
    return p;
}

Program::~Program() { unload(); }

void Program::unload() {
    while (!images_.empty()) {
        log::debug(log::Category::Dyld, "Unloading {}", images_.back()->leaf);
        images_.pop_back();   // reverse load order; the mapping is released by the Region destructor
    }
}

Program::Lookup Program::lookup(size_t idx, std::string_view name, std::vector<bool>& visited, int depth) const {
    Lookup out;
    if (idx >= images_.size() || depth > 32 || visited[idx]) return out;
    visited[idx] = true;
    const ImageRecord& r = *images_[idx];

    auto via_dep = [&](int dep, const macho::Dylib& d, std::string_view sym) -> Lookup {
        Lookup l;
        if (dep >= 0) return lookup(static_cast<size_t>(dep), sym, visited, depth + 1);
        if (dep == kProvidedDep) {
            if (auto s = libsystem_->lookup(d.path, sym)) l.address = s->address;
            else l.reason = "symbol not implemented by CIDER";
        }
        return l;
    };

    if (auto it = r.export_index.find(name); it != r.export_index.end()) {
        const dyld::ExportedSymbol& e = r.exports[it->second];
        if (e.is_reexport()) {
            if (e.reexport_ordinal == 0 || e.reexport_ordinal > r.deps.size()) { out.reason = "re-export names a library that does not exist"; return out; }
            const size_t di = static_cast<size_t>(e.reexport_ordinal) - 1;
            return via_dep(r.deps[di], r.info.dylibs[di], e.reexport_name.empty() ? name : std::string_view(e.reexport_name));
        }
        if (e.flags & dyld::kExportStubAndResolver) { out.reason = "symbol is a resolver function (STUB_AND_RESOLVER), not supported"; return out; }
        out.address = e.is_absolute() ? e.address : r.header + e.address;
        return out;
    }
    for (size_t i = 0; i < r.info.dylibs.size(); ++i) {
        if (r.info.dylibs[i].kind != macho::DylibKind::Reexport) continue;
        Lookup l = via_dep(r.deps[i], r.info.dylibs[i], name);
        if (l.address) return l;
        if (!l.reason.empty()) out.reason = l.reason;
    }
    return out;
}

std::optional<uint64_t> Program::find_symbol(size_t image, std::string_view name) const {
    std::vector<bool> visited(images_.size(), false);
    return lookup(image, name, visited, 0).address;
}

std::optional<uint64_t> Program::find_symbol_anywhere(std::string_view name) const {
    for (size_t i = 0; i < images_.size(); ++i)
        if (auto a = find_symbol(i, name)) return a;
    return std::nullopt;
}

std::vector<size_t> Program::initialization_order() const {
    std::vector<size_t> order;
    std::vector<uint8_t> seen(images_.size(), 0);
    std::function<void(size_t)> visit = [&](size_t i) {
        if (seen[i]) return;     // already scheduled, or on the current path (a back edge of a cycle)
        seen[i] = 1;
        for (int d : images_[i]->deps)
            if (d >= 0) visit(static_cast<size_t>(d));
        order.push_back(i);
    };
    if (!images_.empty()) visit(0);
    return order;
}

InitResult Program::run_initializers(const InitCaller& call) {
    InitResult res;
    const auto order = initialization_order();
    size_t total = 0;
    for (auto i : order) total += images_[i]->initializers.size();
    log::info(log::Category::Dyld, "Running initializers ({} in {} image(s))", total, order.size());
    for (size_t i : order) {
        ImageRecord& r = *images_[i];
        if (r.init_state == InitState::Done) continue;                 // exactly once
        if (r.init_state == InitState::Running) {                       // never re-enter an image whose initializers are running
            log::warn(log::Category::Dyld, "{}: recursive initialization prevented", r.leaf);
            continue;
        }
        r.init_state = InitState::Running;
        for (uint64_t fn : r.initializers) {
            log::debug(log::Category::Dyld, "{}: initializer at {:#x}", r.leaf, fn);
            InitCall c = call(fn, r);
            ++res.calls;
            if (c.exited) {
                res.status = InitResult::Status::Exited;
                res.exit_code = c.exit_code;
                res.message = std::format("{}: an initializer called exit({})", r.leaf, static_cast<int32_t>(c.exit_code));
                return res;
            }
        }
        r.init_state = InitState::Done;
    }
    return res;
}

}  // namespace cider::linker
