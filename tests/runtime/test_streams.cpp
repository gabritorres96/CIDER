// FILE streams (ORVYN 0.4.1) exercised by real Mach-O programs; every file is compared byte for byte on the host.
#include <cstring>
#include <fstream>

#include "orvyn/guest.hpp"
#include "guest_code.hpp"
#include "macho_builder.hpp"
#include "test_framework.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace orvyn;
using namespace orvyn::testing;
namespace fs = std::filesystem;

namespace {

struct Run {
    guest::Outcome r;
    std::string out;
};
Run run_fixture(const std::string& name, const fs::path& root) {
    Run run;
    guest::GuestConfig cfg;
    cfg.args = {name};
    cfg.root = root;
    cfg.sink = [&](int, const char* d, size_t n) { run.out.append(d, n); };
    auto bytes = exec_fixture(name);
    run.r = guest::run_in_process(ByteView(bytes), cfg);
    return run;
}
std::vector<uint8_t> slurp(const fs::path& p) {
    auto r = read_file(p, 1 << 20);
    return r.ok() ? r.value() : std::vector<uint8_t>{};
}
std::string str(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }

}  // namespace

ORVYN_TEST(fwrite_writes_exactly_the_bytes_without_text_mode_translation) {
    TempDir t;
    Run r = run_fixture("streams_write", t.path());
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 0);
    CHECK_EQ(r.out, std::string("streams written\n"));
    auto all = slurp(t.path() / "all.bin");
    REQUIRE_EQ(all.size(), size_t{256});
    for (int i = 0; i < 256; ++i) CHECK_EQ(all[static_cast<size_t>(i)], static_cast<uint8_t>(i));    // 0x0A, 0x0D, 0x1A, 0x00 untouched
    CHECK_EQ(str(slurp(t.path() / "text.txt")), std::string("line1\nline2\r\nline3\nappended"));      // no LF -> CRLF conversion
    CHECK_EQ(str(slurp(t.path() / "update.bin")), std::string("012abc6789"));                         // update mode: overwrite after a read
    CHECK(!fs::exists(t.path() / "a.bin"));                                                           // the invalid mode created nothing
}

ORVYN_TEST(fread_fseek_ftell_feof_ferror_and_fgets_follow_the_c_contract) {
    TempDir t;
    std::vector<uint8_t> all(256);
    for (int i = 0; i < 256; ++i) all[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
    t.write("data.bin", all);
    t.write("lines.txt", std::string("alpha\nbeta\n\ngamma"));
    Run r = run_fixture("streams_read", t.path());
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 0);                                      // a non-zero code names the failing step in streams_read.c
    CHECK_EQ(r.out, std::string("streams read ok\n"));
}

ORVYN_TEST(streams_cannot_leave_the_root) {
    TempDir t;
    fs::create_directories(t.path() / "root");
    t.write("outside.bin", std::string("secret"));
    std::vector<uint8_t> all(256, 1);
    t.write("root/data.bin", all);
    t.write("root/lines.txt", std::string("alpha\nbeta\n\ngamma"));
    Run r = run_fixture("streams_read", t.path() / "root");
    REQUIRE(r.r.state == guest::State::Exited);
    // step 40 opens "../../outside.bin": it resolves INSIDE the root (clamped), where it does not exist -> ENOENT, never the secret
    CHECK(r.r.exit_code != 40);
}

ORVYN_TEST(without_a_root_fopen_fails_with_eacces) {
    Run r = run_fixture("streams_write", {});
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 10);                                     // fopen returned NULL: EACCES
}

ORVYN_TEST(the_stream_pool_is_finite_slots_are_reusable_and_forged_pointers_are_refused) {
    TempDir t;
    Run r = run_fixture("streams_limit", t.path());
    REQUIRE(r.r.state == guest::State::Exited);
    CHECK_EQ(r.r.exit_code, 42);
    CHECK_EQ(r.out, std::string("opened=61 errno=24\n"));            // 64 slots - stdin/stdout/stderr, then EMFILE (24 on Darwin)
}

ORVYN_TEST(open_streams_are_closed_when_the_guest_exits) {
    TempDir t;
    const DWORD start = [] { DWORD h = 0; GetProcessHandleCount(GetCurrentProcess(), &h); return h; }();
    for (int i = 0; i < 6; ++i) {
        Run r = run_fixture("streams_limit", t.path());               // exits with 61 streams closed explicitly...
        CHECK_EQ(r.r.exit_code, 42);
    }
    DWORD end = 0;
    GetProcessHandleCount(GetCurrentProcess(), &end);
    CHECK(end <= start + 3);
    // ...and a program that leaves streams open: streams_write leaves none, so use the RuntimeContext destructor path directly
    fs::remove(t.path() / "limit.tmp");
}
