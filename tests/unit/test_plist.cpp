#include <string>

#include "orvyn/plist.hpp"
#include "macho_builder.hpp"
#include "test_framework.hpp"

using namespace orvyn;
using namespace orvyn::testing;

static std::string wrap(const std::string& body) {
    return "<?xml version=\"1.0\"?>\n<plist version=\"1.0\">" + body + "</plist>";
}
static Result<plist::Value> xml(const std::string& s) { return plist::parse_xml(s); }

ORVYN_TEST(xml_info_plist_roundtrip) {
    auto r = plist::parse_xml(xml_plist({{"CFBundleExecutable", "Example"}, {"CFBundleIdentifier", "com.example.app"}}));
    REQUIRE(r.ok());
    CHECK(r.value().is_dict());
    CHECK_EQ(r.value().get_string("CFBundleExecutable").value_or(""), std::string("Example"));
    CHECK(!r.value().get("Nope"));
}

ORVYN_TEST(xml_all_scalar_types) {
    auto r = xml(wrap("<dict><key>s</key><string>a &amp; b &lt;c&gt; &#65;&#x42;</string><key>i</key><integer>-42</integer>"
                      "<key>h</key><integer>0x10</integer><key>r</key><real>1.5</real><key>t</key><true/><key>f</key><false/>"
                      "<key>d</key><date>2001-01-02T00:00:00Z</date><key>b</key><data>aGVsbG8=</data></dict>"));
    REQUIRE(r.ok());
    auto& v = r.value();
    CHECK_EQ(v.get("s")->s, std::string("a & b <c> AB"));
    CHECK_EQ(v.get("i")->i, int64_t{-42});
    CHECK_EQ(v.get("h")->i, int64_t{16});
    CHECK_EQ(v.get("r")->d, 1.5);
    CHECK(v.get("t")->b);
    CHECK(!v.get("f")->b);
    CHECK_EQ(v.get("d")->d, 86400.0);
    CHECK_EQ(std::string(v.get("b")->data.begin(), v.get("b")->data.end()), std::string("hello"));
}

ORVYN_TEST(xml_nested_containers_comments_cdata) {
    auto r = xml(wrap("<!-- c --><dict><key>a</key><array><string>x</string><dict/><array/></array>"
                      "<key>c</key><string><![CDATA[<raw>]]></string><key>e</key><string/></dict>"));
    REQUIRE(r.ok());
    auto* a = r.value().get("a");
    REQUIRE(a != nullptr);
    CHECK_EQ(a->items.size(), size_t{3});
    CHECK_EQ(r.value().get("c")->s, std::string("<raw>"));
    CHECK_EQ(r.value().get("e")->s, std::string(""));
}

ORVYN_TEST(xml_duplicate_keys_last_wins) {
    auto r = xml(wrap("<dict><key>k</key><string>1</string><key>k</key><string>2</string></dict>"));
    REQUIRE(r.ok());
    CHECK_EQ(r.value().get("k")->s, std::string("2"));
}

ORVYN_TEST(xml_bom_and_whitespace) {
    std::string bom = "\xEF\xBB\xBF  \n<plist><dict/></plist>";
    auto r = plist::parse(ByteView(reinterpret_cast<const uint8_t*>(bom.data()), bom.size()));
    REQUIRE(r.ok());
    CHECK(r.value().is_dict());
}

ORVYN_TEST(xml_malformed_rejected) {
    CHECK_ERR(xml(""), ErrorCode::Malformed);
    CHECK_ERR(xml("<plist>"), ErrorCode::Malformed);
    CHECK_ERR(xml(wrap("<dict><key>a</key>")), ErrorCode::Malformed);
    CHECK_ERR(xml(wrap("<dict><string>x</string></dict>")), ErrorCode::Malformed);          // value without key
    CHECK_ERR(xml(wrap("<integer>abc</integer>")), ErrorCode::Malformed);
    CHECK_ERR(xml(wrap("<integer>99999999999999999999</integer>")), ErrorCode::Malformed);  // > int64
    CHECK_ERR(xml(wrap("<date>yesterday</date>")), ErrorCode::Malformed);
    CHECK_ERR(xml(wrap("<data>!!!</data>")), ErrorCode::Malformed);
    CHECK_ERR(xml(wrap("<string>&bogus;</string>")), ErrorCode::Malformed);
    CHECK_ERR(xml(wrap("<string>&#0;</string>")), ErrorCode::Malformed);
    CHECK_ERR(xml(wrap("<string>&#xD800;</string>")), ErrorCode::Malformed);
    CHECK_ERR(xml(wrap("<foo/>")), ErrorCode::Malformed);
    CHECK_ERR(xml(wrap("<string>a</strong>")), ErrorCode::Malformed);
    CHECK_ERR(xml(wrap("<string>x</string><string>y</string>")), ErrorCode::Malformed);     // two roots
    CHECK_ERR(xml("<plist><string>unterminated"), ErrorCode::Malformed);
    CHECK_ERR(xml("<plist><![CDATA[x"), ErrorCode::Malformed);
}

ORVYN_TEST(xml_depth_and_node_limits) {
    std::string deep, close;
    for (int i = 0; i < 200; ++i) { deep += "<array>"; close += "</array>"; }
    CHECK_ERR(xml(wrap(deep + close)), ErrorCode::LimitExceeded);
    plist::Limits l;
    l.max_nodes = 5;
    std::string many = "<array>";
    for (int i = 0; i < 20; ++i) many += "<true/>";
    many += "</array>";
    CHECK_ERR(plist::parse_xml(wrap(many), l), ErrorCode::LimitExceeded);
    plist::Limits small;
    small.max_input = 10;
    CHECK_ERR(plist::parse_xml(wrap("<dict/>"), small), ErrorCode::TooLarge);
}

ORVYN_TEST(binary_plist_parses) {
    auto s = binary_plist({{"Name", "Example"}, {"Id", "com.x"}});
    CHECK(plist::detect(ByteView(s)) == plist::Format::Binary);
    auto r = plist::parse(ByteView(s));
    REQUIRE(r.ok());
    CHECK_EQ(r.value().get_string("Name").value_or(""), std::string("Example"));
    CHECK_EQ(r.value().get_string("Id").value_or(""), std::string("com.x"));
}

ORVYN_TEST(binary_plist_corruptions_rejected) {
    auto good = binary_plist({{"Name", "Example"}});
    REQUIRE(plist::parse(ByteView(good)).ok());
    for (size_t cut = 0; cut < good.size(); ++cut) {
        std::vector<uint8_t> c(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
        c.resize(cut);
        CHECK(!plist::parse_binary(ByteView(c)).ok() || cut == good.size());
    }
    auto t = good.size() - 32;
    auto b = good;
    b[t + 6] = 0;  // offset int size 0
    CHECK_ERR(plist::parse_binary(ByteView(b)), ErrorCode::Malformed);
    b = good; b[t + 7] = 9;  // ref size 9
    CHECK_ERR(plist::parse_binary(ByteView(b)), ErrorCode::Malformed);
    b = good; b[t + 15] = 0; b[t + 14] = 0xff;  // num objects huge vs table
    CHECK(!plist::parse_binary(ByteView(b)).ok());
    b = good; b[t + 23] = 77;  // top object out of range
    CHECK_ERR(plist::parse_binary(ByteView(b)), ErrorCode::Malformed);
    b = good; b[t + 31] = 2;  // offset table in header
    CHECK(!plist::parse_binary(ByteView(b)).ok());
}

ORVYN_TEST(binary_plist_self_referencing_array_is_bounded) {
    // object 0 = array with one element referencing object 0 (cycle)
    Buf b;
    for (char c : std::string("bplist00")) b.u8(static_cast<uint8_t>(c));
    size_t obj0 = b.size();
    b.u8(0xA1); b.u8(0);
    size_t table = b.size();
    b.u8(static_cast<uint8_t>(obj0));
    for (int i = 0; i < 6; ++i) b.u8(0);
    b.u8(1); b.u8(1);
    b.be64(1); b.be64(0); b.be64(table);
    CHECK_ERR(plist::parse_binary(ByteView(b.v)), ErrorCode::LimitExceeded);
}

ORVYN_TEST(unknown_format_rejected) {
    std::string s = "{ CFBundleName = Foo; }";  // OpenStep: unsupported
    CHECK_ERR(plist::parse(ByteView(reinterpret_cast<const uint8_t*>(s.data()), s.size())), ErrorCode::Unsupported);
}
