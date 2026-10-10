// Interactive GUI tests (CIDER 0.5.11): the REAL cider.exe runs hello_window; this controller process finds its window by enumerating top-level windows of the
// worker, reads and drives the controls with ordinary window messages (BM_CLICK, WM_GETTEXT, WM_CLOSE) and checks the program's reaction. Nothing in the runtime
// knows about the test. They need an interactive Windows desktop: without one (service session, CI) they are skipped.
//
// Not part of the headless suites: run with `ctest -L gui` (or this executable directly).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "cider/util.hpp"
#include "test_framework.hpp"

using namespace cider;
#define REQUIRE_MSG(cond, msg) do { if (!(cond)) { cider::testing::report_failure(__FILE__, __LINE__, msg); throw cider::testing::AbortTest{}; } } while (0)
namespace fs = std::filesystem;

namespace {

bool desktop_available() {
    if (std::getenv("CIDER_NO_GUI_TESTS")) return false;
    HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
    if (!d) return false;
    CloseDesktop(d);
    return true;
}

std::wstring text_of(HWND h) {
    wchar_t buf[512] = {};
    SendMessageTimeoutW(h, WM_GETTEXT, 511, reinterpret_cast<LPARAM>(buf), SMTO_ABORTIFHUNG, 3000, nullptr);
    return buf;
}
std::wstring class_of(HWND h) {
    wchar_t buf[128] = {};
    GetClassNameW(h, buf, 127);
    return buf;
}

DWORD parent_of(DWORD pid) {
    DWORD parent = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e{sizeof e};
    if (snap != INVALID_HANDLE_VALUE) {
        for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) if (e.th32ProcessID == pid) { parent = e.th32ParentProcessID; break; }
        CloseHandle(snap);
    }
    return parent;
}

struct Controls {
    HWND window = nullptr;
    HWND click_me = nullptr, close = nullptr, counter = nullptr, hello = nullptr, subtitle = nullptr;
};

// the program under test: cider.exe run <fixture>, with stdout/stderr going to files
class Program {
public:
    explicit Program(const fs::path& fixture, const std::string& extra = "--timeout 90") {
        const char* exe = std::getenv("CIDER_EXE");
        REQUIRE_MSG(exe != nullptr, "CIDER_EXE is not set");
        out_ = fs::temp_directory_path() / ("cider-gui-test-" + std::to_string(GetCurrentProcessId()) + ".log");
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE f = CreateFileW(out_.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW si{sizeof si};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = f;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        std::wstring cmd = L"\"" + fs::path(exe).wstring() + L"\" run \"" + fixture.wstring() + L"\" " + std::wstring(extra.begin(), extra.end());
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
        CloseHandle(f);
        REQUIRE_MSG(ok != 0, "cannot start cider.exe");
        process_ = pi.hProcess;
        pid_ = pi.dwProcessId;
        CloseHandle(pi.hThread);
    }
    ~Program() {
        if (process_) {
            if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) TerminateProcess(process_, 99);
            CloseHandle(process_);
        }
        std::error_code ec;
        fs::remove(out_, ec);
    }
    bool is_descendant(DWORD pid) const {
        for (int depth = 0; pid && depth < 6; ++depth, pid = parent_of(pid)) if (pid == pid_) return true;
        return false;
    }
    // waits for the program's top-level CIDERWindow
    HWND find_window(int timeout_ms) {
        struct Ctx { const Program* p; HWND found; } ctx{this, nullptr};
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < end) {
            EnumWindows([](HWND h, LPARAM l) -> BOOL {
                auto* c = reinterpret_cast<Ctx*>(l);
                if (class_of(h) != L"CIDERWindow" || !IsWindowVisible(h)) return TRUE;
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                if (c->p->is_descendant(pid)) { c->found = h; return FALSE; }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&ctx));
            if (ctx.found) return ctx.found;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return nullptr;
    }
    bool wait_exit(int timeout_ms, DWORD& code) {
        if (WaitForSingleObject(process_, static_cast<DWORD>(timeout_ms)) != WAIT_OBJECT_0) return false;
        GetExitCodeProcess(process_, &code);
        return true;
    }
    std::string output() const {
        std::ifstream f(out_, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), {});
    }

private:
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
    fs::path out_;
};

Controls inspect(HWND window) {
    Controls c;
    c.window = window;
    EnumChildWindows(window, [](HWND h, LPARAM l) -> BOOL {
        auto* c = reinterpret_cast<Controls*>(l);
        const std::wstring cls = class_of(h), t = text_of(h);
        if (cls == L"Button" && t == L"Click me") c->click_me = h;
        else if (cls == L"Button" && t == L"Close") c->close = h;
        else if (cls == L"Static" && t.rfind(L"Clicks:", 0) == 0) c->counter = h;
        else if (cls == L"Static" && t == L"Hello from macOS!") c->hello = h;
        else if (cls == L"Static" && t == L"Running through CIDER 0.5") c->subtitle = h;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&c));
    return c;
}

bool wait_for_text(HWND h, const std::wstring& expected, int timeout_ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (text_of(h) == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

}  // namespace

CIDER_TEST(hello_window_creates_a_real_window_with_the_program_controls_and_reacts_to_button_clicks) {
    if (!desktop_available()) SKIP("no interactive desktop");
    Program program(fs::path(CIDER_APPKIT_FIXTURES_DIR) / "bin" / "hello_window" / "hello_window");
    HWND w = program.find_window(20000);
    REQUIRE_MSG(w != nullptr, "the program did not create its window");

    // the window the Objective-C code described
    CHECK(text_of(w) == L"CIDER — Hello from macOS");
    CHECK(class_of(w) == L"CIDERWindow" && IsWindowVisible(w));
    Controls c = inspect(w);
    CHECK(c.click_me != nullptr && c.close != nullptr && c.counter != nullptr && c.hello != nullptr && c.subtitle != nullptr);
    if (!c.click_me || !c.close || !c.counter) return;
    CHECK(text_of(c.counter) == L"Clicks: 0");
    RECT client{};
    GetClientRect(w, &client);
    const double scale = GetDpiForWindow(w) / 96.0;
    CHECK(std::abs((client.right - client.left) - 420 * scale) <= 1 && std::abs((client.bottom - client.top) - 240 * scale) <= 1);
    CHECK(IsWindowEnabled(c.click_me) && IsWindowEnabled(c.close));

    // three real BM_CLICK messages: the button sends WM_COMMAND to its parent, the backend dispatches -[AppController buttonClicked:] in the guest
    for (int i = 1; i <= 3; ++i) {
        SendMessageW(c.click_me, BM_CLICK, 0, 0);
        CHECK(wait_for_text(c.counter, L"Clicks: " + std::to_wstring(i), 5000));
    }

    // Close: the guest closes the window, the delegate ends the application, the event loop returns and the process exits with status 0
    SendMessageW(c.close, BM_CLICK, 0, 0);
    DWORD code = 999;
    CHECK(program.wait_exit(15000, code));
    CHECK_EQ(code, DWORD{0});
    CHECK(!IsWindow(w));
    const std::string log = program.output();
    CHECK(log.find("Button clicked! count=1\n") != std::string::npos && log.find("Button clicked! count=3\n") != std::string::npos);
    CHECK(log.find("Close pressed\n") != std::string::npos && log.find("Window closing after 3 click(s)\n") != std::string::npos);
    CHECK(log.find("Application terminating\n") != std::string::npos && log.find("Guest exit code: 0") != std::string::npos);
}

CIDER_TEST(closing_the_window_with_the_title_bar_button_ends_the_program_cleanly) {
    if (!desktop_available()) SKIP("no interactive desktop");
    Program program(fs::path(CIDER_APPKIT_FIXTURES_DIR) / "bin" / "hello_window" / "hello_window");
    HWND w = program.find_window(20000);
    REQUIRE_MSG(w != nullptr, "the program did not create its window");
    PostMessageW(w, WM_CLOSE, 0, 0);                              // what the X button sends
    DWORD code = 999;
    CHECK(program.wait_exit(15000, code));
    CHECK_EQ(code, DWORD{0});
    CHECK(program.output().find("Window closing after 0 click(s)\n") != std::string::npos);
}

CIDER_TEST(two_runs_in_a_row_leave_no_windows_behind) {
    if (!desktop_available()) SKIP("no interactive desktop");
    for (int i = 0; i < 2; ++i) {
        Program program(fs::path(CIDER_APPKIT_FIXTURES_DIR) / "bin" / "hello_window" / "hello_window");
        HWND w = program.find_window(20000);
        REQUIRE_MSG(w != nullptr, "the program did not create its window");
        PostMessageW(w, WM_CLOSE, 0, 0);
        DWORD code = 999;
        CHECK(program.wait_exit(15000, code));
        CHECK(!IsWindow(w));
    }
}
