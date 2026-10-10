// `cider run` (supervisor side) and the internal `cider __worker` (loader + guest side).
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cider/bundle.hpp"
#include "cider/guest.hpp"
#include "cider/json.hpp"
#include "cider/loader.hpp"
#include "cider/log.hpp"
#include "cider/macho.hpp"
#include "cider/process.hpp"
#include "cider/util.hpp"
#include "commands.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace cider::cli {
namespace {

using guest::SupervisorResult;

// Exit codes of `cider run` (distinct from the guest's own exit code, which is reported separately):
//   0  the guest ran to completion (its exit code is printed / in the JSON "guest" object)
//   10 internal CIDER error      11 file incompatible/unsupported     12 dependency or symbol not available
//   13 load failure              14 guest failure (crash, runtime trap, timeout)
constexpr int kOk = 0, kInternal = 10, kIncompatible = 11, kDependency = 12, kLoadFailure = 13, kGuestFailure = 14;

struct RunReport {
    std::string status;           // runtime status word
    std::string message;
    int exit_code = kInternal;
    guest::Outcome guest;
    uint32_t worker_exit_code = 0;
    std::string guest_stdout, guest_stderr;
    std::vector<std::string> log;
};

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> v;
    std::istringstream in(s);
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty()) v.push_back(l);
    }
    return v;
}

void print_json(const Options& o, const RunReport& r) {
    JsonWriter w;
    w.begin_object();
    w.field("tool", "cider");
    w.field("version", kVersion);
    w.field("command", "run");
    w.field("path", o.positional.empty() ? std::string() : o.positional[0]);
    w.field("cider_exit_code", r.exit_code);
    w.key("runtime");
    w.begin_object();
    w.field("status", r.status);
    w.field("message", r.message);
    w.field("worker_exit_code", static_cast<uint64_t>(r.worker_exit_code));
    w.field("load_failure", [&] {
        switch (r.guest.load_failure) {
            case loader::LoadFailure::None: return "none";
            case loader::LoadFailure::Incompatible: return "incompatible";
            case loader::LoadFailure::Unsupported: return "unsupported";
            case loader::LoadFailure::MissingDependency: return "missing-dependency";
            case loader::LoadFailure::UnresolvedSymbol: return "unresolved-symbol";
            case loader::LoadFailure::Internal: return "internal";
        }
        return "internal";
    }());
    w.field("entry_address", r.guest.entry_address);
    w.field("image_base", r.guest.image_base);
    w.key("required_libraries");
    w.begin_array();
    for (auto& l : r.guest.required_libs) w.value(l);
    w.end_array();
    w.field("missing_library", r.guest.missing_library);
    w.field("initializers_run", r.guest.initializers_run);
    w.key("images");
    w.begin_array();
    for (auto& i : r.guest.images) {
        w.begin_object();
        w.field("name", i.name);
        w.field("path", i.path);
        w.field("main", i.main);
        w.field("base", i.base);
        w.field("initializers", i.initializers);
        w.end_object();
    }
    w.end_array();
    w.key("unresolved_symbols");
    w.begin_array();
    for (auto& u : r.guest.unresolved) {
        w.begin_object();
        w.field("symbol", u.symbol);
        w.field("library", u.library);
        w.field("reason", u.reason);
        w.end_object();
    }
    w.end_array();
    w.end_object();
    w.key("guest");
    w.begin_object();
    w.field("state", guest::to_string(r.guest.state));
    if (r.guest.state == guest::State::Exited) {
        w.field("exit_code", r.guest.exit_code);
        w.field("exit_status", r.guest.exit_code & 0xff);
        w.field("returned_from_main", r.guest.returned_from_main);
    } else {
        w.key("exit_code"); w.null();
    }
    w.field("stdout", r.guest_stdout);
    w.field("stderr", r.guest_stderr);
    w.end_object();
    w.key("log");
    w.begin_array();
    for (auto& l : r.log) w.value(l);
    w.end_array();
    w.end_object();
    std::puts(w.str().c_str());
}

int finish(const Options& o, RunReport r) {
    if (o.json) {
        print_json(o, r);
    } else if (r.exit_code == kOk) {
        std::printf("\nGuest exit code: %d\n", r.guest.exit_code);
    } else {
        std::fprintf(stderr, "[CIDER][ERROR] Execution aborted\n");
    }
    return r.exit_code;
}

RunReport failed(int code, std::string status, std::string message) {
    RunReport r;
    r.exit_code = code;
    r.status = std::move(status);
    r.message = std::move(message);
    log::error("{}", r.message);
    return r;
}

}  // namespace

int cmd_run(const Options& o) {
    if (o.positional.empty()) {
        std::fprintf(stderr, "usage: cider run <program|App.app> [--json] [--verbose] [--timeout <seconds>] [-- args...]\n");
        return 2;
    }
    fs::path target = path_from_utf8(o.positional[0]);
    std::error_code ec;
    if (!fs::exists(target, ec)) return finish(o, failed(kIncompatible, "file-error", "path does not exist: " + o.positional[0]));
    if (fs::is_directory(target, ec)) {
        auto b = bundle::inspect(target);
        if (!b.ok() || !b.value().executable_found)
            return finish(o, failed(kIncompatible, "file-error", "not an application bundle with a main executable: " + o.positional[0]));
        target = b.value().executable_path;
        log::info(log::Category::Loader, "application bundle: running {}", path_to_utf8(target));
    }

    // ---- static analysis + validation in the supervisor, before any process is started -----------------
    auto bytes = read_file(target, macho::Limits{}.max_file_size);
    if (!bytes.ok()) return finish(o, failed(kIncompatible, "file-error", bytes.error().message));
    auto parsed = macho::parse_file(ByteView(bytes.value()));
    if (!parsed.ok()) return finish(o, failed(kIncompatible, "incompatible", "not a valid Mach-O file: " + parsed.error().message));
    const macho::Slice* sl = parsed.value().find_slice(macho::kCpuX86_64);
    if (!sl || !sl->image) {
        std::string have;
        for (auto& s : parsed.value().slices) have += (have.empty() ? "" : ", ") + macho::cpu_name(s.cputype, s.cpusubtype);
        return finish(o, failed(kIncompatible, "incompatible", "no usable x86_64 slice (found: " + have + "); CIDER 0.2 executes x86_64 only"));
    }
    loader::ValidatorOptions vopts;
    vopts.linker_resolves_dependencies = true;   // the worker's dynamic linker searches, loads and checks the dylibs
    vopts.allow_initializers = true;
    vopts.allow_tls = true;
    loader::ValidationReport vr = loader::validate_for_execution(*sl->image, vopts);
    if (!vr.ok()) {
        bool missing = false;
        std::string msg;
        for (auto& i : vr.issues) {
            missing |= i.kind == loader::Issue::Kind::MissingDependency;
            msg += (msg.empty() ? "" : "; ") + i.message;
            if (i.kind == loader::Issue::Kind::MissingDependency) log::error("{}", i.message);
        }
        RunReport r = failed(missing ? kDependency : kIncompatible, missing ? "dependency-missing" : "incompatible", msg);
        r.guest.required_libs = vr.required_libs;
        return finish(o, r);
    }

    // ---- execution in a worker process ---------------------------------------------------------------------
    guest::SupervisorOptions so;
    so.worker_exe = current_executable();
    if (so.worker_exe.empty()) return finish(o, failed(kInternal, "internal-error", "cannot locate the cider executable to start the worker"));
    so.image = target;
    so.guest_args = o.guest_args;
    // The guest's "/" is --root, or the directory cider was started in. Always an existing absolute directory.
    {
        std::error_code rec;
        fs::path root = o.root.empty() ? fs::current_path(rec) : fs::absolute(path_from_utf8(o.root), rec);
        if (rec || !fs::is_directory(root, rec)) return finish(o, failed(kIncompatible, "file-error", "--root is not an existing directory: " + (o.root.empty() ? std::string(".") : o.root)));
        so.root = root;
    }
    // A program that links AppKit is interactive: the default 60 s limit would kill it while the user works with its windows. The limit is lifted
    // (24 h) unless --timeout was given; the worker is still supervised and the job limits (memory, processes) still apply.
    bool interactive = false;
    for (const auto& d : sl->image->dylibs) interactive |= d.path.find("/AppKit.framework/") != std::string::npos;
    so.timeout = std::chrono::seconds(o.timeout_s ? o.timeout_s : (interactive ? 24 * 3600 : 60));
    if (interactive && !o.timeout_s) log::info(log::Category::Process, "GUI program (AppKit): the default timeout is disabled, use --timeout <seconds> to set one");
    so.capture_output = o.json;
    // JSON mode always collects at least INFO-level runtime messages into the "log" array.
    so.log_level = log::level_name(o.json && log::level() > log::Level::Info ? log::Level::Info : log::level());
    for (auto& c : so.log_level) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    fs::path log_tmp;
    if (o.json) {
        log_tmp = fs::temp_directory_path(ec) / ("cider-worker-" + std::to_string(GetCurrentProcessId()) + ".log");
        fs::remove(log_tmp, ec);
        so.log_file = log_tmp;
    }

    guest::SupervisorResult sr = guest::run_supervised(so);

    RunReport r;
    r.guest = sr.guest;
    r.message = sr.message;
    r.worker_exit_code = sr.worker_exit_code;
    r.guest_stdout = sr.guest_stdout;
    r.guest_stderr = sr.guest_stderr;
    if (o.json) {
        r.log = split_lines(sr.worker_log);
        fs::remove(log_tmp, ec);
    }
    r.status = guest::to_string(sr.status);
    switch (sr.status) {
        case SupervisorResult::Status::Completed: r.exit_code = kOk; break;
        case SupervisorResult::Status::LoadFailed:
            for (auto& u : sr.guest.unresolved) {
                log::error("Unresolved symbol: {}", u.symbol);
                log::error("Required library: {}", u.library);
            }
            switch (sr.guest.load_failure) {
                case loader::LoadFailure::MissingDependency:
                case loader::LoadFailure::UnresolvedSymbol: r.exit_code = kDependency; break;
                case loader::LoadFailure::Incompatible:
                case loader::LoadFailure::Unsupported: r.exit_code = kIncompatible; break;
                default: r.exit_code = kLoadFailure; break;
            }
            if (sr.guest.unresolved.empty()) log::error("{}", sr.message);
            break;
        case SupervisorResult::Status::GuestFaulted:
        case SupervisorResult::Status::GuestCrashed:
        case SupervisorResult::Status::Timeout: r.exit_code = kGuestFailure; break;
        case SupervisorResult::Status::InternalError: r.exit_code = kInternal; log::error("{}", sr.message); break;
    }
    return finish(o, r);
}

int cmd_worker(const Options& o) {
    if (o.positional.empty()) return 2;
    return guest::worker_main(path_from_utf8(o.positional[0]), o.guest_args, o.result_file.empty() ? fs::path() : path_from_utf8(o.result_file),
                              o.root.empty() ? fs::path() : path_from_utf8(o.root));
}

}  // namespace cider::cli
