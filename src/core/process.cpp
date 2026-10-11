#include "orvyn/process.hpp"

#include <format>

#include "orvyn/util.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace orvyn {

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
    void close() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); h = nullptr; }
};

// Drains whatever is available on `pipe` into `out`. Returns false when the pipe is closed and empty.
bool drain(HANDLE pipe, std::string& out, size_t cap, bool& overflow) {
    char buf[8192];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) return false;  // broken pipe: writer gone
        if (avail == 0) return true;
        DWORD got = 0;
        if (!ReadFile(pipe, buf, static_cast<DWORD>(sizeof buf), &got, nullptr) || got == 0) return false;
        if (out.size() + got > cap) { overflow = true; return true; }
        out.append(buf, got);
    }
}

void inheritable(HANDLE h) {
    if (h && h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
}
}  // namespace

std::filesystem::path current_executable() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    return std::filesystem::path(buf);
}

Result<ProcessResult> run_process_ex(const std::filesystem::path& exe, const std::vector<std::string>& args,
                                     std::chrono::milliseconds timeout, const ProcessOptions& opts) {
    if (!exe.is_absolute()) return Error{ErrorCode::InvalidArgument, "process path must be absolute: " + path_to_utf8(exe)};

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    Handle out_r, out_w, err_r, err_w, nul_in;
    nul_in.h = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul_in.h;
    if (opts.inherit_stdin) {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        if (in && in != INVALID_HANDLE_VALUE) { inheritable(in); si.hStdInput = in; }
    }
    if (opts.inherit_stdio) {
        HANDLE so = GetStdHandle(STD_OUTPUT_HANDLE), se = GetStdHandle(STD_ERROR_HANDLE);
        inheritable(so);
        inheritable(se);
        si.hStdOutput = so;
        si.hStdError = se;
    } else {
        if (!CreatePipe(&out_r.h, &out_w.h, &sa, 0)) return Error{ErrorCode::Io, "CreatePipe failed"};
        SetHandleInformation(out_r.h, HANDLE_FLAG_INHERIT, 0);
        si.hStdOutput = out_w.h;
        si.hStdError = out_w.h;
        if (opts.separate_stderr) {
            if (!CreatePipe(&err_r.h, &err_w.h, &sa, 0)) return Error{ErrorCode::Io, "CreatePipe failed"};
            SetHandleInformation(err_r.h, HANDLE_FLAG_INHERIT, 0);
            si.hStdError = err_w.h;
        }
    }

    // Native separators: cmd.exe mis-parses a forward-slash program name as a switch.
    std::filesystem::path native = exe;
    native.make_preferred();
    auto u8 = native.u8string();
    std::string cmd = quote_windows_arg(std::string(reinterpret_cast<const char*>(u8.data()), u8.size()));
    for (auto& a : args) cmd += " " + quote_windows_arg(a);
    std::wstring wcmd = widen(cmd);

    Handle job;
    job.h = CreateJobObjectW(nullptr, nullptr);
    if (job.h) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (opts.restrict_job) {
            li.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
            li.BasicLimitInformation.ActiveProcessLimit = 1;  // the worker itself; it cannot spawn children
            if (opts.job_memory_limit) {
                li.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
                li.ProcessMemoryLimit = static_cast<SIZE_T>(opts.job_memory_limit);
            }
            JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
            ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_EXITWINDOWS |
                                     JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                                     JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
            SetInformationJobObject(job.h, JobObjectBasicUIRestrictions, &ui, sizeof ui);
        }
        SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &li, sizeof li);
    }

    PROCESS_INFORMATION pi{};
    // Application name is given explicitly: no PATH / current-directory search.
    // CREATE_NO_WINDOW gives the child a fresh hidden console, which invalidates inherited console handles: the
    // child's stdout/stderr would vanish when the parent runs in a terminal. Inherit the parent's console instead.
    const DWORD flags = CREATE_SUSPENDED | (opts.inherit_stdio ? 0 : CREATE_NO_WINDOW);
    if (!CreateProcessW(exe.c_str(), wcmd.data(), nullptr, nullptr, TRUE, flags, nullptr,
                        nullptr, &si, &pi))
        return Error{ErrorCode::Io, std::format("cannot start {} (error {})", path_to_utf8(exe), GetLastError())};
    Handle process, thread;
    process.h = pi.hProcess;
    thread.h = pi.hThread;
    if (job.h && !AssignProcessToJobObject(job.h, pi.hProcess) && opts.restrict_job) {
        TerminateProcess(pi.hProcess, 1);
        return Error{ErrorCode::Io, std::format("cannot assign the process to its job object (error {})", GetLastError())};
    }
    ResumeThread(pi.hThread);
    out_w.close();  // the child owns the only write ends now
    err_w.close();

    ProcessResult res;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool overflow = false, out_open = out_r.h != nullptr, err_open = err_r.h != nullptr;
    for (;;) {
        if (out_open) out_open = drain(out_r.h, res.output, opts.max_output_bytes, overflow);
        if (err_open) err_open = drain(err_r.h, opts.separate_stderr ? res.errput : res.output, opts.max_output_bytes, overflow);
        if (overflow) break;
        const bool exited = WaitForSingleObject(process.h, 0) == WAIT_OBJECT_0;
        if (exited && !(out_open || err_open)) break;
        if (exited) {  // process gone: one more drain pass, then stop even if a grandchild keeps the pipe open
            if (out_open) out_open = drain(out_r.h, res.output, opts.max_output_bytes, overflow);
            if (err_open) err_open = drain(err_r.h, opts.separate_stderr ? res.errput : res.output, opts.max_output_bytes, overflow);
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) { res.timed_out = true; break; }
        Sleep(5);
    }
    if (res.timed_out || overflow) {
        TerminateProcess(process.h, 1);
        if (job.h) TerminateJobObject(job.h, 1);
    }
    WaitForSingleObject(process.h, 5000);
    DWORD code = 0;
    GetExitCodeProcess(process.h, &code);
    res.exit_code = static_cast<int>(code);
    res.exit_code_u = code;
    if (overflow) return Error{ErrorCode::LimitExceeded, "process output exceeded the capture limit"};
    return res;
}
#else
std::filesystem::path current_executable() { return {}; }
Result<ProcessResult> run_process_ex(const std::filesystem::path&, const std::vector<std::string>&, std::chrono::milliseconds, const ProcessOptions&) {
    return Error{ErrorCode::Unsupported, "external process execution is only implemented on Windows"};
}
#endif

Result<ProcessResult> run_process(const std::filesystem::path& exe, const std::vector<std::string>& args,
                                  std::chrono::milliseconds timeout, size_t max_output) {
    ProcessOptions o;
    o.max_output_bytes = max_output;
    return run_process_ex(exe, args, timeout, o);
}

}  // namespace orvyn
