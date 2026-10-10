// Message forwarding and exceptions (CIDER 0.5.4): clang-built programs resolve methods dynamically, forward messages to other receivers or through
// NSInvocation, and throw NSExceptions (uncaught only). @try/@catch programs are refused, never run with a broken catch.
#include "fixture_run.hpp"

using namespace cider::testing;

CIDER_TEST(dynamic_resolution_forwarding_targets_and_invocations_work_inside_the_guest) {
    FixtureRun r = run_fixture_file(objc_fixture("objc_forward"));
    expect_exit_code(r, 0, "objc_forward");
    CHECK(r.err.find("FAIL") == std::string::npos);
    CHECK(r.err.find("forward checks done: 0 failures\n") != std::string::npos);
}

CIDER_TEST(forwarding_a_struct_argument_and_unhandled_selectors_end_the_guest_with_a_diagnosis) {
    FixtureRun s = run_fixture_file(objc_fixture("objc_forward"), {"s"});
    CHECK(s.r.state == cider::guest::State::Faulted);
    CHECK(s.r.message.find("is not supported by message forwarding") != std::string::npos);
    FixtureRun u = run_fixture_file(objc_fixture("objc_forward"), {"u"});
    CHECK(u.r.state == cider::guest::State::Faulted);
    CHECK(u.r.message.find("-[Worker nobodyHandlesThis]: unrecognized selector sent to instance") != std::string::npos);
}

CIDER_TEST(nsexception_objects_work_and_an_uncaught_throw_ends_the_guest_like_macos) {
    FixtureRun n = run_fixture_file(objc_fixture("objc_exception"), {"n"});
    expect_exit_code(n, 0, "no throw");
    FixtureRun r = run_fixture_file(objc_fixture("objc_exception"), {"r"});
    CHECK(r.r.state == cider::guest::State::Faulted);
    CHECK(r.r.message == "*** Terminating app due to uncaught exception 'CIDERRaise', reason: 'value 42 and text'");
    FixtureRun t = run_fixture_file(objc_fixture("objc_exception"), {"t"});
    CHECK(t.r.state == cider::guest::State::Faulted);
    CHECK(t.r.message == "*** Terminating app due to uncaught exception 'CIDERError', reason: 'made by hand'");
    CHECK_EQ(t.err, std::string("before throw, objects ok=1\n"));
}

CIDER_TEST(a_program_that_catches_exceptions_is_refused_instead_of_run_with_a_broken_catch) {
    FixtureRun r = run_fixture_file(objc_fixture("objc_try"));
    CHECK(r.r.state != cider::guest::State::Exited);
    CHECK(r.err.empty());                                        // no guest instruction ran
}
