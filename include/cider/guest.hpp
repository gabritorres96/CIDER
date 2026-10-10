#pragma once
// Guest process management (CIDER 0.2): in-process execution of a loaded Mach-O image, the worker
// result protocol, and the supervisor that runs the worker in a separate, job-restricted Windows process.
//
// A "worker" is NOT a sandbox: it isolates CIDER's main process from crashes of the guest and applies
// a few Windows Job Object limits (one process, memory cap, UI restrictions, kill-on-close, timeout).
// Guest machine code still runs with the full rights of the user.
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "cider/loader.hpp"
#include "cider/result.hpp"

namespace cider::guest {

struct GuestConfig {
    std::vector<std::string> args;   // argv[0..]; args[0] is the program path as the user gave it
    std::vector<std::string> env;    // "NAME=value"
    std::function<void(int fd, const char* data, size_t size)> sink;  // default: real stdout/stderr
    std::filesystem::path image_path;   // where the executable lives: base of @executable_path and @rpath searches
    std::filesystem::path root;         // host directory that the guest sees as "/" (empty: the guest cannot open files)
    bool reject_cycles = false;         // refuse dependency cycles instead of tolerating them (dyld tolerates them)
};

// One image the dynamic linker loaded (reported in the outcome and in `cider run --json`).
struct LoadedImageInfo {
    std::string name;                 // install name, or the executable's file name
    std::string path;
    uint64_t base = 0;                // address of the Mach-O header
    uint64_t initializers = 0;        // Mach-O initializers it contributed
    bool main = false;
};

enum class State {
    NotRun,
    LoadFailed,   // the image was rejected before any guest instruction ran
    Exited,       // guest called exit() or main() returned: exit_code is valid
    Faulted,      // runtime trap inside CIDER's libSystem (e.g. lazy binding), exit_code is not meaningful
};
const char* to_string(State s) noexcept;

struct Outcome {
    State state = State::NotRun;
    int32_t exit_code = 0;                 // value passed to exit() / returned by main
    std::string message;                   // human readable error (LoadFailed / Faulted)
    loader::LoadFailure load_failure = loader::LoadFailure::None;
    std::vector<std::string> required_libs;
    std::vector<dyld::Unresolved> unresolved;
    uint64_t entry_address = 0;            // where control was transferred (diagnostics)
    uint64_t image_base = 0;
    bool returned_from_main = false;       // false: exit() was called explicitly
    std::vector<LoadedImageInfo> images;   // main executable first, then libraries in load order
    std::string missing_library;           // LoadFailed / MissingDependency: the install name that was not found
    uint64_t initializers_run = 0;
    uint64_t objc_live_objects = 0;        // Objective-C objects still alive when the guest ended (leak check; in-process only)
};

// Loads and runs `file` in THIS process, then releases everything (image, stack, thunks).
// Guest crashes (access violation...) kill the calling process: use the supervisor for untrusted code.
Outcome run_in_process(ByteView file, const GuestConfig& cfg, const loader::ValidatorOptions& opts = {});

// ---- worker protocol (process/exit_handler.cpp) -------------------------------------------------
std::string serialize_result(const Outcome& r);                       // line-based key=value text
std::optional<Outcome> parse_result(const std::string& text);         // nullopt if malformed
bool write_result_file(const std::filesystem::path& p, const Outcome& r);

// ---- supervisor (process/supervisor.cpp) -----------------------------------------------------------
struct SupervisorOptions {
    std::filesystem::path worker_exe;     // normally current_executable()
    std::filesystem::path image;          // Mach-O file to execute
    std::vector<std::string> guest_args;  // argv[1..]
    std::chrono::milliseconds timeout = std::chrono::seconds(60);
    bool capture_output = false;          // capture guest stdout/stderr (JSON mode) instead of inheriting
    std::string log_level = "info";
    std::filesystem::path log_file;       // worker log file (empty: worker logs to stderr)
    uint64_t memory_limit = 4ull << 30;   // per-process committed memory limit
    std::filesystem::path root;           // guest file root (`cider run --root`); empty: guest file access is refused
};

struct SupervisorResult {
    enum class Status {
        Completed,       // guest ran to completion (see `guest.exit_code`)
        LoadFailed,      // the worker refused the image (see guest.load_failure)
        GuestFaulted,    // CIDER's libSystem trapped (e.g. lazy binding); the worker survived
        GuestCrashed,    // worker died without reporting (access violation, ...)
        Timeout,         // killed after the timeout
        InternalError,   // CIDER failed to start/talk to the worker
    } status = Status::InternalError;
    Outcome guest;
    std::string message;
    uint32_t worker_exit_code = 0;
    std::string guest_stdout, guest_stderr;  // only when capture_output
    std::string worker_log;                  // contents of log_file when set
};
const char* to_string(SupervisorResult::Status s) noexcept;

SupervisorResult run_supervised(const SupervisorOptions& opts);

// Entry point of `cider __worker`: loads the image, runs it, writes the result file. Returns the worker
// process exit code (0 when a result was written, even if the guest failed).
int worker_main(const std::filesystem::path& image, const std::vector<std::string>& guest_args,
                const std::filesystem::path& result_file, const std::filesystem::path& root = {});

}  // namespace cider::guest
