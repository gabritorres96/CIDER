#include "cider/json.hpp"

#include <charconv>
#include <cmath>

#include "cider/util.hpp"

namespace cider {

void JsonWriter::newline() {
    out_.push_back('\n');
    out_.append(stack_.size() * 2, ' ');
}

void JsonWriter::before_value() {
    if (after_key_) { after_key_ = false; return; }
    if (stack_.empty()) return;
    if (!stack_.back().first) out_.push_back(',');
    stack_.back().first = false;
    newline();
}

void JsonWriter::begin_object() { before_value(); out_.push_back('{'); stack_.push_back({true, true}); }
void JsonWriter::begin_array() { before_value(); out_.push_back('['); stack_.push_back({false, true}); }

void JsonWriter::end_object() {
    bool empty = stack_.back().first;
    stack_.pop_back();
    if (!empty) newline();
    out_.push_back('}');
}
void JsonWriter::end_array() {
    bool empty = stack_.back().first;
    stack_.pop_back();
    if (!empty) newline();
    out_.push_back(']');
}

static void append_string(std::string& out, std::string_view raw) {
    std::string s = sanitize_utf8(raw);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) out += std::format("\\u{:04x}", c);
                else out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
}

void JsonWriter::key(std::string_view k) {
    before_value();
    append_string(out_, k);
    out_ += ": ";
    after_key_ = true;
}

void JsonWriter::value(std::string_view s) { before_value(); append_string(out_, s); }
void JsonWriter::value(bool b) { before_value(); out_ += b ? "true" : "false"; }
void JsonWriter::value(int64_t v) { before_value(); out_ += std::to_string(v); }
void JsonWriter::value(uint64_t v) { before_value(); out_ += std::to_string(v); }
void JsonWriter::value(double d) {
    before_value();
    if (!std::isfinite(d)) { out_ += "null"; return; }
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, d);
    out_.append(buf, r.ptr);
}
void JsonWriter::null() { before_value(); out_ += "null"; }

}  // namespace cider
