#pragma once
// Tiny streaming JSON writer (pretty-printed, always valid UTF-8 output).
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cider {

class JsonWriter {
public:
    void begin_object();
    void end_object();
    void begin_array();
    void end_array();
    void key(std::string_view k);

    void value(std::string_view s);
    void value(const char* s) { value(std::string_view(s)); }
    void value(const std::string& s) { value(std::string_view(s)); }
    void value(bool b);
    void value(int64_t v);
    void value(uint64_t v);
    void value(int v) { value(static_cast<int64_t>(v)); }
    void value(uint32_t v) { value(static_cast<uint64_t>(v)); }
    void value(double d);
    void null();

    template <class V> void field(std::string_view k, const V& v) { key(k); value(v); }

    const std::string& str() const noexcept { return out_; }

private:
    struct Frame { bool object; bool first; };
    void before_value();
    void newline();
    std::string out_;
    std::vector<Frame> stack_;
    bool after_key_ = false;
};

}  // namespace cider
