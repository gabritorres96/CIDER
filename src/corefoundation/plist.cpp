#include "orvyn/plist.hpp"

#include <charconv>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

#include "orvyn/util.hpp"

namespace orvyn::plist {

// ---------------------------------------------------------------- Value helpers
const Value* Value::get(std::string_view key) const {
    if (type != Type::Dict) return nullptr;
    for (size_t k = keys.size(); k-- > 0;)
        if (keys[k] == key) return &items[k];
    return nullptr;
}

std::optional<std::string> Value::as_text() const {
    switch (type) {
        case Type::String: return s;
        case Type::Int: return std::to_string(i);
        case Type::Real: return std::format("{}", d);
        case Type::Bool: return b ? "true" : "false";
        default: return std::nullopt;
    }
}

std::optional<std::string> Value::get_string(std::string_view key) const {
    const Value* v = get(key);
    return v ? v->as_text() : std::nullopt;
}

Format detect(ByteView d) noexcept {
    if (d.size() >= 8 && std::memcmp(d.data(), "bplist00", 8) == 0) return Format::Binary;
    size_t i = 0;
    if (d.size() >= 3 && d.data()[0] == 0xEF && d.data()[1] == 0xBB && d.data()[2] == 0xBF) i = 3;
    while (i < d.size() && (d.data()[i] == ' ' || d.data()[i] == '\t' || d.data()[i] == '\r' || d.data()[i] == '\n')) ++i;
    if (i < d.size() && d.data()[i] == '<') return Format::Xml;
    return Format::Unknown;
}

Result<Value> parse(ByteView data, const Limits& limits) {
    if (data.size() > limits.max_input) return Error{ErrorCode::TooLarge, "plist exceeds input size limit"};
    switch (detect(data)) {
        case Format::Binary: return parse_binary(data, limits);
        case Format::Xml: return parse_xml(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), limits);
        default: return Error{ErrorCode::Unsupported, "unrecognised plist format (only XML and binary are supported)"};
    }
}

// ---------------------------------------------------------------- XML
namespace {

struct Tag {
    std::string name;
    bool close = false, self = false;
};

Result<std::vector<uint8_t>> base64_decode(std::string_view in) {
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0, pad = 0;
    for (char c : in) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') { ++pad; continue; }
        else return Error{ErrorCode::Malformed, "invalid base64 character in <data>"};
        if (pad) return Error{ErrorCode::Malformed, "base64 data after padding"};
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<uint8_t>((acc >> bits) & 0xff)); }
    }
    return out;
}

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

class XmlParser {
public:
    XmlParser(std::string_view s, const Limits& l) : s_(s), lim_(l) {}

    Result<Value> run() {
        if (s_.size() >= 3 && static_cast<uint8_t>(s_[0]) == 0xEF && static_cast<uint8_t>(s_[1]) == 0xBB &&
            static_cast<uint8_t>(s_[2]) == 0xBF) p_ = 3;
        ORVYN_CHECK(skip_misc());
        ORVYN_TRY(root, read_tag());
        if (root.close || root.name != "plist") return err("expected <plist> root element");
        Value v;
        if (root.self) {
            return err("empty <plist> element");
        }
        ORVYN_TRY(val, parse_value(0));
        v = std::move(val);
        ORVYN_CHECK(skip_misc());
        ORVYN_TRY(end, read_tag());
        if (!end.close || end.name != "plist") return err("expected </plist>");
        ORVYN_CHECK(skip_misc());
        if (p_ != s_.size()) return err("trailing content after </plist>");
        return v;
    }

private:
    std::string_view s_;
    Limits lim_;
    size_t p_ = 0, nodes_ = 0;

    Error err(const std::string& msg) const {
        size_t line = 1;
        for (size_t i = 0; i < p_ && i < s_.size(); ++i) line += s_[i] == '\n';
        return Error{ErrorCode::Malformed, std::format("XML plist line {}: {}", line, msg)};
    }
    bool starts(std::string_view lit) const { return s_.substr(p_, lit.size()) == lit; }
    void skip_ws() { while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\r' || s_[p_] == '\n')) ++p_; }

    Status skip_until(std::string_view end, const char* what) {
        size_t e = s_.find(end, p_);
        if (e == std::string_view::npos) return err(std::string("unterminated ") + what);
        p_ = e + end.size();
        return {};
    }

    Status skip_misc() {
        for (;;) {
            skip_ws();
            if (starts("<!--")) ORVYN_CHECK(skip_until("-->", "comment"));
            else if (starts("<?")) ORVYN_CHECK(skip_until("?>", "processing instruction"));
            else if (starts("<!DOCTYPE")) {
                int depth = 0;
                while (p_ < s_.size()) {
                    char c = s_[p_++];
                    if (c == '[') ++depth;
                    else if (c == ']') --depth;
                    else if (c == '>' && depth <= 0) break;
                }
            } else return {};
        }
    }

    Result<Tag> read_tag() {
        if (p_ >= s_.size() || s_[p_] != '<') return err("expected '<'");
        ++p_;
        Tag t;
        if (p_ < s_.size() && s_[p_] == '/') { t.close = true; ++p_; }
        while (p_ < s_.size() && s_[p_] != '>' && s_[p_] != '/' && s_[p_] != ' ' && s_[p_] != '\t' && s_[p_] != '\r' && s_[p_] != '\n')
            t.name.push_back(s_[p_++]);
        if (t.name.empty()) return err("empty tag name");
        char quote = 0;
        while (p_ < s_.size()) {
            char c = s_[p_];
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '"' || c == '\'') quote = c;
            else if (c == '>') { ++p_; return t; }
            else if (c == '/' && p_ + 1 < s_.size() && s_[p_ + 1] == '>') { t.self = true; p_ += 2; return t; }
            ++p_;
        }
        return err("unterminated tag");
    }

    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else { out.push_back(static_cast<char>(0xF0 | (cp >> 18))); out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    }

    // Reads character data up to the matching close tag and consumes it.
    Result<std::string> read_text(const std::string& name) {
        std::string out;
        while (p_ < s_.size()) {
            char c = s_[p_];
            if (c == '<') {
                if (starts("<![CDATA[")) {
                    p_ += 9;
                    size_t e = s_.find("]]>", p_);
                    if (e == std::string_view::npos) return err("unterminated CDATA");
                    out.append(s_.substr(p_, e - p_));
                    p_ = e + 3;
                    continue;
                }
                if (starts("<!--")) { ORVYN_CHECK(skip_until("-->", "comment")); continue; }
                break;
            }
            if (c == '&') {
                size_t semi = s_.find(';', p_);
                if (semi == std::string_view::npos || semi - p_ > 12) return err("malformed entity");
                std::string_view ent = s_.substr(p_ + 1, semi - p_ - 1);
                if (ent == "lt") out.push_back('<');
                else if (ent == "gt") out.push_back('>');
                else if (ent == "amp") out.push_back('&');
                else if (ent == "quot") out.push_back('"');
                else if (ent == "apos") out.push_back('\'');
                else if (ent.size() > 1 && ent[0] == '#') {
                    uint32_t cp = 0;
                    bool hexa = ent[1] == 'x' || ent[1] == 'X';
                    std::string_view digits = ent.substr(hexa ? 2 : 1);
                    auto r = std::from_chars(digits.data(), digits.data() + digits.size(), cp, hexa ? 16 : 10);
                    if (digits.empty() || r.ec != std::errc() || r.ptr != digits.data() + digits.size() || cp > 0x10FFFF ||
                        (cp >= 0xD800 && cp <= 0xDFFF) || cp == 0)
                        return err("invalid character reference");
                    append_utf8(out, cp);
                } else return err("unknown entity &" + std::string(ent) + ";");
                p_ = semi + 1;
                continue;
            }
            out.push_back(c);
            ++p_;
        }
        ORVYN_TRY(t, read_tag());
        if (!t.close || t.name != name) return err("expected </" + name + ">");
        return out;
    }

    static std::string_view trim(std::string_view v) {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r' || v.front() == '\n')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n')) v.remove_suffix(1);
        return v;
    }

    Result<Value> parse_value(size_t depth) {
        if (depth > lim_.max_depth) return Error{ErrorCode::LimitExceeded, "plist nesting too deep"};
        if (++nodes_ > lim_.max_nodes) return Error{ErrorCode::LimitExceeded, "plist has too many nodes"};
        ORVYN_CHECK(skip_misc());
        ORVYN_TRY(t, read_tag());
        if (t.close) return err("unexpected closing tag </" + t.name + ">");
        Value v;
        if (t.name == "dict") {
            v.type = Value::Type::Dict;
            if (t.self) return v;
            for (;;) {
                ORVYN_CHECK(skip_misc());
                if (starts("</")) {
                    ORVYN_TRY(e, read_tag());
                    if (e.name != "dict") return err("expected </dict>");
                    return v;
                }
                ORVYN_TRY(k, read_tag());
                if (k.close || k.name != "key") return err("expected <key> inside <dict>");
                std::string key;
                if (!k.self) { ORVYN_TRY(kt, read_text("key")); key = kt; }
                ORVYN_TRY(val, parse_value(depth + 1));
                v.keys.push_back(std::move(key));
                v.items.push_back(std::move(val));
            }
        }
        if (t.name == "array") {
            v.type = Value::Type::Array;
            if (t.self) return v;
            for (;;) {
                ORVYN_CHECK(skip_misc());
                if (starts("</")) {
                    ORVYN_TRY(e, read_tag());
                    if (e.name != "array") return err("expected </array>");
                    return v;
                }
                ORVYN_TRY(val, parse_value(depth + 1));
                v.items.push_back(std::move(val));
            }
        }
        if (t.name == "true" || t.name == "false") {
            v.type = Value::Type::Bool;
            v.b = t.name == "true";
            if (!t.self) { ORVYN_TRY(txt, read_text(t.name)); if (!trim(txt).empty()) return err("unexpected text in boolean"); }
            return v;
        }
        std::string text;
        if (!t.self) { ORVYN_TRY(txt, read_text(t.name)); text = txt; }
        std::string_view tv = trim(text);
        if (t.name == "string") { v.type = Value::Type::String; v.s = std::move(text); return v; }
        if (t.name == "integer") {
            v.type = Value::Type::Int;
            bool neg = !tv.empty() && tv[0] == '-';
            std::string_view digits = (neg || (!tv.empty() && tv[0] == '+')) ? tv.substr(1) : tv;
            int base = 10;
            if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) { base = 16; digits.remove_prefix(2); }
            uint64_t mag = 0;
            auto r = std::from_chars(digits.data(), digits.data() + digits.size(), mag, base);
            if (digits.empty() || r.ec != std::errc() || r.ptr != digits.data() + digits.size()) return err("invalid <integer>");
            if (neg ? mag > (1ull << 63) : mag > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                return err("<integer> out of int64 range");
            v.i = neg ? static_cast<int64_t>(0 - mag) : static_cast<int64_t>(mag);
            return v;
        }
        if (t.name == "real") {
            v.type = Value::Type::Real;
            std::string low = to_lower_ascii(tv);
            if (low == "nan") v.d = std::nan("");
            else if (low == "inf" || low == "+inf" || low == "infinity" || low == "+infinity") v.d = INFINITY;
            else if (low == "-inf" || low == "-infinity") v.d = -INFINITY;
            else {
                std::string_view d = (!tv.empty() && tv[0] == '+') ? tv.substr(1) : tv;
                auto r = std::from_chars(d.data(), d.data() + d.size(), v.d);
                if (d.empty() || r.ec != std::errc() || r.ptr != d.data() + d.size()) return err("invalid <real>");
            }
            return v;
        }
        if (t.name == "date") {
            v.type = Value::Type::Date;
            int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;
            if (tv.size() != 20 || std::sscanf(std::string(tv).c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ", &Y, &M, &D, &h, &m, &s) != 6 ||
                M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || s > 60)
                return err("invalid <date> (expected YYYY-MM-DDThh:mm:ssZ)");
            int64_t days = days_from_civil(Y, static_cast<unsigned>(M), static_cast<unsigned>(D));
            v.d = static_cast<double>(days * 86400 + h * 3600 + m * 60 + s - 978307200);
            return v;
        }
        if (t.name == "data") {
            v.type = Value::Type::Data;
            auto r = base64_decode(tv);
            if (!r.ok()) return err(r.error().message);
            v.data = std::move(r.value());
            return v;
        }
        return err("unsupported element <" + t.name + ">");
    }
};

// ---------------------------------------------------------------- Binary
class BinaryParser {
public:
    BinaryParser(ByteView d, const Limits& l) : d_(d), lim_(l) {}

    Result<Value> run() {
        if (d_.size() < 8 + 32) return Error{ErrorCode::Truncated, "binary plist too small"};
        uint64_t t = d_.size() - 32;
        offset_size_ = d_.u8(t + 6).value();
        ref_size_ = d_.u8(t + 7).value();
        num_objects_ = d_.u64(t + 8, true).value();
        uint64_t top = d_.u64(t + 16, true).value();
        uint64_t table = d_.u64(t + 24, true).value();
        if (offset_size_ < 1 || offset_size_ > 8 || ref_size_ < 1 || ref_size_ > 8)
            return Error{ErrorCode::Malformed, "binary plist: invalid offset/ref size in trailer"};
        if (num_objects_ == 0 || num_objects_ > lim_.max_nodes)
            return Error{ErrorCode::LimitExceeded, "binary plist: object count invalid or too large"};
        auto tbytes = checked_mul(num_objects_, offset_size_);
        if (!tbytes || table < 8 || table > t || *tbytes > t - table)
            return Error{ErrorCode::OutOfBounds, "binary plist: offset table outside file"};
        if (top >= num_objects_) return Error{ErrorCode::Malformed, "binary plist: top object index out of range"};
        table_ = table;
        data_end_ = table;
        return object(top, 0);
    }

private:
    ByteView d_;
    Limits lim_;
    uint64_t offset_size_ = 0, ref_size_ = 0, num_objects_ = 0, table_ = 0, data_end_ = 0;
    size_t nodes_ = 0;

    Error err(const std::string& m) const { return Error{ErrorCode::Malformed, "binary plist: " + m}; }

    Result<uint64_t> read_be(uint64_t off, uint64_t n) const {
        if (!d_.contains(off, n)) return Error{ErrorCode::OutOfBounds, "binary plist: read outside file"};
        uint64_t v = 0;
        for (uint64_t i = 0; i < n; ++i) v = (v << 8) | d_.data()[off + i];
        return v;
    }

    // length nibble; 0xF means an int object follows
    Result<uint64_t> length(uint8_t nib, uint64_t& pos) const {
        if (nib != 0xF) return static_cast<uint64_t>(nib);
        ORVYN_TRY(m, d_.u8(pos));
        if ((m >> 4) != 0x1) return err("invalid length marker");
        uint64_t n = 1ull << (m & 0xF);
        if (n > 8) return err("length integer too wide");
        ORVYN_TRY(v, read_be(pos + 1, n));
        pos += 1 + n;
        return v;
    }

    Result<Value> object(uint64_t index, size_t depth) {
        if (depth > lim_.max_depth) return Error{ErrorCode::LimitExceeded, "plist nesting too deep"};
        if (++nodes_ > lim_.max_nodes) return Error{ErrorCode::LimitExceeded, "plist has too many nodes"};
        if (index >= num_objects_) return err("object reference out of range");
        ORVYN_TRY(off, read_be(table_ + index * offset_size_, offset_size_));
        if (off < 8 || off >= data_end_) return err("object offset outside object area");
        ORVYN_TRY(marker, d_.u8(off));
        uint8_t hi = marker >> 4, lo = marker & 0xF;
        uint64_t pos = off + 1;
        Value v;
        switch (hi) {
            case 0x0:
                if (lo == 0x0) return v;
                if (lo == 0x8 || lo == 0x9) { v.type = Value::Type::Bool; v.b = lo == 0x9; return v; }
                return err("unsupported simple object");
            case 0x1: {
                uint64_t n = 1ull << lo;
                if (n > 8) return err("integers wider than 64 bits are unsupported");
                ORVYN_TRY(raw, read_be(pos, n));
                v.type = Value::Type::Int;
                v.i = n == 8 ? static_cast<int64_t>(raw) : static_cast<int64_t>(raw);  // 1/2/4 byte ints are unsigned
                return v;
            }
            case 0x2: {
                uint64_t n = 1ull << lo;
                ORVYN_TRY(raw, read_be(pos, n));
                v.type = Value::Type::Real;
                if (n == 4) { uint32_t u = static_cast<uint32_t>(raw); float f; std::memcpy(&f, &u, 4); v.d = f; }
                else if (n == 8) std::memcpy(&v.d, &raw, 8);
                else return err("invalid real size");
                return v;
            }
            case 0x3: {
                if (lo != 3) return err("invalid date marker");
                ORVYN_TRY(raw, read_be(pos, 8));
                v.type = Value::Type::Date;
                std::memcpy(&v.d, &raw, 8);
                return v;
            }
            case 0x4: {
                ORVYN_TRY(n, length(lo, pos));
                if (!d_.contains(pos, n) || pos + n > data_end_) return err("data object exceeds file");
                v.type = Value::Type::Data;
                v.data.assign(d_.data() + pos, d_.data() + pos + n);
                return v;
            }
            case 0x5: {
                ORVYN_TRY(n, length(lo, pos));
                if (!d_.contains(pos, n) || pos + n > data_end_) return err("string object exceeds file");
                v.type = Value::Type::String;
                v.s.assign(reinterpret_cast<const char*>(d_.data() + pos), static_cast<size_t>(n));
                return v;
            }
            case 0x6: {
                ORVYN_TRY(n, length(lo, pos));
                auto bytes = checked_mul(n, 2);
                if (!bytes || !d_.contains(pos, *bytes) || pos + *bytes > data_end_) return err("UTF-16 string exceeds file");
                v.type = Value::Type::String;
                for (uint64_t i = 0; i < n; ++i) {
                    uint32_t u = (d_.data()[pos + 2 * i] << 8) | d_.data()[pos + 2 * i + 1];
                    if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n) {
                        uint32_t lw = (d_.data()[pos + 2 * i + 2] << 8) | d_.data()[pos + 2 * i + 3];
                        if (lw >= 0xDC00 && lw <= 0xDFFF) { u = 0x10000 + ((u - 0xD800) << 10) + (lw - 0xDC00); ++i; }
                        else u = 0xFFFD;
                    } else if (u >= 0xD800 && u <= 0xDFFF) u = 0xFFFD;
                    if (u < 0x80) v.s.push_back(static_cast<char>(u));
                    else if (u < 0x800) { v.s.push_back(static_cast<char>(0xC0 | (u >> 6))); v.s.push_back(static_cast<char>(0x80 | (u & 0x3F))); }
                    else if (u < 0x10000) { v.s.push_back(static_cast<char>(0xE0 | (u >> 12))); v.s.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F))); v.s.push_back(static_cast<char>(0x80 | (u & 0x3F))); }
                    else { v.s.push_back(static_cast<char>(0xF0 | (u >> 18))); v.s.push_back(static_cast<char>(0x80 | ((u >> 12) & 0x3F))); v.s.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F))); v.s.push_back(static_cast<char>(0x80 | (u & 0x3F))); }
                }
                return v;
            }
            case 0x8: {
                ORVYN_TRY(raw, read_be(pos, static_cast<uint64_t>(lo) + 1));
                v.type = Value::Type::Uid;
                v.i = static_cast<int64_t>(raw);
                return v;
            }
            case 0xA:
            case 0xD: {
                ORVYN_TRY(n, length(lo, pos));
                bool dict = hi == 0xD;
                auto refs = checked_mul(n, dict ? 2 * ref_size_ : ref_size_);
                if (!refs || !d_.contains(pos, *refs) || pos + *refs > data_end_) return err("container references exceed file");
                if (n > lim_.max_nodes) return Error{ErrorCode::LimitExceeded, "container too large"};
                v.type = dict ? Value::Type::Dict : Value::Type::Array;
                for (uint64_t i = 0; i < n; ++i) {
                    if (dict) {
                        ORVYN_TRY(kref, read_be(pos + i * ref_size_, ref_size_));
                        ORVYN_TRY(kv, object(kref, depth + 1));
                        if (kv.type != Value::Type::String) return err("dictionary key is not a string");
                        v.keys.push_back(std::move(kv.s));
                        ORVYN_TRY(vref, read_be(pos + (n + i) * ref_size_, ref_size_));
                        ORVYN_TRY(vv, object(vref, depth + 1));
                        v.items.push_back(std::move(vv));
                    } else {
                        ORVYN_TRY(ref, read_be(pos + i * ref_size_, ref_size_));
                        ORVYN_TRY(ev, object(ref, depth + 1));
                        v.items.push_back(std::move(ev));
                    }
                }
                return v;
            }
            default: return err(std::format("unsupported object marker {:#x}", marker));
        }
    }
};

}  // namespace

Result<Value> parse_xml(std::string_view text, const Limits& limits) {
    if (text.size() > limits.max_input) return Error{ErrorCode::TooLarge, "plist exceeds input size limit"};
    return XmlParser(text, limits).run();
}

Result<Value> parse_binary(ByteView data, const Limits& limits) {
    if (data.size() > limits.max_input) return Error{ErrorCode::TooLarge, "plist exceeds input size limit"};
    if (data.size() < 8 || std::memcmp(data.data(), "bplist00", 8) != 0) return Error{ErrorCode::BadMagic, "not a binary plist"};
    return BinaryParser(data, limits).run();
}

}  // namespace orvyn::plist
