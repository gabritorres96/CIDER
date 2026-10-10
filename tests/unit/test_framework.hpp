#pragma once
// Minimal self-contained test harness (no third-party dependency, works offline on CI).
#include <cstdio>
#include <exception>
#include <functional>
#include <sstream>
#include <type_traits>
#include <string>
#include <vector>

namespace cider::testing {

struct TestCase {
    const char* name;
    void (*fn)();
};
std::vector<TestCase>& registry();
int& failure_count();
struct Registrar {
    Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); }
};
struct AbortTest {};
struct SkipTest {};

inline void report_failure(const char* file, int line, const std::string& msg) {
    ++failure_count();
    std::fprintf(stderr, "    FAILED %s:%d: %s\n", file, line, msg.c_str());
}

template <class T>
void show(std::ostringstream& o, const T& v) {
    if constexpr (std::is_enum_v<T>) o << static_cast<long long>(v);
    else o << v;
}

template <class A, class B>
std::string describe(const A& a, const B& b, const char* ea, const char* eb) {
    std::ostringstream o;
    o << ea << " == " << eb << "  (lhs=";
    show(o, a);
    o << ", rhs=";
    show(o, b);
    o << ")";
    return o.str();
}

}  // namespace cider::testing

#define CIDER_TEST(name)                                                         \
    static void name();                                                          \
    static ::cider::testing::Registrar cider_reg_##name(#name, name);            \
    static void name()

#define CHECK(cond)                                                                       \
    do { if (!(cond)) ::cider::testing::report_failure(__FILE__, __LINE__, #cond); } while (0)
#define REQUIRE(cond)                                                                     \
    do { if (!(cond)) { ::cider::testing::report_failure(__FILE__, __LINE__, #cond); throw ::cider::testing::AbortTest{}; } } while (0)
#define CHECK_EQ(a, b)                                                                    \
    do { auto cider_a = (a); auto cider_b = (b);                                      \
         if (!(cider_a == cider_b)) ::cider::testing::report_failure(__FILE__, __LINE__, ::cider::testing::describe(cider_a, cider_b, #a, #b)); } while (0)
#define REQUIRE_EQ(a, b)                                                                  \
    do { auto cider_a = (a); auto cider_b = (b);                                      \
         if (!(cider_a == cider_b)) { ::cider::testing::report_failure(__FILE__, __LINE__, ::cider::testing::describe(cider_a, cider_b, #a, #b)); throw ::cider::testing::AbortTest{}; } } while (0)
// Expects a Result/Status to be an error with the given code.
#define CHECK_ERR(expr, code_)                                                            \
    do { auto cider_r = (expr);                                                           \
         if (cider_r.ok()) ::cider::testing::report_failure(__FILE__, __LINE__, std::string(#expr) + " unexpectedly succeeded"); \
         else if (cider_r.error().code != (code_)) ::cider::testing::report_failure(__FILE__, __LINE__, std::string(#expr) + " failed with " + ::cider::to_string(cider_r.error().code) + ": " + cider_r.error().message); } while (0)

// Marks the running test as skipped (e.g. an optional external tool is not installed).
#define SKIP(msg) \
    do { std::printf("    SKIPPED: %s\n", msg); throw ::cider::testing::SkipTest{}; } while (0)
