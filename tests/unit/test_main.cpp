#include <cstring>

#include "cider/log.hpp"
#include "test_framework.hpp"

namespace cider::testing {
std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
int& failure_count() { static int n = 0; return n; }
}  // namespace cider::testing

int main(int argc, char** argv) {
    using namespace cider::testing;
    cider::log::set_level(cider::log::Level::Off);  // tests deliberately feed corrupt data: keep stderr quiet
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0, failed = 0, skipped = 0;
    for (auto& t : registry()) {
        if (filter && !std::strstr(t.name, filter)) continue;
        int before = failure_count();
        try { t.fn(); }
        catch (const AbortTest&) {}
        catch (const SkipTest&) {
            ++skipped;
            std::printf("[SKIP] %s\n", t.name);
            continue;
        }
        catch (const std::exception& e) { report_failure(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what()); }
        ++ran;
        bool bad = failure_count() != before;
        failed += bad;
        std::printf("[%s] %s\n", bad ? "FAIL" : " ok ", t.name);
    }
    std::printf("%d test(s) run, %d failed, %d skipped\n", ran, failed, skipped);
    return failed || (ran == 0 && skipped == 0) ? 1 : 0;
}
