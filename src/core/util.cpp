#include "cider/util.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace cider {

std::string hex(uint64_t v) { return std::format("{:#x}", v); }

std::string version_string(uint32_t v) {
    unsigned x = v >> 16, y = (v >> 8) & 0xff, z = v & 0xff;
    return z ? std::format("{}.{}.{}", x, y, z) : std::format("{}.{}", x, y);
}

std::string sanitize_utf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    auto cont = [&](size_t k) { return k < s.size() && (static_cast<uint8_t>(s[k]) & 0xC0) == 0x80; };
    while (i < s.size()) {
        uint8_t c = static_cast<uint8_t>(s[i]);
        size_t len = 0;
        uint32_t cp = 0;
        if (c < 0x80) { out.push_back(static_cast<char>(c)); ++i; continue; }
        if (c >= 0xC2 && c <= 0xDF && cont(i + 1)) { len = 2; cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu); }
        else if (c >= 0xE0 && c <= 0xEF && cont(i + 1) && cont(i + 2)) {
            cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
            if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) len = 3;
        } else if (c >= 0xF0 && c <= 0xF4 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
            cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu);
            if (cp >= 0x10000 && cp <= 0x10FFFF) len = 4;
        }
        if (len) { out.append(s.substr(i, len)); i += len; }
        else { out += "\xEF\xBF\xBD"; ++i; }
    }
    return out;
}

std::string to_lower_ascii(std::string_view s) {
    std::string r(s);
    for (char& c : r) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}

bool starts_with(std::string_view s, std::string_view p) noexcept {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool ends_with_icase(std::string_view s, std::string_view suf) noexcept {
    if (s.size() < suf.size()) return false;
    return to_lower_ascii(s.substr(s.size() - suf.size())) == to_lower_ascii(suf);
}

std::filesystem::path path_from_utf8(std::string_view s) {
    // Native separators: extended-length paths (\?\...) are not normalised by Windows, so a '/' appended
    // to one by operator/ would be taken literally.
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())).make_preferred();
}

std::string path_to_utf8(const std::filesystem::path& p) {
    auto u = p.generic_u8string();
    std::string s(reinterpret_cast<const char*>(u.data()), u.size());
    // strip the Windows extended-length prefix: "//?/C:/x" -> "C:/x", "//?/UNC/h/s" -> "//h/s"
    if (s.rfind("//?/UNC/", 0) == 0) s = "//" + s.substr(8);
    else if (s.rfind("//?/", 0) == 0 || s.rfind("//./", 0) == 0) s = s.substr(4);
    return s;
}

std::filesystem::path extended_path(const std::filesystem::path& p) {
#ifdef _WIN32
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec);
    if (ec) return p;
    std::wstring w = abs.make_preferred().native();
    if (w.rfind(L"\\\\?\\", 0) == 0 || w.rfind(L"\\\\.\\", 0) == 0) return abs;  // already extended / device path
    if (w.rfind(L"\\\\", 0) == 0) return std::filesystem::path(L"\\\\?\\UNC\\" + w.substr(2));
    return std::filesystem::path(L"\\\\?\\" + w);
#else
    return p;
#endif
}

Result<std::vector<uint8_t>> read_file(const std::filesystem::path& p, uint64_t max_size) {
    std::error_code ec;
    auto st = std::filesystem::status(p, ec);
    if (ec || !std::filesystem::exists(st))
        return Error{ErrorCode::NotFound, "file not found: " + path_to_utf8(p)};
    if (!std::filesystem::is_regular_file(st))
        return Error{ErrorCode::InvalidArgument, "not a regular file: " + path_to_utf8(p)};
    uint64_t size = std::filesystem::file_size(p, ec);
    if (ec) return Error{ErrorCode::Io, "cannot stat " + path_to_utf8(p) + ": " + ec.message()};
    if (size > max_size)
        return Error{ErrorCode::TooLarge, std::format("{} is {} bytes, limit is {}", path_to_utf8(p), size, max_size)};
    std::ifstream f(p, std::ios::binary);
    if (!f) return Error{ErrorCode::Io, "cannot open " + path_to_utf8(p)};
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (size && !f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size)))
        return Error{ErrorCode::Io, "short read on " + path_to_utf8(p)};
    return buf;
}

bool is_inside(const std::filesystem::path& root, const std::filesystem::path& child) {
    std::error_code ec;
    auto r = std::filesystem::weakly_canonical(root, ec);
    if (ec) return false;
    auto c = std::filesystem::weakly_canonical(child, ec);
    if (ec) return false;
    auto rel = c.lexically_relative(r);
    if (rel.empty()) return false;
    auto first = *rel.begin();
    return first != ".." && !rel.is_absolute();
}

}  // namespace cider
