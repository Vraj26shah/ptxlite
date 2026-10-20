// TestFramework.h - a deliberately tiny test harness (no external dependency,
// so the project builds anywhere with just CMake and a C++17 compiler).
#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline int runAll() {
    int failedTests = 0;
    for (const auto& t : registry()) {
        const int before = failures();
        t.fn();
        const bool ok = failures() == before;
        if (!ok) ++failedTests;
        std::cout << (ok ? "[  OK  ] " : "[ FAIL ] ") << t.name << '\n';
    }
    std::cout << '\n' << registry().size() - failedTests << '/' << registry().size() << " tests passed\n";
    return failedTests == 0 ? 0 : 1;
}

}  // namespace testing

#define TEST(name)                                                    \
    static void name();                                               \
    static testing::Registrar registrar_##name(#name, name);          \
    static void name()

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::cout << "    " << __FILE__ << ':' << __LINE__ << ": CHECK(" #cond ")" \
                      << " failed\n";                                                  \
            ++testing::failures();                                                     \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                  \
    do {                                                                                \
        const auto va_ = (a);                                                          \
        const auto vb_ = (b);                                                          \
        if (!(va_ == vb_)) {                                                            \
            std::ostringstream os_;                                                     \
            os_ << "    " << __FILE__ << ':' << __LINE__ << ": CHECK_EQ(" #a ", " #b    \
                << ") failed: " << va_ << " != " << vb_ << '\n';                        \
            std::cout << os_.str();                                                     \
            ++testing::failures();                                                      \
        }                                                                               \
    } while (0)
