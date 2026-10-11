// UNMODIFIED external macOS binaries run by ORVYN. The binaries are NOT in the repository: set ORVYN_EXTERNAL_JQ to the path of the original
// `jq-macos-amd64` of jq 1.7.1 (https://github.com/jqlang/jq/releases/download/jq-1.7.1/jq-macos-amd64, MIT licence). The file's SHA-256 is checked
// before anything runs; a different file is skipped, never run. Without the variable these tests are skipped.
//
// The expected outputs are not produced by ORVYN: they come from jq's own documentation (manual examples) or are plain consequences of the jq
// language (arithmetic), so a wrong ORVYN libc/ABI shows up as a mismatch instead of being baked into the expectation.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "orvyn/guest.hpp"
#include "guest_code.hpp"
#include "macho_builder.hpp"
#include "orvyn/process.hpp"
#include "test_framework.hpp"

using namespace orvyn;
using namespace orvyn::testing;
namespace fs = std::filesystem;

namespace {

// ---- SHA-256 (FIPS 180-4) ----------------------------------------------------------------------------------------------------------------------------
std::string sha256_hex(const std::vector<uint8_t>& data) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74,
        0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d,
        0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e,
        0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<uint8_t> m = data;
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t{m[off + 4 * i]} << 24) | (uint32_t{m[off + 4 * i + 1]} << 16) | (uint32_t{m[off + 4 * i + 2]} << 8) | m[off + 4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char out[65];
    for (int i = 0; i < 8; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
    return out;
}

constexpr const char* kJqSha256 = "4155822bbf5ea90f5c79cf254665975eb4274d426d0709770c21774de5407443";

struct Jq {
    fs::path path;
    std::vector<uint8_t> bytes;
};

Jq find_jq() {
    const char* p = std::getenv("ORVYN_EXTERNAL_JQ");
    if (!p || !*p) SKIP("ORVYN_EXTERNAL_JQ is not set (the original jq-macos-amd64 of jq 1.7.1 is not part of the repository)");
    Jq j;
    j.path = fs::path(std::string(p));
    auto b = read_file(j.path, 64u << 20);
    if (!b.ok()) SKIP("ORVYN_EXTERNAL_JQ does not point to a readable file");
    j.bytes = b.value();
    if (sha256_hex(j.bytes) != kJqSha256) SKIP("the file is not the verified jq 1.7.1 macOS x86_64 release (SHA-256 mismatch): not run");
    return j;
}

struct JqRun {
    guest::Outcome r;
    std::string out, err;
};

JqRun run_jq(const Jq& j, std::vector<std::string> args, const fs::path& root = {}) {
    JqRun res;
    guest::GuestConfig cfg;
    cfg.args = {"jq"};
    cfg.args.insert(cfg.args.end(), args.begin(), args.end());
    cfg.image_path = j.path;
    cfg.root = root;
    cfg.sink = [&](int fd, const char* d, size_t n) { (fd == 2 ? res.err : res.out).append(d, n); };
    res.r = guest::run_in_process(ByteView(j.bytes), cfg);
    return res;
}

void expect_ok(const JqRun& r, const std::string& out, const char* what) {
    if (r.r.state != guest::State::Exited) { report_failure(__FILE__, __LINE__, std::string(what) + ": state " + guest::to_string(r.r.state) + " " + r.r.message); return; }
    if (r.r.exit_code != 0) report_failure(__FILE__, __LINE__, std::string(what) + ": exit code " + std::to_string(r.r.exit_code) + ", stderr: " + r.err);
    if (r.out != out) report_failure(__FILE__, __LINE__, std::string(what) + ": stdout was [" + r.out + "], expected [" + out + "]");
}

}  // namespace

ORVYN_TEST(sha256_helper_matches_known_vectors) {
    CHECK_EQ(sha256_hex({}), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    const std::string abc = "abc";
    CHECK_EQ(sha256_hex(std::vector<uint8_t>(abc.begin(), abc.end())), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

ORVYN_TEST(jq_1_7_1_prints_its_version) {
    Jq j = find_jq();
    JqRun r = run_jq(j, {"--version"});
    expect_ok(r, "jq-1.7.1\n", "jq --version");
}

ORVYN_TEST(jq_1_7_1_evaluates_filters_with_the_expected_results) {
    Jq j = find_jq();
    const struct { std::vector<std::string> args; const char* out; } cases[] = {
        {{"-n", "1+2"}, "3\n"},
        {{"-n", "-c", "[1,2,3]|map(.*2)"}, "[2,4,6]\n"},
        {{"-n", "-c", "{\"a\":[1,2.5,\"x\"]}"}, "{\"a\":[1,2.5,\"x\"]}\n"},
        {{"-n", "-r", "\"a,b\"|split(\",\")|join(\"-\")"}, "a-b\n"},
        // jq manual: fromdate / todate
        {{"-n", "\"2015-03-05T23:51:47Z\"|fromdate"}, "1425599507\n"},
        {{"-n", "-r", "1425599507|todate"}, "2015-03-05T23:51:47Z\n"},
        {{"-n", "-c", "1425599507|gmtime"}, "[2015,2,5,23,51,47,4,63]\n"},                  // manual: broken-down time, Thursday, day 63 of the year
        {{"-n", "-r", "1425599507|strftime(\"%A, %B %d, %Y\")"}, "Thursday, March 05, 2015\n"},   // manual strftime example
        {{"-n", "-c", "\"2015 64\"|strptime(\"%Y %j\")|todate|.[0:4]"}, "\"2015\"\n"},
        // math through libm (known exact values)
        {{"-n", "-c", "[16|sqrt, (2|pow(.;10)), (8|cbrt), (1|exp2), (100|log10), (3|floor), (-3.5|fabs)]"}, "[4,1024,2,2,2,3,3.5]\n"},
        {{"-n", "-c", "[(5|tgamma), (0|cosh), (0|sinh), (0|tanh), (1|atan|.*4|.*1000|round)]"}, "[24,1,0,0,3142]\n"},
        {{"-n", "-c", "[range(5)]|add"}, "10\n"},
        {{"-n", "-r", "\"héllo\"|ascii_upcase"}, "HéLLO\n"},                                // jq's ascii_upcase leaves non-ASCII bytes alone
        {{"-n", "-c", "\"test\"|test(\"^t\")"}, "true\n"},                                    // oniguruma (statically linked in jq) runs too
        {{"-n", "-c", "\"abc123\"|[match(\"[0-9]+\").string]"}, "[\"123\"]\n"},
        // number formatting (shortest round-trip doubles), sorting, regex substitution, formats
        {{"-n", "-c", "[1/3, 0.1+0.2, 123456789012, 3.0]"}, "[0.3333333333333333,0.30000000000000004,123456789012,3.0]\n"},
        {{"-n", "-c", "[3,1,2]|sort"}, "[1,2,3]\n"},
        {{"-n", "-c", "[{\"a\":2},{\"a\":1}]|sort_by(.a)"}, "[{\"a\":1},{\"a\":2}]\n"},
        {{"-n", "-c", "\"a1b22c\"|[gsub(\"[0-9]+\";\"#\"), ([scan(\"[0-9]+\")])]"}, "[\"a#b#c\",[\"1\",\"22\"]]\n"},
        {{"-n", "-c", "[1,\"a\"]|@csv"}, "\"1,\\\"a\\\"\"\n"},
        {{"-n", "-r", "@sh \"echo \\(\"a b\")\""}, "echo 'a b'\n"},
        {{"-n", "-c", "\"\xc3\xbcn\xc3\xaf\"|[length, utf8bytelength, @base64]"}, "[3,5,\"w7xuw68=\"]\n"},
        {{"-n", "-c", "[limit(3;range(10))], (reduce range(5) as $i (0;.+$i))"}, "[0,1,2]\n10\n"},
        {{"--arg", "v", "5", "-n", "$v|tonumber+1"}, "6\n"},
    };
    for (const auto& c : cases) {
        JqRun r = run_jq(j, c.args);
        std::string what = "jq";
        for (const auto& a : c.args) what += " " + a;
        expect_ok(r, c.out, what.c_str());
    }
}

ORVYN_TEST(jq_1_7_1_reads_json_files_from_the_guest_root_and_reports_errors_like_jq) {
    Jq j = find_jq();
    TempDir t;
    t.write("in.json", std::vector<uint8_t>{'{', '"', 'b', '"', ':', '2', ',', '"', 'a', '"', ':', '[', '1', ',', '2', ']', '}', '\n'});
    expect_ok(run_jq(j, {"-c", ".", "/in.json"}, t.path()), "{\"b\":2,\"a\":[1,2]}\n", "jq -c . file");
    expect_ok(run_jq(j, {"-S", "-c", ".", "/in.json"}, t.path()), "{\"a\":[1,2],\"b\":2}\n", "jq -S -c . file");
    expect_ok(run_jq(j, {".a|length", "/in.json"}, t.path()), "2\n", "jq .a|length file");
    // a missing file: jq prints an error and exits 2
    JqRun miss = run_jq(j, {".", "/nope.json"}, t.path());
    REQUIRE(miss.r.state == guest::State::Exited);
    CHECK_EQ(miss.r.exit_code, 2);
    CHECK(miss.err.find("nope.json") != std::string::npos);
    CHECK(miss.err.find("No such file or directory") != std::string::npos);
    // a runtime error in the filter: exit code 5 and jq's message
    JqRun div = run_jq(j, {"-n", "1/0"});
    REQUIRE(div.r.state == guest::State::Exited);
    CHECK_EQ(div.r.exit_code, 5);
    CHECK(div.err.find("cannot be divided because the divisor is zero") != std::string::npos);
    // a syntax error: exit code 3
    JqRun bad = run_jq(j, {"-n", "1 +"});
    REQUIRE(bad.r.state == guest::State::Exited);
    CHECK_EQ(bad.r.exit_code, 3);
    CHECK(bad.err.find("syntax error") != std::string::npos);
}

ORVYN_TEST(jq_1_7_1_reads_standard_input_only_when_run_with_stdin) {
    Jq j = find_jq();
    const char* exe = std::getenv("ORVYN_EXE");
    if (!exe || !*exe) SKIP("ORVYN_EXE is not set");
#ifdef _WIN32
    TempDir t;
    auto in = t.write("in.json", std::vector<uint8_t>{'{', '"', 'a', '"', ':', '[', '1', ',', '2', ',', '3', ']', '}', '\n'});
    const std::string orvyn = path_to_utf8(fs::absolute(fs::path(exe)).make_preferred());
    const std::string jq = path_to_utf8(fs::absolute(j.path).make_preferred());
    auto pipeline = [&](const char* flag) {
        // a batch file: cmd.exe feeds the file to ORVYN's own stdin and ORVYN decides whether the guest may see it
        const std::string bat = "@\"" + orvyn + "\" run " + flag + " \"" + jq + "\" -- -c .a[1] < \"" + path_to_utf8(in.make_preferred()) + "\"\r\n";
        auto script = t.write("run.cmd", std::vector<uint8_t>(bat.begin(), bat.end()));
        return run_process(fs::path("C:/Windows/System32/cmd.exe"), {"/d", "/c", path_to_utf8(script.make_preferred())}, std::chrono::seconds(60));
    };
    auto with = pipeline("--stdin");
    REQUIRE(with.ok());
    const std::string& wo = with.value().output;
    if (wo.find("\n2\n") == std::string::npos && wo.find("\n2\r\n") == std::string::npos) report_failure(__FILE__, __LINE__, "jq .a[1] with --stdin did not print 2; size=" + std::to_string(wo.size()) + " exit=" + std::to_string(with.value().exit_code) + " tail: [" + wo.substr(wo.size() > 700 ? wo.size() - 700 : 0) + "]");
    auto without = pipeline("--verbose");                       // no --stdin: the guest sees an empty input, jq prints nothing
    REQUIRE(without.ok());
    CHECK(without.value().output.find("\n2\n") == std::string::npos && without.value().output.find("\n2\r\n") == std::string::npos);
#else
    SKIP("Windows only");
#endif
}
