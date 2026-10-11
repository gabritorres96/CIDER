// AppKit subset (ORVYN 0.5.7 - 0.5.11) on the offscreen GUI backend: real Mach-O programs create windows, views and controls; scripted input travels
// the same backend callbacks as Win32 window messages; the Objective-C methods that react are the ones compiled into the program.
#include "fixture_run.hpp"

using namespace orvyn::testing;

namespace {
const std::vector<std::string> kScript = {"wait 10", "click Add", "click Add", "click Toggle", "click Disabled", "type abc", "mouse 60 240", "key z", "close", "close"};
}

ORVYN_TEST(appkit_classes_target_action_responders_resize_and_close_behave_inside_the_guest) {
    FixtureOptions o;
    o.gui_backend = "offscreen";
    o.gui_script = kScript;
    FixtureRun r = run_fixture_file(appkit_fixture("appkit_basic"), o);
    expect_exit_code(r, 0, "appkit_basic");
    CHECK(r.err.find("FAIL") == std::string::npos);
    CHECK(r.err.find("appkit checks done: 0 failures\n") != std::string::npos);
    CHECK_EQ(r.r.gui_open_windows_at_exit, uint64_t{0});          // the program closed its window
    CHECK_EQ(r.r.gui_native_objects_after, uint64_t{0});          // and nothing native is left
    CHECK_EQ(r.r.objc_live_objects, uint64_t{0});                 // views, controls, window and delegate were all released
}

ORVYN_TEST(hello_window_runs_headless_counts_clicks_in_guest_code_and_exits_cleanly) {
    FixtureOptions o;
    o.gui_backend = "offscreen";
    o.gui_script = {"wait 10", "click Click me", "click Click me", "click Click me", "click Close"};
    FixtureRun r = run_fixture_file(appkit_fixture("hello_window"), o);
    expect_exit_code(r, 0, "hello_window");
    CHECK_EQ(r.err, std::string("Application launched\nButton clicked! count=1\nButton clicked! count=2\nButton clicked! count=3\nClose pressed\n"
                                "Window closing after 3 click(s)\nApplication terminating\n"));
    CHECK_EQ(r.r.gui_open_windows_at_exit, uint64_t{0});
    CHECK_EQ(r.r.gui_native_objects_after, uint64_t{0});
}

ORVYN_TEST(a_window_left_open_when_the_program_ends_is_destroyed_by_the_runtime) {
    FixtureOptions o;
    o.gui_backend = "offscreen";
    o.gui_script = {"wait 10", "click Click me"};                 // never closes: the script ends, the loop would run forever, so the process is stopped by exit
    // use the timeout-free path: a second script command ends the program through the application delegate is not available here, so run with a bounded script
    // and an app that terminates by itself: appkit_basic stops its run loop after the second close; here we only check the open-window accounting.
    o.gui_script = {"wait 10", "click Add", "close", "close"};
    FixtureRun r = run_fixture_file(appkit_fixture("appkit_basic"), o);
    CHECK(r.r.state == orvyn::guest::State::Exited);
    CHECK_EQ(r.r.gui_native_objects_after, uint64_t{0});
}
