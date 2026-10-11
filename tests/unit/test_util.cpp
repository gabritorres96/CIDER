#include <cstdlib>
#include <string>

#include "orvyn/bytes.hpp"
#include "orvyn/json.hpp"
#include "orvyn/log.hpp"
#include "orvyn/util.hpp"
#include "macho_builder.hpp"
#include "test_framework.hpp"

using namespace orvyn;
using namespace orvyn::testing;

ORVYN_TEST(checked_arithmetic) {
    CHECK(checked_add(1, 2).value() == 3);
    CHECK(!checked_add(UINT64_MAX, 1).has_value());
    CHECK(checked_mul(0, UINT64_MAX).value() == 0);
    CHECK(!checked_mul(UINT64_MAX, 2).has_value());
}

ORVYN_TEST(byteview_bounds) {
    std::vector<uint8_t> d{1, 2, 3, 4, 5, 6, 7, 8};
    ByteView v(d);
    CHECK(v.contains(0, 8));
    CHECK(!v.contains(1, 8));
    CHECK(!v.contains(UINT64_MAX, 2));
    CHECK(!v.contains(2, UINT64_MAX));
    CHECK_EQ(v.u32(0).value(), 0x04030201u);
    CHECK_EQ(v.u32(0, true).value(), 0x01020304u);
    CHECK_EQ(v.u64(0).value(), 0x0807060504030201ull);
    CHECK_ERR(v.u32(5), ErrorCode::Truncated);
    CHECK_ERR(v.u8(8), ErrorCode::Truncated);
    CHECK_ERR(v.slice(4, 5), ErrorCode::OutOfBounds);
    CHECK_EQ(v.peek_u32(100), 0u);
}

ORVYN_TEST(byteview_strings) {
    std::vector<uint8_t> d{'a', 'b', 0, 'c', 'd'};
    ByteView v(d);
    CHECK_EQ(v.cstring(0, 5).value(), std::string("ab"));
    CHECK_EQ(v.cstring(2, 5).value(), std::string(""));
    CHECK_ERR(v.cstring(3, 5), ErrorCode::Malformed);   // unterminated
    CHECK_ERR(v.cstring(9, 5), ErrorCode::OutOfBounds);
    CHECK_ERR(v.cstring(0, 2), ErrorCode::Malformed);   // terminator beyond `end`
    CHECK_EQ(v.fixed_string(0, 5), std::string("ab"));
    CHECK_EQ(v.fixed_string(3, 2), std::string("cd"));
    CHECK_EQ(v.fixed_string(4, 10), std::string(""));
}

ORVYN_TEST(version_and_hex_formatting) {
    CHECK_EQ(version_string(0x000a0f00), std::string("10.15"));
    CHECK_EQ(version_string(0x000b0203), std::string("11.2.3"));
    CHECK_EQ(hex(255), std::string("0xff"));
}

ORVYN_TEST(utf8_sanitising) {
    CHECK_EQ(sanitize_utf8("plain"), std::string("plain"));
    CHECK_EQ(sanitize_utf8("caf\xC3\xA9"), std::string("caf\xC3\xA9"));
    CHECK_EQ(sanitize_utf8("a\xFF" "b"), std::string("a\xEF\xBF\xBD" "b"));
    CHECK_EQ(sanitize_utf8("\xC0\x80"), std::string("\xEF\xBF\xBD\xEF\xBF\xBD"));       // overlong
    CHECK_EQ(sanitize_utf8("\xED\xA0\x80"), std::string("\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"));  // surrogate
}

ORVYN_TEST(json_writer_escapes_and_nests) {
    JsonWriter w;
    w.begin_object();
    w.field("s", "a\"b\\c\n\x01");
    w.field("n", 5);
    w.field("b", true);
    w.key("arr"); w.begin_array(); w.value(1); w.value("x"); w.begin_object(); w.end_object(); w.end_array();
    w.key("nil"); w.null();
    w.field("bad", "x\xFF");
    w.end_object();
    const std::string& s = w.str();
    CHECK(s.find("\"a\\\"b\\\\c\\n\\u0001\"") != std::string::npos);
    CHECK(s.find("\"n\": 5") != std::string::npos);
    CHECK(s.find("\"nil\": null") != std::string::npos);
    CHECK(s.find("\\u") != std::string::npos);
    CHECK(s.find('\xFF') == std::string::npos);  // invalid UTF-8 never leaks into JSON
}

ORVYN_TEST(log_levels_and_file_sink) {
    log::Level l;
    CHECK(log::parse_level("DEBUG", l) && l == log::Level::Debug);
    CHECK(!log::parse_level("loud", l));
    TempDir t;
    auto file = path_to_utf8(t.path() / "x.log");
    log::set_stderr_enabled(false);
    log::set_level(log::Level::Info);
    REQUIRE(log::set_file(file));
    log::info("hello {}", 42);
    log::debug("hidden");
    log::error("bad");
    log::close_file();
    log::set_stderr_enabled(true);
    log::set_level(log::Level::Off);
    auto bytes = read_file(t.path() / "x.log", 1 << 20).value();
    std::string text(bytes.begin(), bytes.end());
    CHECK(text.find("[ORVYN][INFO] hello 42") != std::string::npos);
    CHECK(text.find("[ORVYN][ERROR] bad") != std::string::npos);
    CHECK(text.find("hidden") == std::string::npos);
}

ORVYN_TEST(read_file_limits) {
    TempDir t;
    t.write("f.bin", std::vector<uint8_t>(100, 7));
    CHECK(read_file(t.path() / "f.bin", 100).ok());
    CHECK_ERR(read_file(t.path() / "f.bin", 99), ErrorCode::TooLarge);
    CHECK_ERR(read_file(t.path() / "nope", 100), ErrorCode::NotFound);
    CHECK_ERR(read_file(t.path(), 100), ErrorCode::InvalidArgument);
}

ORVYN_TEST(is_inside_detects_escape) {
    TempDir t;
    std::filesystem::create_directories(t.path() / "a/b");
    CHECK(is_inside(t.path() / "a", t.path() / "a/b/c.txt"));
    CHECK(!is_inside(t.path() / "a", t.path() / "a/../x"));
    CHECK(is_inside(t.path() / "a", t.path() / "a"));  // the root itself counts as inside
}

ORVYN_TEST(getenv_compat_prefers_the_new_name_and_falls_back_to_the_legacy_one) {
    _putenv_s("ORVYN_TEST_COMPAT", "");
    _putenv_s("CIDER_TEST_COMPAT", "");
    CHECK(orvyn::getenv_compat("ORVYN_TEST_COMPAT", "CIDER_TEST_COMPAT") == nullptr);
    _putenv_s("CIDER_TEST_COMPAT", "legacy");
    CHECK(std::string(orvyn::getenv_compat("ORVYN_TEST_COMPAT", "CIDER_TEST_COMPAT")) == "legacy");
    _putenv_s("ORVYN_TEST_COMPAT", "new");
    CHECK(std::string(orvyn::getenv_compat("ORVYN_TEST_COMPAT", "CIDER_TEST_COMPAT")) == "new");     // the new name wins
    _putenv_s("ORVYN_TEST_COMPAT", "");
    _putenv_s("CIDER_TEST_COMPAT", "");
}
