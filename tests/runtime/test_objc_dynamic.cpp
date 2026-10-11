// Message forwarding and exceptions (ORVYN 0.5.4): clang-built programs resolve methods dynamically, forward messages to other receivers or through
// NSInvocation, and throw NSExceptions (uncaught only). @try/@catch programs are refused, never run with a broken catch.
#include "fixture_run.hpp"

using namespace orvyn::testing;

ORVYN_TEST(dynamic_resolution_forwarding_targets_and_invocations_work_inside_the_guest) {
    FixtureRun r = run_fixture_file(objc_fixture("objc_forward"));
    expect_exit_code(r, 0, "objc_forward");
    CHECK(r.err.find("FAIL") == std::string::npos);
    CHECK(r.err.find("forward checks done: 0 failures\n") != std::string::npos);
}

ORVYN_TEST(forwarding_a_struct_argument_and_unhandled_selectors_end_the_guest_with_a_diagnosis) {
    FixtureRun s = run_fixture_file(objc_fixture("objc_forward"), {"s"});
    CHECK(s.r.state == orvyn::guest::State::Faulted);
    CHECK(s.r.message.find("is not supported by message forwarding") != std::string::npos);
    FixtureRun u = run_fixture_file(objc_fixture("objc_forward"), {"u"});
    CHECK(u.r.state == orvyn::guest::State::Faulted);
    CHECK(u.r.message.find("-[Worker nobodyHandlesThis]: unrecognized selector sent to instance") != std::string::npos);
}

ORVYN_TEST(nsexception_objects_work_and_an_uncaught_throw_ends_the_guest_like_macos) {
    FixtureRun n = run_fixture_file(objc_fixture("objc_exception"), {"n"});
    expect_exit_code(n, 0, "no throw");
    FixtureRun r = run_fixture_file(objc_fixture("objc_exception"), {"r"});
    CHECK(r.r.state == orvyn::guest::State::Faulted);
    CHECK(r.r.message == "*** Terminating app due to uncaught exception 'ORVYNRaise', reason: 'value 42 and text'");
    FixtureRun t = run_fixture_file(objc_fixture("objc_exception"), {"t"});
    CHECK(t.r.state == orvyn::guest::State::Faulted);
    CHECK(t.r.message == "*** Terminating app due to uncaught exception 'ORVYNError', reason: 'made by hand'");
    CHECK_EQ(t.err, std::string("before throw, objects ok=1\n"));
}

ORVYN_TEST(a_program_that_contains_catch_handlers_loads_and_runs_while_nothing_is_thrown) {
    FixtureRun ok = run_fixture_file(objc_fixture("objc_try_nothrow"));
    expect_exit_code(ok, 42, "try without a throw");
    CHECK(ok.err.find("no throw") != std::string::npos);
    CHECK(ok.err.find("finally") != std::string::npos);          // the @finally body of the normal path ran
    CHECK(ok.err.find("caught") == std::string::npos);
}

ORVYN_TEST(a_throw_in_a_program_with_handlers_stops_with_an_explicit_no_unwinder_diagnostic) {
    // never "caught": ORVYN cannot deliver the exception to the @catch, and does not pretend to
    for (const char* fixture : {"objc_try", "objc_try_nothrow"}) {
        FixtureRun r = fixture == std::string("objc_try") ? run_fixture_file(objc_fixture(fixture)) : run_fixture_file(objc_fixture(fixture), {"throw"});
        if (r.r.state != orvyn::guest::State::Faulted) { report_failure(__FILE__, __LINE__, std::string(fixture) + ": state " + orvyn::guest::to_string(r.r.state)); continue; }
        CHECK(r.r.message.find("ORVYN has no unwinder") != std::string::npos);
        CHECK(r.r.message.find("was thrown") != std::string::npos);
        CHECK(r.err.find("caught") == std::string::npos);
    }
}

ORVYN_TEST(an_uncaught_throw_in_a_program_without_handlers_keeps_the_macos_message) {
    FixtureRun r = run_fixture_file(objc_fixture("objc_exception"), {"r"});
    CHECK(r.r.state == orvyn::guest::State::Faulted);
    CHECK(r.r.message.rfind("*** Terminating app due to uncaught exception", 0) == 0);
}

ORVYN_TEST(calling_the_unwinding_entry_points_directly_stops_the_program_and_never_returns) {
    // 0.6.8 audit: nothing may return a fake result. The program imports all of them, only the requested one is called.
    FixtureRun ok = run_fixture_file(objc_fixture("objc_exc_direct"));
    expect_exit_code(ok, 42, "imported, never called");
    const struct { const char* mode; const char* needle; } cases[] = {
        {"b", "a @catch handler was entered"},     {"e", "a @catch handler was left"},     {"u", "tried to resume unwinding"},
        {"r", "a @catch handler rethrew"},         {"p", "personality routine was called"}};
    for (auto& c : cases) {
        FixtureRun r = run_fixture_file(objc_fixture("objc_exc_direct"), {c.mode});
        if (r.r.state != orvyn::guest::State::Faulted) { report_failure(__FILE__, __LINE__, std::string("mode ") + c.mode + ": state " + orvyn::guest::to_string(r.r.state)); continue; }
        if (r.r.message.find(c.needle) == std::string::npos) report_failure(__FILE__, __LINE__, std::string("mode ") + c.mode + ": message was " + r.r.message);
        CHECK(r.r.message.find("made-up result") != std::string::npos);
        CHECK(r.r.message.find("Public prototype") != std::string::npos);
        CHECK(r.err.find("must never print") == std::string::npos);
        CHECK(r.out.find("must never print") == std::string::npos);
    }
    // throwing nil with handlers present
    FixtureRun n = run_fixture_file(objc_fixture("objc_exc_direct"), {"n"});
    CHECK(n.r.state == orvyn::guest::State::Faulted);
    CHECK(n.r.message.find("nil exception thrown") != std::string::npos);
}
