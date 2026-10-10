#include <fstream>
#include <map>
#include <sstream>

#include "cider/guest.hpp"
#include "cider/util.hpp"

namespace cider::guest {
namespace {

std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '%') o += "%25";
        else if (c == '\n') o += "%0A";
        else if (c == '\r') o += "%0D";
        else if (c == '|') o += "%7C";
        else o.push_back(c);
    }
    return o;
}
std::string unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && i + 2 <= s.size() - 1 + 0) {
            std::string h = s.substr(i + 1, 2);
            if (h == "25") { o.push_back('%'); i += 2; continue; }
            if (h == "0A") { o.push_back('\n'); i += 2; continue; }
            if (h == "0D") { o.push_back('\r'); i += 2; continue; }
            if (h == "7C") { o.push_back('|'); i += 2; continue; }
        }
        o.push_back(s[i]);
    }
    return o;
}
const char* failure_name(loader::LoadFailure f) {
    switch (f) {
        case loader::LoadFailure::None: return "none";
        case loader::LoadFailure::Incompatible: return "incompatible";
        case loader::LoadFailure::Unsupported: return "unsupported";
        case loader::LoadFailure::MissingDependency: return "missing-dependency";
        case loader::LoadFailure::UnresolvedSymbol: return "unresolved-symbol";
        case loader::LoadFailure::Internal: return "internal";
    }
    return "internal";
}
loader::LoadFailure failure_from(const std::string& s) {
    for (auto f : {loader::LoadFailure::None, loader::LoadFailure::Incompatible, loader::LoadFailure::Unsupported,
                   loader::LoadFailure::MissingDependency, loader::LoadFailure::UnresolvedSymbol, loader::LoadFailure::Internal})
        if (s == failure_name(f)) return f;
    return loader::LoadFailure::Internal;
}
}  // namespace

std::string serialize_result(const Outcome& r) {
    std::ostringstream o;
    o << "cider-result=1\n";
    o << "state=" << to_string(r.state) << "\n";
    o << "exit_code=" << r.exit_code << "\n";
    o << "returned_from_main=" << (r.returned_from_main ? 1 : 0) << "\n";
    o << "load_failure=" << failure_name(r.load_failure) << "\n";
    o << "message=" << esc(r.message) << "\n";
    o << "entry=" << r.entry_address << "\n";
    o << "base=" << r.image_base << "\n";
    o << "missing_library=" << esc(r.missing_library) << "\n";
    o << "initializers_run=" << r.initializers_run << "\n";
    for (auto& i : r.images) o << "image=" << esc(i.name) << "|" << esc(i.path) << "|" << i.base << "|" << i.initializers << "|" << (i.main ? 1 : 0) << "\n";
    for (auto& l : r.required_libs) o << "lib=" << esc(l) << "\n";
    for (auto& u : r.unresolved) o << "unresolved=" << esc(u.symbol) << "|" << esc(u.library) << "|" << esc(u.reason) << "|" << (u.weak ? 1 : 0) << "\n";
    o << "end\n";
    return o.str();
}

std::optional<Outcome> parse_result(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != "cider-result=1") return std::nullopt;
    Outcome r;
    bool end = false, have_state = false;
    while (std::getline(in, line)) {
        if (line == "end") { end = true; break; }
        auto eq = line.find('=');
        if (eq == std::string::npos) return std::nullopt;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        try {
            if (k == "state") {
                have_state = true;
                if (v == "exited") r.state = State::Exited;
                else if (v == "faulted") r.state = State::Faulted;
                else if (v == "load-failed") r.state = State::LoadFailed;
                else if (v == "not-run") r.state = State::NotRun;
                else return std::nullopt;
            } else if (k == "exit_code") r.exit_code = static_cast<int32_t>(std::stoll(v));
            else if (k == "returned_from_main") r.returned_from_main = v == "1";
            else if (k == "load_failure") r.load_failure = failure_from(v);
            else if (k == "message") r.message = unesc(v);
            else if (k == "entry") r.entry_address = std::stoull(v);
            else if (k == "base") r.image_base = std::stoull(v);
            else if (k == "lib") r.required_libs.push_back(unesc(v));
            else if (k == "missing_library") r.missing_library = unesc(v);
            else if (k == "initializers_run") r.initializers_run = std::stoull(v);
            else if (k == "image") {
                std::vector<std::string> parts;
                size_t st = 0;
                for (;;) {
                    size_t b = v.find('|', st);
                    parts.push_back(v.substr(st, b == std::string::npos ? std::string::npos : b - st));
                    if (b == std::string::npos) break;
                    st = b + 1;
                }
                if (parts.size() != 5) return std::nullopt;
                r.images.push_back({unesc(parts[0]), unesc(parts[1]), std::stoull(parts[2]), std::stoull(parts[3]), parts[4] == "1"});
            }
            else if (k == "unresolved") {
                std::vector<std::string> parts;
                size_t st = 0;
                for (;;) {
                    size_t b = v.find('|', st);
                    parts.push_back(v.substr(st, b == std::string::npos ? std::string::npos : b - st));
                    if (b == std::string::npos) break;
                    st = b + 1;
                }
                if (parts.size() != 4) return std::nullopt;
                r.unresolved.push_back({unesc(parts[0]), unesc(parts[1]), parts[3] == "1", unesc(parts[2])});
            }
        } catch (...) { return std::nullopt; }
    }
    if (!end || !have_state) return std::nullopt;  // truncated file: the worker died while writing
    return r;
}

bool write_result_file(const std::filesystem::path& p, const Outcome& r) {
    // write to a temp name then rename: the supervisor never sees a half-written result
    std::filesystem::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << serialize_result(r);
        f.flush();
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    return !ec;
}

}  // namespace cider::guest
