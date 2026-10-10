// Foundation, second batch (CIDER 0.5.3): NSMutableString, NSData, NSDate, NSURL, NSError, NSFileManager checked from guest code, with the file part
// confined to a temporary --root, plus host-side verification of what the guest wrote.
#include <fstream>

#include "fixture_run.hpp"
#include "macho_builder.hpp"

using namespace cider::testing;

CIDER_TEST(mutable_strings_data_dates_urls_errors_and_files_are_checked_by_guest_code) {
    TempDir t;
    FixtureRun r = run_fixture_file(objc_fixture("foundation_extra"), {}, t.path());
    expect_exit_code(r, 0, "foundation_extra");
    CHECK(r.err.find("FAIL") == std::string::npos);
    CHECK(r.err.find("extra checks done: 0 failures\n") != std::string::npos);
    CHECK_EQ(r.r.objc_live_objects, uint64_t{0});
}

CIDER_TEST(the_guest_removes_its_work_directory_and_the_root_itself_survives) {
    TempDir t;
    FixtureRun r = run_fixture_file(objc_fixture("foundation_extra"), {}, t.path());
    expect_exit_code(r, 0, "foundation_extra");
    std::error_code ec;
    CHECK(!std::filesystem::exists(t.path() / "work", ec));          // removed by the guest through NSFileManager
    CHECK(std::filesystem::is_directory(t.path(), ec));
}

CIDER_TEST(without_a_root_file_operations_fail_instead_of_touching_the_host) {
    FixtureRun r = run_fixture_file(objc_fixture("foundation_extra"));       // no --root: createDirectory/createFile fail, so the guest reports failures
    CHECK(r.r.state == cider::guest::State::Exited);
    CHECK(r.r.exit_code != 0);
    CHECK(r.err.find("FAIL") != std::string::npos);
}
