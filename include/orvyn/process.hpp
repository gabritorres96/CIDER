#pragma once
// Safe external-process runner: used for the optional 7-Zip backend and for the ORVYN 0.2 worker process.
// The child gets no stdin, output is captured with a size cap (or inherited), and it is killed on timeout
// or when ORVYN exits (Windows Job Object).
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "orvyn/result.hpp"

namespace orvyn {

struct ProcessResult {
    int exit_code = -1;     // Windows: the process exit code / NTSTATUS (e.g. 0xC0000005 as a negative int)
    uint32_t exit_code_u = 0;
    std::string output;     // stdout (merged with stderr unless ProcessOptions::separate_stderr)
    std::string errput;     // stderr when separate_stderr
    bool timed_out = false;
};

struct ProcessOptions {
    size_t max_output_bytes = 64u << 20;
    bool inherit_stdio = false;      // child writes directly to our stdout/stderr (nothing is captured)
    bool inherit_stdin = false;      // child reads OUR stdin (default: the NUL device, so a guest never reads the terminal by accident)
    bool separate_stderr = false;    // capture stderr separately instead of merging it into `output`
    bool restrict_job = false;       // job limits: one process, UI restrictions, no WER dialog on crash
    uint64_t job_memory_limit = 0;   // per-process committed memory limit in bytes (0 = none), needs restrict_job
};

// `exe` must be an absolute path (no PATH or working-directory search is performed).
Result<ProcessResult> run_process(const std::filesystem::path& exe, const std::vector<std::string>& utf8_args,
                                  std::chrono::milliseconds timeout, size_t max_output_bytes = 64u << 20);
Result<ProcessResult> run_process_ex(const std::filesystem::path& exe, const std::vector<std::string>& utf8_args,
                                     std::chrono::milliseconds timeout, const ProcessOptions& opts);

// Windows command-line quoting for one argument (exposed for tests).
std::string quote_windows_arg(const std::string& arg);

// Path of the running executable (empty on failure).
std::filesystem::path current_executable();

}  // namespace orvyn
