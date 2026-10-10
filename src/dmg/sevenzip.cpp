#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <map>
#include <random>
#include <set>
#include <sstream>

#include "cider/bytes.hpp"
#include "cider/dmg.hpp"
#include "cider/log.hpp"
#include "cider/process.hpp"
#include "cider/util.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace cider::dmg {
namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

std::string tail_lines(const std::string& out, size_t n = 6) {
    std::vector<std::string> lines;
    std::istringstream ss(out);
    for (std::string l; std::getline(ss, l);) {
        l = trim(l);
        if (!l.empty()) lines.push_back(l);
    }
    std::string r;
    for (size_t i = lines.size() > n ? lines.size() - n : 0; i < lines.size(); ++i) r += (r.empty() ? "" : " | ") + lines[i];
    return sanitize_utf8(r);
}

Result<Backend> probe_candidate(const fs::path& exe) {
    auto r = run_process(exe, {}, std::chrono::seconds(15), 1 << 20);
    if (!r.ok()) return r.error();
    const std::string& out = r.value().output;
    auto pos = out.find("7-Zip");
    if (pos == std::string::npos) return Error{ErrorCode::NotFound, path_to_utf8(exe) + " does not look like 7-Zip"};
    Backend be;
    be.exe = exe;
    auto eol = out.find('\n', pos);
    be.banner = trim(out.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos));
    // first "<digits>.<digits>" after the product name
    for (size_t i = pos + 5; i + 2 < out.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(out[i])) && (i == 0 || !std::isdigit(static_cast<unsigned char>(out[i - 1])))) {
            size_t j = i;
            while (j < out.size() && std::isdigit(static_cast<unsigned char>(out[j]))) ++j;
            if (j < out.size() && out[j] == '.' && j + 1 < out.size() && std::isdigit(static_cast<unsigned char>(out[j + 1]))) {
                be.major = std::atoi(out.substr(i, j - i).c_str());
                be.minor = std::atoi(out.c_str() + j + 1);
                break;
            }
            i = j;
        }
    }
    return be;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool reserved_device_name(const std::string& comp) {
    std::string stem = lower(comp.substr(0, comp.find('.')));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    static const std::set<std::string> names = {"con", "prn", "aux", "nul", "conin$", "conout$"};
    if (names.count(stem)) return true;
    if (stem.size() == 4 && (stem.rfind("com", 0) == 0 || stem.rfind("lpt", 0) == 0) && stem[3] >= '0' && stem[3] <= '9') return true;
    return false;
}

std::string sanitize_component(std::string s) {
    for (char& c : s)
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*' || static_cast<unsigned char>(c) < 0x20) c = '_';
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    if (s.empty() || reserved_device_name(s)) s = "_" + s;
    return s;
}

bool is_link(const fs::path& p, const fs::file_status& sym) {
    if (fs::is_symlink(sym)) return true;
#ifdef _WIN32
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    (void)p;
    return false;
#endif
}

}  // namespace

// Walks a freshly extracted tree and enforces the post-conditions: links are removed (never
// followed), nothing outside the root, limits respected.
Result<TreeStats> verify_extracted_tree(const fs::path& root, const ExtractLimits& lim, std::vector<std::string>* notes) {
    TreeStats st;
    std::vector<fs::path> links;
    std::error_code ec;
    // Pass 1: inspect only (links are neither followed nor modified while the iterator is live).
    for (fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path p = it->path();
        std::error_code e2;
        auto sym = fs::symlink_status(p, e2);
        if (e2) return Error{ErrorCode::Io, "cannot stat extracted entry: " + e2.message()};
        if (is_link(p, sym)) {
            it.disable_recursion_pending();
            links.push_back(p);
            continue;
        }
        if (!is_inside(root, p)) return Error{ErrorCode::Malformed, "extracted entry escapes the output directory: " + path_to_utf8(p)};
        if (fs::is_regular_file(sym)) {
            ++st.files;
            st.bytes += fs::file_size(p, e2);
            if (st.files > lim.max_files) return Error{ErrorCode::LimitExceeded, "extracted file count exceeds the limit"};
            if (st.bytes > lim.max_total_bytes) return Error{ErrorCode::LimitExceeded, "extracted size exceeds the limit"};
        }
    }
    if (ec) return Error{ErrorCode::Io, "cannot walk extracted tree: " + ec.message()};
    // Pass 2: real images always carry e.g. an "Applications" alias pointing at /Applications. A link
    // could point anywhere on this machine, so each one is deleted (the link itself, never its target).
    for (const auto& p : links) {
        std::string rel = path_to_utf8(p.lexically_relative(root));
        std::error_code e3;
        fs::remove(p, e3);
        if (e3) return Error{ErrorCode::Malformed, "extraction produced a link that could not be removed: " + sanitize_utf8(rel)};
        ++st.links_removed;
        if (notes) notes->push_back("removed link/reparse point from the extracted tree: " + sanitize_utf8(rel));
    }
    return st;
}

namespace {

Status extract_one(const Backend& be, const fs::path& archive, const fs::path& out_dir, std::chrono::milliseconds timeout,
                   const std::string& type_switch = {}) {
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) return Error{ErrorCode::Io, "cannot create " + path_to_utf8(out_dir) + ": " + ec.message()};
    std::vector<std::string> args = {"x", "-y", "-bd", "-sccUTF-8", "-scsUTF-8", "-o" + path_to_utf8(out_dir)};
    if (!type_switch.empty()) args.push_back("-t" + type_switch);  // e.g. "-tdmg": do not auto-expand nested archives
    args.push_back("--");
    args.push_back(path_to_utf8(archive));
    log::info("7-Zip: extracting {}", path_to_utf8(archive.filename()));
    auto r = run_process(be.exe, args, timeout);
    if (!r.ok()) return r.error();
    if (r.value().timed_out) return Error{ErrorCode::LimitExceeded, "7-Zip timed out while extracting " + path_to_utf8(archive.filename())};
    if (r.value().exit_code >= 2)
        return Error{ErrorCode::Malformed, std::format("7-Zip failed (exit {}): {}", r.value().exit_code, tail_lines(r.value().output))};
    if (r.value().exit_code == 1) log::warn("7-Zip reported warnings: {}", tail_lines(r.value().output));
    return {};
}

}  // namespace

// ---- backend -----------------------------------------------------------------------------
Result<Backend> locate_7zip(const fs::path& explicit_path) {
    std::vector<fs::path> candidates;
    if (!explicit_path.empty()) {
        if (!explicit_path.is_absolute()) return Error{ErrorCode::InvalidArgument, "the 7-Zip path must be absolute"};
        if (!is_file(explicit_path)) return Error{ErrorCode::NotFound, "7-Zip not found at " + path_to_utf8(explicit_path)};
        return probe_candidate(explicit_path);
    }
    if (const char* env = std::getenv("CIDER_7Z"); env && *env) {
        fs::path p = path_from_utf8(env);
        if (p.is_absolute() && is_file(p)) candidates.push_back(p);
    }
    if (const char* path_env = std::getenv("PATH")) {
#ifdef _WIN32
        const char sep = ';';
        const char* names[] = {"7z.exe", "7zz.exe"};
#else
        const char sep = ':';
        const char* names[] = {"7zz", "7z"};
#endif
        std::string pe = path_env;
        size_t start = 0;
        while (start <= pe.size()) {
            size_t e = pe.find(sep, start);
            std::string dir = pe.substr(start, e == std::string::npos ? std::string::npos : e - start);
            start = e == std::string::npos ? pe.size() + 1 : e + 1;
            if (dir.empty()) continue;
            fs::path d = path_from_utf8(dir);
            if (!d.is_absolute()) continue;  // never search the current directory / relative entries
            for (auto n : names)
                if (is_file(d / n)) candidates.push_back(d / n);
        }
    }
#ifdef _WIN32
    for (const char* env : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"})
        if (const char* pf = std::getenv(env)) candidates.push_back(fs::path(pf) / "7-Zip" / "7z.exe");
#endif
    std::set<std::string> seen;
    std::string last_error = "7-Zip was not found (set CIDER_7Z, use --7z <path>, or install it from https://www.7-zip.org/)";
    for (auto& c : candidates) {
        if (!is_file(c) || !seen.insert(lower(path_to_utf8(c))).second) continue;
        auto be = probe_candidate(c);
        if (be.ok()) return be;
        last_error = be.error().message;
    }
    return Error{ErrorCode::NotFound, last_error};
}

// ---- listing -----------------------------------------------------------------------------
Status validate_item_path(const std::string& raw) {
    if (raw.empty()) return Error{ErrorCode::Malformed, "empty path in archive listing"};
    if (raw.find('\0') != std::string::npos) return Error{ErrorCode::Malformed, "NUL in archive path"};
    std::string p = raw;
    std::replace(p.begin(), p.end(), '\\', '/');
    if (p[0] == '/') return Error{ErrorCode::Malformed, "absolute path in archive: " + sanitize_utf8(raw)};
    if (p.size() >= 2 && p[1] == ':') return Error{ErrorCode::Malformed, "drive-letter path in archive: " + sanitize_utf8(raw)};
    size_t start = 0;
    while (start <= p.size()) {
        size_t e = p.find('/', start);
        std::string comp = p.substr(start, e == std::string::npos ? std::string::npos : e - start);
        start = e == std::string::npos ? p.size() + 1 : e + 1;
        if (comp.empty() || comp == ".") continue;
        std::string trimmed = comp;
        while (!trimmed.empty() && (trimmed.back() == '.' || trimmed.back() == ' ')) trimmed.pop_back();
        if (comp == ".." || trimmed.empty())  // Windows strips trailing dots/spaces: "... " can become ".."
            return Error{ErrorCode::Malformed, "path traversal in archive: " + sanitize_utf8(raw)};
        if (comp.find(':') != std::string::npos) return Error{ErrorCode::Malformed, "alternate data stream / drive reference in archive path: " + sanitize_utf8(raw)};
        if (reserved_device_name(comp)) return Error{ErrorCode::Malformed, "reserved device name in archive path: " + sanitize_utf8(raw)};
    }
    return {};
}

Status validate_listing(const std::vector<ArchiveItem>& items, const ExtractLimits& lim) {
    if (items.size() > lim.max_files) return Error{ErrorCode::LimitExceeded, std::format("archive lists {} entries (limit {})", items.size(), lim.max_files)};
    uint64_t total = 0;
    for (auto& it : items) {
        CIDER_CHECK(validate_item_path(it.path));
        if (it.encrypted) return Error{ErrorCode::Unsupported, "archive contains encrypted entries (password-protected): " + sanitize_utf8(it.path)};
        auto sum = checked_add(total, it.size);
        if (!sum || *sum > lim.max_total_bytes)
            return Error{ErrorCode::LimitExceeded, std::format("archive would expand beyond the size limit of {} bytes", lim.max_total_bytes)};
        total = *sum;
    }
    return {};
}

Result<Listing> list_archive(const Backend& be, const fs::path& archive, std::chrono::milliseconds timeout, const std::string& type_switch) {
    std::vector<std::string> args = {"l", "-slt", "-bd", "-y", "-sccUTF-8"};
    if (!type_switch.empty()) args.push_back("-t" + type_switch);
    args.push_back("--");
    args.push_back(path_to_utf8(archive));
    auto r = run_process(be.exe, args, timeout);
    if (!r.ok()) return r.error();
    if (r.value().timed_out) return Error{ErrorCode::LimitExceeded, "7-Zip timed out while listing"};
    const std::string& out = r.value().output;
    if (r.value().exit_code >= 2)
        return Error{ErrorCode::Malformed, std::format("7-Zip cannot open this archive (exit {}): {}", r.value().exit_code, tail_lines(out))};

    Listing l;
    bool in_items = false;
    ArchiveItem cur;
    bool have = false;
    auto flush = [&] {
        if (have && !cur.path.empty()) {
            std::replace(cur.path.begin(), cur.path.end(), '\\', '/');
            l.total_size += cur.is_dir ? 0 : cur.size;
            l.items.push_back(cur);
        }
        cur = {};
        have = false;
    };
    std::istringstream ss(out);
    for (std::string line; std::getline(ss, line);) {
        line = trim(line);
        if (line.size() >= 4 && line.find_first_not_of('-') == std::string::npos) { in_items = true; continue; }
        if (line.empty()) { if (in_items) flush(); continue; }
        auto eq = line.find(" = ");
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), val = line.substr(eq + 3);
        if (!in_items) {
            if (key == "Type" && l.type.empty()) l.type = val;
            continue;
        }
        if (key == "Path") { flush(); cur.path = val; have = true; }
        else if (key == "Folder") cur.is_dir = val == "+";
        else if (key == "Size") cur.size = std::strtoull(val.c_str(), nullptr, 10);
        else if (key == "Encrypted") cur.encrypted = val == "+";
    }
    flush();
    return l;
}

// ---- directories -------------------------------------------------------------------------
TempDir::TempDir(const fs::path& base) {
    std::error_code ec;
    fs::path b = base.empty() ? fs::temp_directory_path(ec) : base;
    if (ec) return;
    std::random_device rd;
    std::mt19937_64 rng((static_cast<uint64_t>(rd()) << 32) ^ rd());
    for (int attempt = 0; attempt < 16; ++attempt) {
        fs::path p = b / std::format("cider-{:016x}", rng());
        if (fs::create_directory(p, ec) && !ec) { path_ = p; return; }
    }
}

bool TempDir::remove() {
    if (path_.empty()) return true;
    std::error_code ec;
    fs::remove_all(path_, ec);
    if (fs::exists(path_, ec)) {
        // clear read-only attributes (extracted files may carry them) and retry once
        for (auto& e : fs::recursive_directory_iterator(path_, fs::directory_options::skip_permission_denied, ec)) {
            std::error_code e2;
            fs::permissions(e.path(), fs::perms::owner_write, fs::perm_options::add, e2);
        }
        fs::remove_all(path_, ec);
    }
    bool gone = !fs::exists(path_, ec);
    if (gone) path_.clear();
    else log::warn("could not remove temporary directory {}", path_to_utf8(path_));
    return gone;
}

TempDir::~TempDir() { remove(); }

std::vector<std::string> find_apps(const fs::path& root, size_t max_depth) {
    std::vector<std::string> apps;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (it.depth() > static_cast<int>(max_depth)) { it.disable_recursion_pending(); continue; }
        std::error_code e2;
        if (!it->is_directory(e2) || it->is_symlink(e2)) continue;
        std::string name = path_to_utf8(it->path().filename());
        if (ends_with_icase(name, ".app")) {
            apps.push_back(path_to_utf8(it->path().lexically_relative(root)));
            it.disable_recursion_pending();
        }
    }
    std::sort(apps.begin(), apps.end());
    return apps;
}

// ---- extraction --------------------------------------------------------------------------
Result<ExtractReport> extract_image(const Backend& be, const fs::path& image, const fs::path& output_in, const ExtractOptions& opts) {
    std::error_code ec;
    // Real images nest deep (Contents/Frameworks/X.framework/Versions/A/...): use extended-length paths for
    // every filesystem operation of ours (7-Zip receives the normal form: path_to_utf8 strips the prefix).
    const fs::path output = extended_path(output_in);
    if (fs::exists(output, ec)) {
        if (!fs::is_directory(output, ec)) return Error{ErrorCode::InvalidArgument, "output exists and is not a directory: " + path_to_utf8(output)};
        if (!fs::is_empty(output, ec)) return Error{ErrorCode::InvalidArgument, "output directory is not empty: " + path_to_utf8(output)};
    }
    const bool created_output = !fs::exists(output, ec);
    auto cleanup_output = [&] {
        if (created_output) fs::remove_all(output, ec);
        else for (auto& e : fs::directory_iterator(output, ec)) fs::remove_all(e.path(), ec);
    };

    TempDir work(opts.temp_base);
    if (!work.ok()) return Error{ErrorCode::Io, "cannot create a private temporary directory"};
    log::info("Extraction work directory created");

    auto fail = [&](Error e) -> Result<ExtractReport> {
        cleanup_output();
        return e;
    };

    // The auto-detecting listing may also include the entries of nested filesystems: use it to vet
    // every path up front. Sizes are checked on the per-stage listings below (no double counting).
    auto l1 = list_archive(be, image, opts.timeout);
    if (!l1.ok()) return fail(l1.error());
    if (l1.value().items.empty()) return fail(Error{ErrorCode::Unsupported, "7-Zip sees no content in this image"});
    {
        ExtractLimits paths_only = opts.limits;
        paths_only.max_total_bytes = UINT64_MAX;
        if (auto s = validate_listing(l1.value().items, paths_only); !s.ok()) return fail(s.error());
    }
    const bool is_udif = lower(l1.value().type) == "dmg";
    if (is_udif) {
        auto outer = list_archive(be, image, opts.timeout, "dmg");
        if (!outer.ok()) return fail(outer.error());
        if (auto s = validate_listing(outer.value().items, opts.limits); !s.ok()) return fail(s.error());
    } else if (auto s = validate_listing(l1.value().items, opts.limits); !s.ok()) {
        return fail(s.error());
    }

    ExtractReport rep;
    std::vector<std::string> link_notes;
    rep.output = output;
    fs::create_directories(output, ec);

    // Stage 1: a UDIF container is unpacked into one raw file per partition. -tdmg keeps 7-Zip from
    // also expanding the filesystems inside, so every size is validated before it is written.
    // A non-UDIF image (raw ISO/HFS/APFS...) is its own single partition.
    std::vector<fs::path> files;
    if (is_udif) {
        fs::path stage1 = extended_path(work.path()) / "stage1";
        if (auto s = extract_one(be, image, stage1, opts.timeout, "dmg"); !s.ok()) return fail(s.error());
        if (auto v = verify_extracted_tree(stage1, opts.limits, &link_notes); !v.ok()) return fail(v.error());
        for (auto& e : fs::recursive_directory_iterator(stage1, ec))
            if (e.is_regular_file(ec)) files.push_back(e.path());
        std::sort(files.begin(), files.end());
    } else {
        files.push_back(image);
    }

    std::set<std::string> used_names;
    for (auto& f : files) {
        const bool is_image_itself = f == image;
        std::string pname = path_to_utf8(f.filename());
        Result<Listing> l2 = is_image_itself ? l1 : list_archive(be, f, opts.timeout);
        if (!l2.ok() || l2.value().type.empty() || l2.value().items.empty()) {
            std::error_code se;
            const uint64_t psize = is_image_itself ? fs::file_size(f, se) : fs::file_size(f, se);
            // Small leftovers (GPT headers/tables, free space) are expected in real images and carry no
            // filesystem; only a large unrecognised partition is worth a warning.
            if (se || psize >= (1u << 20))
                rep.warnings.push_back("partition '" + sanitize_utf8(pname) + "': no filesystem recognised by 7-Zip" +
                                       (l2.ok() ? "" : " (" + tail_lines(l2.error().message, 2) + ")"));
            else
                log::info("ignoring small non-filesystem partition '{}'", pname);
            continue;
        }
        // Partition-table metadata ("0.MBR", "1.Primary GPT Header", ...) is not a filesystem: 7-Zip
        // would try to open it as a partition scheme and fail on the truncated sector (seen with
        // real hdiutil images), so it is skipped, not extracted.
        if (const std::string t = lower(l2.value().type); t == "mbr" || t == "gpt" || t == "apm") {
            log::info("skipping partition-scheme metadata '{}' (7-Zip type {})", path_to_utf8(f.filename()), l2.value().type);
            continue;
        }
        if (auto s = validate_listing(l2.value().items, opts.limits); !s.ok()) return fail(s.error());
        std::string dirname = sanitize_component(path_to_utf8(f.stem()));
        const std::string base = dirname;
        for (int n = 2; !used_names.insert(lower(dirname)).second; ++n) dirname = base + "_" + std::to_string(n);
        fs::path dest = output / path_from_utf8(dirname);
        if (auto s = extract_one(be, f, dest, opts.timeout); !s.ok()) return fail(s.error());
        auto v = verify_extracted_tree(dest, opts.limits, &link_notes);
        if (!v.ok()) return fail(v.error());
        rep.partitions.push_back({dirname, l2.value().type, v.value().files, v.value().bytes});
        rep.total_files += v.value().files;
        rep.total_bytes += v.value().bytes;
        if (rep.total_bytes > opts.limits.max_total_bytes) return fail(Error{ErrorCode::LimitExceeded, "total extracted size exceeds the limit"});
        if (!is_image_itself) fs::remove(f, ec);  // free the intermediate partition image early
    }
    if (rep.partitions.empty())
        return fail(Error{ErrorCode::Unsupported, "no supported filesystem could be extracted (7-Zip " + std::to_string(be.major) + "." +
                                                       std::to_string(be.minor) + " opened the image but could not read its partitions" +
                                                       (rep.warnings.empty() ? ")" : "; " + rep.warnings.front() + ")")});
    if (!link_notes.empty()) {
        rep.links_removed = link_notes.size();
        std::string msg = std::to_string(link_notes.size()) + " symbolic link(s) in the image were not recreated (e.g. the /Applications alias, framework Versions/Current)";
        for (size_t i = 0; i < link_notes.size() && i < 3; ++i) msg += (i ? "; " : ": ") + link_notes[i].substr(link_notes[i].find(':') == std::string::npos ? 0 : link_notes[i].find(':') + 2);
        rep.warnings.push_back(msg);
    }
    rep.apps = find_apps(output);
    log::info("Extracted {} partition(s), {} file(s), {} app bundle(s)", rep.partitions.size(), rep.total_files, rep.apps.size());
    return rep;
}

}  // namespace cider::dmg
