// CoreFoundation subset, CoreGraphics geometry and the run loop (CIDER 0.5.5 / 0.5.6): a clang-built program calls the real exported functions with
// the System V conventions of the geometry structures (CGRect on the stack, CGPoint in xmm registers, hidden-pointer results) and runs timers,
// a version-0 source and a main-queue block on the shared run loop.
#include "fixture_run.hpp"

using namespace cider::testing;

CIDER_TEST(corefoundation_geometry_and_runloop_checks_pass_in_the_guest) {
    FixtureRun r = run_fixture_file(objc_fixture("cf_basic"));
    expect_exit_code(r, 0, "cf_basic");
    CHECK(r.err.find("FAIL") == std::string::npos);
    CHECK(r.err.find("cf checks done: 0 failures\n") != std::string::npos);
    CHECK_EQ(r.r.objc_live_objects, uint64_t{0});
}

