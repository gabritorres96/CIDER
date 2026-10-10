#pragma once
// Property-list reader (XML and binary "bplist00"). Treats input as untrusted.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cider/bytes.hpp"
#include "cider/result.hpp"

namespace cider::plist {

struct Value {
    enum class Type { Null, Bool, Int, Real, String, Date, Data, Uid, Array, Dict };
    Type type = Type::Null;
    bool b = false;
    int64_t i = 0;            // Int, Uid
    double d = 0;             // Real, Date (seconds since 2001-01-01 UTC)
    std::string s;            // String
    std::vector<uint8_t> data;
    std::vector<Value> items;        // Array elements, or Dict values
    std::vector<std::string> keys;   // Dict keys (parallel to items)

    // Dict lookup. With duplicate keys the last one wins (same as CoreFoundation).
    const Value* get(std::string_view key) const;
    std::optional<std::string> get_string(std::string_view key) const;  // scalars rendered as text
    bool is_dict() const noexcept { return type == Type::Dict; }
    // Scalar -> text (String/Int/Real/Bool); nullopt for containers/data.
    std::optional<std::string> as_text() const;
};

struct Limits {
    size_t max_depth = 64;
    size_t max_nodes = 1u << 20;
    size_t max_input = 64u << 20;
};

enum class Format { Unknown, Xml, Binary };
Format detect(ByteView data) noexcept;

Result<Value> parse(ByteView data, const Limits& limits = {});
Result<Value> parse_xml(std::string_view text, const Limits& limits = {});
Result<Value> parse_binary(ByteView data, const Limits& limits = {});

}  // namespace cider::plist
