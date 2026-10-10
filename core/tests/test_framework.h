// SPDX-License-Identifier: GPL-3.0-or-later
// Tiny dependency-free test harness (the sandbox and macOS CI need nothing extra).
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace sdrstar_test {
struct Case { std::string name; std::function<void()> fn; };
inline std::vector<Case>& cases() { static std::vector<Case> c; return c; }
inline int& failures() { static int f = 0; return f; }
inline int& checks() { static int c = 0; return c; }
struct Registrar { Registrar(const char* n, std::function<void()> f) { cases().push_back({n, f}); } };
inline int runAll(const char* suite) {
    int failedCases = 0;
    for (auto& c : cases()) {
        int before = failures();
        c.fn();
        bool ok = failures() == before;
        std::printf("[%s] %s::%s\n", ok ? " OK " : "FAIL", suite, c.name.c_str());
        if (!ok) { failedCases++; }
    }
    std::printf("%s: %zu cases, %d checks, %d failed checks\n", suite, cases().size(), checks(), failures());
    return failedCases ? 1 : 0;
}
}  // namespace sdrstar_test

#define TEST(name) \
    static void test_##name(); \
    static sdrstar_test::Registrar reg_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond) do { sdrstar_test::checks()++; if (!(cond)) { sdrstar_test::failures()++; \
    std::printf("  CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { sdrstar_test::checks()++; auto _a = (a); auto _b = (b); if (!(_a == _b)) { sdrstar_test::failures()++; \
    std::printf("  CHECK_EQ failed %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)
#define CHECK_NEAR(a, b, tol) do { sdrstar_test::checks()++; double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (tol))) { sdrstar_test::failures()++; \
    std::printf("  CHECK_NEAR failed %s:%d: %s=%.6g vs %s=%.6g (tol %g)\n", __FILE__, __LINE__, #a, _a, #b, _b, (double)(tol)); } } while (0)
#define CHECK_CONTAINS(hay, needle) do { sdrstar_test::checks()++; if ((hay).find(needle) == std::string::npos) { sdrstar_test::failures()++; \
    std::printf("  CHECK_CONTAINS failed %s:%d: \"%s\" not in \"%s\"\n", __FILE__, __LINE__, needle, (hay).c_str()); } } while (0)

#define TEST_MAIN(suite) int main() { return sdrstar_test::runAll(suite); }
