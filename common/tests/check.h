#pragma once

// Minimal test harness (no external dependency): TEST(name) registers a function, CHECK(cond)
// records a failure and continues, REQUIRE(cond) stops the current test.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace check {

struct Test {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Test>& Registry() {
    static std::vector<Test> r;
    return r;
}
inline int& Failures() {
    static int f = 0;
    return f;
}
struct Abort {};

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { Registry().push_back({name, std::move(fn)}); }
};

inline int RunAll(const char* filter) {
    int failed_tests = 0, run = 0;
    for (auto& t : Registry()) {
        if (filter && !std::string(t.name).starts_with(filter)) continue;
        int before = Failures();
        ++run;
        try {
            t.fn();
        } catch (const Abort&) {
        } catch (const std::exception& e) {
            std::printf("  exception: %s\n", e.what());
            ++Failures();
        }
        bool ok = Failures() == before;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", t.name);
        if (!ok) ++failed_tests;
    }
    std::printf("%d/%d tests passed\n", run - failed_tests, run);
    return failed_tests == 0 ? 0 : 1;
}

}  // namespace check

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)
#define TEST(name)                                                              \
    static void name();                                                         \
    static ::check::Registrar CHECK_CAT(reg_, name)(#name, name);               \
    static void name()
#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++::check::Failures();                                              \
        }                                                                       \
    } while (0)
#define REQUIRE(cond)                                                           \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("  %s:%d: REQUIRE(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++::check::Failures();                                              \
            throw ::check::Abort{};                                             \
        }                                                                       \
    } while (0)
