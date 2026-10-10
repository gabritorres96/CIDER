// libdispatch subset (CIDER 0.5.2): clang-built Objective-C programs use the main queue, serial/global queues, sync/async/after/once, groups,
// semaphores and apply; the runtime executes blocks on host-implemented guest threads while main-queue blocks stay on the main thread.
#include "fixture_run.hpp"

using namespace cider::testing;

CIDER_TEST(queues_groups_semaphores_once_after_and_worker_to_main_sync_pass_in_the_guest) {
    FixtureRun r = run_fixture_file(objc_fixture("dispatch_basic"));
    expect_exit_code(r, 0, "dispatch_basic");
    CHECK(r.err.find("FAIL") == std::string::npos);
    CHECK(r.err.find("order: m1,m2,s0,s1,s2,sync,notified,fromworker,immediate,after\n") != std::string::npos);
    CHECK(r.err.find("dispatch checks done: 0 failures\n") != std::string::npos);
    CHECK_EQ(r.r.objc_live_objects, uint64_t{0});          // queues, groups, semaphores and captured objects were all released
}

CIDER_TEST(the_scheduler_gives_the_same_result_over_repeated_runs_without_leaking_threads) {
    for (int i = 0; i < 8; ++i) {
        FixtureRun r = run_fixture_file(objc_fixture("dispatch_basic"));
        expect_exit_code(r, 0, "dispatch_basic (repeat)");
        if (r.err.find("FAIL") != std::string::npos) { report_failure(__FILE__, __LINE__, "run " + std::to_string(i) + ": " + r.err); break; }
    }
}

CIDER_TEST(libdispatch_misuse_is_diagnosed_and_exit_from_a_worker_block_ends_the_process) {
    const struct { const char* mode; const char* needle; } cases[] = {
        {"s", "dispatch_sync onto the main queue from the main thread would deadlock"},
        {"q", "from a block already running on it would deadlock"},
        {"b", "the block is NULL or unreadable"},
        {"o", "the predicate is not writable memory"},
        {"g", "unbalanced call"}};
    for (auto& c : cases) {
        FixtureRun r = run_fixture_file(objc_fixture("dispatch_error"), {c.mode});
        if (r.r.state != cider::guest::State::Faulted) { report_failure(__FILE__, __LINE__, std::string("mode ") + c.mode + ": not faulted, state " + cider::guest::to_string(r.r.state)); continue; }
        if (r.r.message.find(c.needle) == std::string::npos) report_failure(__FILE__, __LINE__, std::string("mode ") + c.mode + ": " + r.r.message);
        CHECK_EQ(r.err, std::string("before\n"));
    }
    FixtureRun x = run_fixture_file(objc_fixture("dispatch_error"), {"x"});
    expect_exit_code(x, 33, "exit() inside a serial-queue block");
}
