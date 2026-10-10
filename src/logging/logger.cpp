#include "cider/log.hpp"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>

#include "cider/util.hpp"

namespace cider::log {
namespace {
std::atomic<Level> g_level{Level::Warn};
std::atomic<bool> g_stderr{true};
std::mutex g_mutex;
std::ofstream g_file;
}  // namespace

const char* level_name(Level l) noexcept {
    switch (l) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warn: return "WARN";
        case Level::Error: return "ERROR";
        case Level::Fatal: return "FATAL";
        case Level::Off: return "OFF";
    }
    return "?";
}

bool parse_level(std::string_view text, Level& out) noexcept {
    std::string t = to_lower_ascii(text);
    static constexpr std::pair<const char*, Level> table[] = {
        {"trace", Level::Trace}, {"debug", Level::Debug}, {"info", Level::Info}, {"warn", Level::Warn},
        {"error", Level::Error}, {"fatal", Level::Fatal}, {"off", Level::Off}};
    for (auto& [n, l] : table)
        if (t == n) { out = l; return true; }
    return false;
}

void set_level(Level l) noexcept { g_level.store(l); }
Level level() noexcept { return g_level.load(); }
void set_stderr_enabled(bool e) noexcept { g_stderr.store(e); }

bool set_file(const std::string& utf8_path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open()) g_file.close();
    g_file.open(path_from_utf8(utf8_path), std::ios::app);
    return g_file.is_open();
}

void close_file() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open()) g_file.close();
}

const char* category_name(Category c) noexcept {
    switch (c) {
        case Category::None: return "";
        case Category::Loader: return "LOADER";
        case Category::Memory: return "MEMORY";
        case Category::Abi: return "ABI";
        case Category::Dyld: return "DYLD";
        case Category::Symbols: return "SYMBOLS";
        case Category::LibSystem: return "LIBSYSTEM";
        case Category::Execution: return "EXECUTION";
        case Category::Process: return "PROCESS";
    }
    return "";
}

void write(Category c, Level l, std::string_view message) {
    if (c == Category::None) { write(l, message); return; }
    if (l == Level::Off) return;
    std::string line = l == Level::Info ? std::format("[CIDER][{}] {}", category_name(c), sanitize_utf8(message))
                                        : std::format("[CIDER][{}][{}] {}", category_name(c), level_name(l), sanitize_utf8(message));
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_stderr.load()) {
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fputc('\n', stderr);
    }
    if (g_file.is_open()) {
        g_file << line << '\n';
        g_file.flush();
    }
}

void write(Level l, std::string_view message) {
    if (l == Level::Off) return;
    std::string line = std::format("[CIDER][{}] {}", level_name(l), sanitize_utf8(message));
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_stderr.load()) {
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fputc('\n', stderr);
    }
    if (g_file.is_open()) {
        g_file << line << '\n';
        g_file.flush();
    }
}

}  // namespace cider::log
