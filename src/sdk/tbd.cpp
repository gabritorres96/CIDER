// Minimal reader for TAPI text stubs. It is a line scanner for the YAML subset TAPI emits, not a general YAML parser.
#include "orvyn/sdk.hpp"

#include <algorithm>

namespace orvyn::sdk {
namespace {

constexpr size_t kMaxInput = 64u << 20;
constexpr size_t kMaxNames = 4'000'000;

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
std::string unquote(std::string_view s) {
    s = trim(s);
    if (s.size() >= 2 && (s.front() == '\'' || s.front() == '"') && s.back() == s.front()) s = s.substr(1, s.size() - 2);
    return std::string(s);
}

// Splits "a, 'b', c" into items.
void split_list(std::string_view body, std::vector<std::string>& out) {
    size_t i = 0;
    while (i < body.size()) {
        while (i < body.size() && (body[i] == ' ' || body[i] == ',' || body[i] == '\t' || body[i] == '\r' || body[i] == '\n')) ++i;
        if (i >= body.size()) break;
        size_t j = i;
        if (body[i] == '\'' || body[i] == '"') {
            const char q = body[i];
            j = body.find(q, i + 1);
            if (j == std::string_view::npos) j = body.size() - 1;
            out.push_back(std::string(body.substr(i + 1, j - i - 1)));
            i = j + 1;
        } else {
            while (j < body.size() && body[j] != ',') ++j;
            out.push_back(std::string(trim(body.substr(i, j - i))));
            i = j;
        }
    }
}

bool targets_have_x86_64(const std::vector<std::string>& t) {
    return std::any_of(t.begin(), t.end(), [](const std::string& s) { return s.rfind("x86_64-macos", 0) == 0 && s.find("macabi") == std::string::npos; });
}

}  // namespace

std::string library_family(std::string_view n) {
    const auto fw = n.find(".framework/");
    if (fw != std::string_view::npos) {
        const auto slash = n.rfind('/', fw);
        return std::string(n.substr(slash + 1, fw - slash - 1));
    }
    const auto slash = n.rfind('/');
    std::string base(slash == std::string_view::npos ? n : n.substr(slash + 1));
    if (const auto dot = base.find('.'); dot != std::string::npos && base.rfind("lib", 0) == 0) base.resize(dot);
    return base;
}

TbdParse parse_tbd(std::string_view text) {
    TbdParse out;
    if (text.size() > kMaxInput) { out.error = "input too large"; return out; }
    size_t names = 0;

    TbdLibrary cur;
    bool in_doc = false;
    std::vector<std::string> block_targets;      // targets of the current exports/reexports block
    bool in_exports = false, in_reexports = false, block_is_x86 = false;
    std::string list_key;                        // key whose bracketed list is being collected
    std::string pending;                         // accumulated list text
    bool collecting = false;

    auto finish_doc = [&] {
        if (in_doc && !cur.install_name.empty()) out.libraries.push_back(std::move(cur));
        cur = TbdLibrary{};
        in_doc = in_exports = in_reexports = collecting = false;
    };
    auto flush_list = [&] {
        std::vector<std::string> items;
        split_list(pending, items);
        pending.clear();
        collecting = false;
        if (list_key == "targets") {
            block_targets = items;
            block_is_x86 = targets_have_x86_64(items);
            if (!in_exports && !in_reexports) { cur.targets = items; cur.has_x86_64 = block_is_x86; }
            return;
        }
        if (!block_is_x86 && (in_exports || in_reexports)) return;
        names += items.size();
        if (names > kMaxNames) return;
        if (in_reexports && list_key == "libraries") for (auto& s : items) cur.reexported_libraries.push_back(s);
        else if (in_exports) {
            if (list_key == "symbols") cur.symbols.insert(items.begin(), items.end());
            else if (list_key == "weak-symbols") cur.weak_symbols.insert(items.begin(), items.end());
            else if (list_key == "thread-local-symbols") cur.thread_local_symbols.insert(items.begin(), items.end());
            else if (list_key == "objc-classes") cur.objc_classes.insert(items.begin(), items.end());
            else if (list_key == "objc-eh-types") cur.objc_eh_types.insert(items.begin(), items.end());
            else if (list_key == "objc-ivars") cur.objc_ivars.insert(items.begin(), items.end());
        }
    };

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (collecting) {                                   // continuation of a multi-line [ ... ]
            const auto close = line.find(']');
            pending.append(line.substr(0, close == std::string_view::npos ? line.size() : close));
            pending.push_back(',');
            if (close != std::string_view::npos) flush_list();
            if (names > kMaxNames) { out.error = "too many names"; return out; }
            continue;
        }
        std::string_view t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.rfind("---", 0) == 0) { finish_doc(); in_doc = true; continue; }
        if (t == "...") { finish_doc(); continue; }
        if (!in_doc) continue;

        bool dash = false;
        if (t[0] == '-' && (t.size() == 1 || t[1] == ' ')) { dash = true; t = trim(t.substr(1)); }
        const auto colon = t.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string key(trim(t.substr(0, colon)));
        std::string_view value = trim(t.substr(colon + 1));
        const bool top = line.find_first_not_of(' ') == 0;  // key at column 0
        if (top) {
            in_exports = key == "exports";
            in_reexports = key == "reexported-libraries";
            if (key == "install-name") cur.install_name = unquote(value);
            if (!in_exports && !in_reexports && key != "targets") block_is_x86 = false;
        } else if (dash && (in_exports || in_reexports)) {
            block_targets.clear();                           // a new list entry starts: its own targets follow
            block_is_x86 = false;
        }
        if (value.empty() || value[0] != '[') continue;
        list_key = key;
        value.remove_prefix(1);
        const auto close = value.find(']');
        pending.assign(value.substr(0, close == std::string_view::npos ? value.size() : close));
        pending.push_back(',');
        if (close != std::string_view::npos) flush_list(); else collecting = true;
        if (names > kMaxNames) { out.error = "too many names"; return out; }
    }
    finish_doc();
    if (out.libraries.empty()) out.error = "no TAPI document with an install-name found";
    return out;
}

}  // namespace orvyn::sdk
