// Blocks ABI subset (ORVYN 0.5.1): clang-built Objective-C programs create, copy, release and invoke real blocks; the runtime invokes blocks
// handed to Foundation; misuse is diagnosed. The checks live in the guest program (exit code = failed checks).
#include "fixture_run.hpp"

using namespace orvyn::testing;

ORVYN_TEST(blocks_global_stack_heap_byref_object_captures_and_runtime_invocation_pass_in_the_guest) {
    FixtureRun r = run_fixture_file(objc_fixture("blocks_basic"));
    expect_exit_code(r, 0, "blocks_basic");
    CHECK(r.err.find("FAIL") == std::string::npos);
    CHECK(r.err.find("blocks checks done: 0 failures\n") != std::string::npos);
    CHECK_EQ(r.r.objc_live_objects, uint64_t{0});
}

ORVYN_TEST(block_misuse_is_diagnosed) {
    const struct { const char* mode; const char* needle; } cases[] = {
        {"r", "_Block_release of"}, {"g", "which is not a heap block"}, {"c", "is not readable memory"}};
    for (auto& c : cases) {
        FixtureRun r = run_fixture_file(objc_fixture("blocks_error"), {c.mode});
        if (r.r.state != orvyn::guest::State::Faulted) { report_failure(__FILE__, __LINE__, std::string("mode ") + c.mode + ": not faulted"); continue; }
        if (r.r.message.find(c.needle) == std::string::npos) report_failure(__FILE__, __LINE__, std::string("mode ") + c.mode + ": " + r.r.message);
        CHECK_EQ(r.err, std::string("before\n"));
    }
    FixtureRun n = run_fixture_file(objc_fixture("blocks_error"), {"n"});      // NULL is allowed everywhere
    expect_exit_code(n, 7, "copy/release of NULL");
}

ORVYN_TEST(the_block_fixture_is_a_real_macho_with_block_helpers) {
    auto b = orvyn::read_file(objc_fixture("blocks_basic"), 64u << 20);
    CHECK(b.ok());
    if (!b.ok()) return;
    const std::string text(reinterpret_cast<const char*>(b.value().data()), b.value().size());
    CHECK(text.find("__Block_object_assign") != std::string::npos);
    CHECK(text.find("__NSConcreteStackBlock") != std::string::npos);
}
