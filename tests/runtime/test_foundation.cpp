// Foundation subset (CIDER 0.4.6): real Objective-C Mach-O programs compiled by clang against the hand-written Foundation declarations are
// executed through the runtime. The checks live in the guest programs (they exit with the number of failed checks); this file asserts exit
// codes, exact NSLog output, leak counts and the diagnoses of misuse. Nothing is interpreted or precomputed on the host.
#include <cstring>

#include "cider/guest.hpp"
#include "cider/objc.hpp"
#include "guest_code.hpp"
#include "test_framework.hpp"

using namespace cider;
using namespace cider::testing;
namespace fs = std::filesystem;

namespace {

const fs::path kObjc = fs::path(CIDER_OBJC_FIXTURES_DIR);
fs::path bin(const std::string& scenario) { return kObjc / "bin" / scenario / scenario; }

struct Run {
    guest::Outcome r;
    std::string out, err;
};
Run run(const fs::path& exe, std::vector<std::string> args = {}) {
    Run run;
    guest::GuestConfig cfg;
    cfg.args = {path_to_utf8(exe)};
    cfg.args.insert(cfg.args.end(), args.begin(), args.end());
    cfg.image_path = exe;
    cfg.sink = [&](int fd, const char* d, size_t n) { (fd == 2 ? run.err : run.out).append(d, n); };
    auto b = read_file(exe, 32u << 20);
    if (!b.ok()) { report_failure(__FILE__, __LINE__, "fixture missing: " + path_to_utf8(exe)); throw AbortTest{}; }
    run.r = guest::run_in_process(ByteView(b.value()), cfg);
    return run;
}
void expect_exit(const Run& r, int code, const char* what) {
    if (r.r.state != guest::State::Exited) { report_failure(__FILE__, __LINE__, std::string(what) + ": state " + guest::to_string(r.r.state) + " " + r.r.message + " | stderr: " + r.err); return; }
    if (r.r.exit_code != code) report_failure(__FILE__, __LINE__, std::string(what) + ": exit code " + std::to_string(r.r.exit_code) + ", expected " + std::to_string(code) + " | stderr: " + r.err);
}

}  // namespace

CIDER_TEST(the_foundation_reference_program_logs_cider_result_42_and_returns_42) {
    Run r = run(bin("foundation_hello"));
    expect_exit(r, 42, "foundation_hello");
    CHECK_EQ(r.err, std::string("CIDER result: 42\n"));           // NSLog writes the bare message to stderr (no timestamp prefix)
    CHECK_EQ(r.out, std::string());
    CHECK_EQ(r.r.objc_live_objects, uint64_t{0});                  // the pool released everything the literals created
}

CIDER_TEST(nsstring_behaviour_is_checked_by_guest_code) {
    Run r = run(bin("foundation_string"));
    expect_exit(r, 0, "foundation_string");
    CHECK(r.err.find("FAIL") == std::string::npos);
    CHECK(r.err.find("string hello / 42-cstr-obj-3.14-7 / [   42|42   |00042]\n") != std::string::npos);
    CHECK(r.err.find("checks done: 0 failures\n") != std::string::npos);
    CHECK_EQ(r.r.objc_live_objects, uint64_t{0});
}

CIDER_TEST(collections_and_numbers_are_checked_by_guest_code) {
    Run r = run(bin("foundation_collections"));
    expect_exit(r, 0, "foundation_collections");
    CHECK(r.err.find("FAIL") == std::string::npos);
    CHECK(r.err.find("collections: (\n    1,\n    two,\n    3\n) two\n") != std::string::npos);
    CHECK_EQ(r.r.objc_live_objects, uint64_t{0});
}

CIDER_TEST(guest_subclasses_of_nsobject_balance_retains_and_pools) {
    Run r = run(bin("foundation_subclass"));
    expect_exit(r, 0, "foundation_subclass");
    CHECK(r.err.find("FAIL") == std::string::npos);
    CHECK(r.err.find("p=(1,2) q=(1,2) dv=D(5,6) arr=(\n    (1,2)\n)\n") != std::string::npos);   // %@ calls the guest -description overrides
    CHECK_EQ(r.r.objc_live_objects, uint64_t{0});
}

CIDER_TEST(foundation_misuse_ends_the_guest_with_an_exception_style_diagnosis) {
    const struct { const char* mode; const char* needle; } cases[] = {
        {"a", "'NSRangeException', reason: '*** -[NSArray objectAtIndex:]: index 5 beyond bounds [0 .. 1]'"},
        {"n", "'NSInvalidArgumentException', reason: '*** -[NSMutableArray addObject:]: object cannot be nil'"},
        {"k", "-[NSMutableDictionary setObject:forKey:]: key cannot be nil"},
        {"v", "-[NSMutableDictionary setObject:forKey:]: object cannot be nil (key: k)"},
        {"s", "'NSRangeException', reason: '-[NSString substringToIndex:]: Range {0, 9} out of bounds; string length 3'"},
        {"u", "-[NSConstantString noSuchMethod]: unrecognized selector sent to instance"},
        {"l", "attempt to insert nil object from objects[1]"},
        {"f", "%n is never honoured"},
        {"c", "-[NSString characterAtIndex:]: Range or index out of bounds: index 3 beyond bounds [0 .. 2]"},
        {"r", "-[NSMutableArray removeLastObject]: index 0 beyond bounds for empty array"}};
    for (auto& c : cases) {
        Run r = run(bin("foundation_error"), {c.mode});
        if (r.r.state != guest::State::Faulted) { report_failure(__FILE__, __LINE__, std::string("mode ") + c.mode + ": not faulted, state " + guest::to_string(r.r.state)); continue; }
        if (r.r.message.find(c.needle) == std::string::npos) report_failure(__FILE__, __LINE__, std::string("mode ") + c.mode + ": message was " + r.r.message);
        CHECK_EQ(r.err, std::string("before\n"));                  // the failing call is the last thing the guest did
    }
}

CIDER_TEST(foundation_program_without_the_selector_is_not_faked) {
    // The program only runs because the selectors exist in the runtime's tables: a class that is NOT part of the subset is not linked.
    auto bytes = read_file(bin("foundation_hello"), 32u << 20);
    CHECK(bytes.ok());
    if (!bytes.ok()) return;
    const std::string text(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
    CHECK(text.find("Foundation.framework/Versions/C/Foundation") != std::string::npos);     // really linked against Foundation
    CHECK(text.find("_OBJC_CLASS_$_NSArray") != std::string::npos || text.find("NSArray") != std::string::npos);
}
