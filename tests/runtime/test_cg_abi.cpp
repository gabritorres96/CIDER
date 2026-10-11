// CoreGraphics ABI and handle safety (ORVYN 0.6.1): cg_abi_test is a macOS x86_64 program, compiled by clang, that passes and returns CGPoint, CGSize, CGRect and
// CGAffineTransform by value; it verifies every result itself and exits with its failure count. cg_unimplemented imports a real CoreGraphics function ORVYN does
// not implement and must be refused at load time.
#include "../../src/graphics/cg_api.hpp"
#include "fixture_run.hpp"

using namespace orvyn::testing;

ORVYN_TEST(struct_arguments_and_results_follow_the_system_v_abi_and_invalid_handles_are_refused) {
    const uint64_t invalid_before = orvyn::graphics::cg_invalid_handle_events();
    FixtureRun r = run_fixture_file(appkit_fixture("cg_abi_test"));
    expect_exit_code(r, 0, "cg_abi_test");
    CHECK(r.err.find("FAIL") == std::string::npos);
    CHECK(r.err.find("cg_abi_test done: failures=0\n") != std::string::npos);
    CHECK_EQ(orvyn::graphics::cg_invalid_handle_events() - invalid_before, uint64_t{11});     // exactly the invalid calls the program made, no more, no less
    CHECK_EQ(r.r.objc_live_summary, std::string());
    CHECK_EQ(r.r.objc_live_objects, uint64_t{0});                                             // every color, space and path was released
    CHECK_EQ(orvyn::graphics::cg_live_objects(), size_t{0});
}

ORVYN_TEST(a_program_importing_an_unimplemented_coregraphics_function_is_refused_before_it_runs) {
    FixtureRun r = run_fixture_file(appkit_fixture("cg_unimplemented"));
    CHECK(r.r.state == orvyn::guest::State::LoadFailed);
    CHECK(r.r.load_failure == orvyn::loader::LoadFailure::UnresolvedSymbol);
    bool named = false;
    for (const auto& u : r.r.unresolved) named = named || u.symbol == "_CGContextDrawLinearGradient";
    CHECK(named);
    CHECK(r.r.message.find("_CGContextDrawLinearGradient") != std::string::npos);
    CHECK(r.err.find("must never run") == std::string::npos);                                   // the program did not start
}
