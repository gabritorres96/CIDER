#include <format>
#include <fstream>
#include <sstream>

#include "cider/guest.hpp"
#include "cider/log.hpp"
#include "cider/process.hpp"
#include "cider/util.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace cider::guest {
namespace {

// Private temporary directory for the result file (removed on destruction).
class ResultDir {
public:
    ResultDir() {
        std::error_code ec;
        auto base = std::filesystem::temp_directory_path(ec);
        if (ec) return;
        for (int i = 0; i < 16; ++i) {
            auto p = base / std::format("cider-run-{:08x}{:08x}", static_cast<unsigned>(GetTickCount64() & 0xffffffff) + i, static_cast<unsigned>(GetCurrentProcessId()));
            if (std::filesystem::create_directory(p, ec) && !ec) { path_ = p; return; }
        }
    }
    ~ResultDir() { std::error_code ec; if (!path_.empty()) std::filesystem::remove_all(path_, ec); }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::string describe_status(uint32_t code) {
    switch (code) {
        case 0xC0000005: return "access violation (the guest touched memory it may not access)";
        case 0xC000001D: return "illegal instruction";
        case 0xC0000096: return "privileged instruction";
        case 0xC00000FD: return "stack overflow";
        case 0xC0000094: return "integer division by zero";
        case 0xC0000409: return "fast-fail / stack buffer overrun";
        case 0xC0000374: return "heap corruption";
        case 0x80000003: return "breakpoint";
        case 0xC0000142: return "worker failed to initialise";
        default: return "";
    }
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}  // namespace

const char* to_string(SupervisorResult::Status s) noexcept {
    switch (s) {
        case SupervisorResult::Status::Completed: return "completed";
        case SupervisorResult::Status::LoadFailed: return "load-failed";
        case SupervisorResult::Status::GuestFaulted: return "guest-faulted";
        case SupervisorResult::Status::GuestCrashed: return "guest-crashed";
        case SupervisorResult::Status::Timeout: return "timeout";
        case SupervisorResult::Status::InternalError: return "internal-error";
    }
    return "?";
}

SupervisorResult run_supervised(const SupervisorOptions& opts) {
    SupervisorResult res;
    ResultDir dir;
    if (dir.path().empty()) {
        res.message = "cannot create a private directory for the worker result";
        return res;
    }
    const auto result_file = dir.path() / "result.txt";

    std::vector<std::string> args = {"__worker", "--result-file", path_to_utf8(result_file), "--log-level", opts.log_level};
    if (!opts.log_file.empty()) { args.push_back("--log-file"); args.push_back(path_to_utf8(opts.log_file)); }
    if (!opts.root.empty()) { args.push_back("--root"); args.push_back(path_to_utf8(opts.root)); }
    args.push_back(path_to_utf8(opts.image));
    if (!opts.guest_args.empty()) {
        args.push_back("--");
        args.insert(args.end(), opts.guest_args.begin(), opts.guest_args.end());
    }

    ProcessOptions po;
    po.restrict_job = true;
    po.job_memory_limit = opts.memory_limit;
    po.inherit_stdio = !opts.capture_output;
    po.separate_stderr = opts.capture_output;
    log::info(log::Category::Process, "starting worker process (timeout {} s)", std::chrono::duration_cast<std::chrono::seconds>(opts.timeout).count());
    auto pr = run_process_ex(opts.worker_exe, args, opts.timeout, po);
    if (!pr.ok()) {
        res.message = "cannot run the worker: " + pr.error().message;
        return res;
    }
    res.worker_exit_code = pr.value().exit_code_u;
    res.guest_stdout = pr.value().output;
    res.guest_stderr = pr.value().errput;
    if (!opts.log_file.empty()) res.worker_log = slurp(opts.log_file);

    if (pr.value().timed_out) {
        res.status = SupervisorResult::Status::Timeout;
        res.message = std::format("the guest did not finish within {} s and was killed", std::chrono::duration_cast<std::chrono::seconds>(opts.timeout).count());
        log::error("{}", res.message);
        return res;
    }

    std::error_code ec;
    if (std::filesystem::exists(result_file, ec)) {
        if (auto parsed = parse_result(slurp(result_file))) {
            res.guest = *parsed;
            switch (parsed->state) {
                case State::Exited: res.status = SupervisorResult::Status::Completed; break;
                case State::LoadFailed: res.status = SupervisorResult::Status::LoadFailed; res.message = parsed->message; break;
                case State::Faulted: res.status = SupervisorResult::Status::GuestFaulted; res.message = parsed->message; break;
                default: res.status = SupervisorResult::Status::InternalError; res.message = "worker reported no result"; break;
            }
            log::info(log::Category::Process, "worker finished (exit code {}), cleanup completed", res.worker_exit_code);
            return res;
        }
    }
    // No (valid) result: the guest took the worker down.
    res.status = SupervisorResult::Status::GuestCrashed;
    std::string why = describe_status(res.worker_exit_code);
    res.message = std::format("the worker process terminated without a result (exit code {:#010x}{}{})", res.worker_exit_code, why.empty() ? "" : ": ", why);
    log::error("{}", res.message);
    return res;
}

}  // namespace cider::guest
