#include "cider/process.hpp"

#include <format>

#include "cider/util.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace cider {

std::string quote_windows_arg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') { ++backslashes; continue; }
        if (c == '"') { out.append(backslashes * 2 + 1, '\\'); out.push_back('"'); }
        else { out.append(backslashes, '\\'); out.push_back(c); }
        backslashes = 0;
    }
    out.append(backslashes * 2, '\\');  // trailing backslashes must be doubled before the closing quote
    out.push_back('"');
    return out;
}

#ifdef _WIN32
namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
struct Handle {
    HANDLE h = nullptr;
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
}  // namespace

Result<ProcessResult> run_process(const std::filesystem::path& exe, const std::vector<std::string>& args,
                                  std::chrono::milliseconds timeout, size_t max_output) {
    if (!exe.is_absolute()) return Error{ErrorCode::InvalidArgument, "process path must be absolute: " + path_to_utf8(exe)};

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    Handle read_end, write_end, nul_in;
    if (!CreatePipe(&read_end.h, &write_end.h, &sa, 0)) return Error{ErrorCode::Io, "CreatePipe failed"};
    SetHandleInformation(read_end.h, HANDLE_FLAG_INHERIT, 0);
    nul_in.h = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    // Native separators: cmd.exe mis-parses a forward-slash program name as a switch.
    std::filesystem::path native = exe;
    native.make_preferred();
    auto u8 = native.u8string();
    std::string cmd = quote_windows_arg(std::string(reinterpret_cast<const char*>(u8.data()), u8.size()));
    for (auto& a : args) cmd += " " + quote_windows_arg(a);
    std::wstring wcmd = widen(cmd);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul_in.h;
    si.hStdOutput = write_end.h;
    si.hStdError = write_end.h;

    Handle job;
    job.h = CreateJobObjectW(nullptr, nullptr);
    if (job.h) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &li, sizeof li);
    }

    PROCESS_INFORMATION pi{};
    // Application name is given explicitly: no PATH / current-directory search.
    if (!CreateProcessW(exe.c_str(), wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                        nullptr, &si, &pi))
        return Error{ErrorCode::Io, std::format("cannot start {} (error {})", path_to_utf8(exe), GetLastError())};
    Handle process, thread;
    process.h = pi.hProcess;
    thread.h = pi.hThread;
    if (job.h) AssignProcessToJobObject(job.h, pi.hProcess);
    ResumeThread(pi.hThread);
    write_end.h = (CloseHandle(write_end.h), nullptr);  // child owns the only write end now

    ProcessResult res;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buf[8192];
    bool overflow = false;
    for (;;) {
        DWORD avail = 0;
        bool pipe_open = PeekNamedPipe(read_end.h, nullptr, 0, nullptr, &avail, nullptr) != 0;
        if (pipe_open && avail) {
            DWORD got = 0;
            if (ReadFile(read_end.h, buf, static_cast<DWORD>(sizeof buf), &got, nullptr) && got) {
                if (res.output.size() + got > max_output) { overflow = true; break; }
                res.output.append(buf, got);
            }
            continue;
        }
        if (!pipe_open) break;  // writer closed and drained
        if (WaitForSingleObject(process.h, 0) == WAIT_OBJECT_0) {
            // process exited: drain whatever is left, then stop
            DWORD left = 0;
            if (PeekNamedPipe(read_end.h, nullptr, 0, nullptr, &left, nullptr) && left) continue;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) { res.timed_out = true; break; }
        Sleep(10);
    }
    if (res.timed_out || overflow) {
        TerminateProcess(process.h, 1);
        if (job.h) TerminateJobObject(job.h, 1);
    }
    WaitForSingleObject(process.h, 5000);
    DWORD code = 0;
    GetExitCodeProcess(process.h, &code);
    res.exit_code = static_cast<int>(code);
    if (overflow) return Error{ErrorCode::LimitExceeded, "process output exceeded the capture limit"};
    return res;
}
#else
Result<ProcessResult> run_process(const std::filesystem::path&, const std::vector<std::string>&, std::chrono::milliseconds, size_t) {
    return Error{ErrorCode::Unsupported, "external process execution is only implemented on Windows"};
}
#endif

}  // namespace cider
