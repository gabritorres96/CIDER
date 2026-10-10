#pragma once
// Centralised logger. Output format: [CIDER][LEVEL] message
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace cider::log {

enum class Level { Trace = 0, Debug, Info, Warn, Error, Fatal, Off };

// Subsystem tag for runtime messages: "[CIDER][LOADER] text" (info) or "[CIDER][LOADER][WARN] text".
// Errors are logged without a category on purpose: "[CIDER][ERROR] text".
enum class Category { None = 0, Loader, Memory, Abi, Dyld, Symbols, LibSystem, Execution, Process, Objc };
const char* category_name(Category c) noexcept;

const char* level_name(Level l) noexcept;
bool parse_level(std::string_view text, Level& out) noexcept;

void set_level(Level l) noexcept;
Level level() noexcept;
inline bool enabled(Level l) noexcept { return l >= level() && l != Level::Off; }

// Sinks. stderr is always active; a file sink is optional.
bool set_file(const std::string& utf8_path);  // false if the file cannot be opened
void close_file();
void set_stderr_enabled(bool enabled) noexcept;

void write(Level l, std::string_view message);
void write(Category c, Level l, std::string_view message);

template <class... A>
void emit(Level l, std::format_string<A...> fmt, A&&... args) {
    if (enabled(l)) write(l, std::format(fmt, std::forward<A>(args)...));
}
template <class... A>
void emit(Category c, Level l, std::format_string<A...> fmt, A&&... args) {
    if (enabled(l)) write(c, l, std::format(fmt, std::forward<A>(args)...));
}
template <class... A> void info(Category c, std::format_string<A...> f, A&&... a) { emit(c, Level::Info, f, std::forward<A>(a)...); }
template <class... A> void debug(Category c, std::format_string<A...> f, A&&... a) { emit(c, Level::Debug, f, std::forward<A>(a)...); }
template <class... A> void warn(Category c, std::format_string<A...> f, A&&... a) { emit(c, Level::Warn, f, std::forward<A>(a)...); }
template <class... A> void trace(std::format_string<A...> f, A&&... a) { emit(Level::Trace, f, std::forward<A>(a)...); }
template <class... A> void debug(std::format_string<A...> f, A&&... a) { emit(Level::Debug, f, std::forward<A>(a)...); }
template <class... A> void info(std::format_string<A...> f, A&&... a) { emit(Level::Info, f, std::forward<A>(a)...); }
template <class... A> void warn(std::format_string<A...> f, A&&... a) { emit(Level::Warn, f, std::forward<A>(a)...); }
template <class... A> void error(std::format_string<A...> f, A&&... a) { emit(Level::Error, f, std::forward<A>(a)...); }
template <class... A> void fatal(std::format_string<A...> f, A&&... a) { emit(Level::Fatal, f, std::forward<A>(a)...); }

}  // namespace cider::log
