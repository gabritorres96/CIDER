#pragma once
// Minimal, safe external-process runner (used only for the optional 7-Zip backend).
// The child gets no stdin, merged stdout+stderr is captured with a size cap, and it is killed
// on timeout or when CIDER exits (Windows Job Object).
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "cider/result.hpp"

namespace cider {

struct ProcessResult {
    int exit_code = -1;
    std::string output;     // raw bytes, merged stdout+stderr
    bool timed_out = false;
};

// `exe` must be an absolute path (no PATH or working-directory search is performed).
Result<ProcessResult> run_process(const std::filesystem::path& exe, const std::vector<std::string>& utf8_args,
                                  std::chrono::milliseconds timeout, size_t max_output_bytes = 64u << 20);

// Windows command-line quoting for one argument (exposed for tests).
std::string quote_windows_arg(const std::string& arg);

}  // namespace cider
