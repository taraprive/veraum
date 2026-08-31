#pragma once

// Ultra-light header-only test framework. No external dependency; keep it
// trivial so the scaffold builds everywhere, including plain g++/clang.

#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace testfw {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline int failures = 0;

inline void check(bool cond, const char* file, int line, const std::string& expr) {
    if (!cond) {
        ++failures;
        std::cerr << "  FAIL " << file << ":" << line << "  " << expr << "\n";
    }
}

inline int runAll() {
    int ran = 0;
    for (auto& t : registry()) {
        std::cout << "[ RUN  ] " << t.name << std::endl;  // endl: flush so a
        t.fn();                                          // hanging test shows up
        ++ran;
    }
    std::cout << "\n" << (ran - failures) << "/" << ran << " passed, "
              << failures << " failed" << std::endl;
    return failures == 0 ? 0 : 1;
}

}  // namespace testfw

#define TEST(name)                                        \
    static void test_##name();                            \
    static ::testfw::Registrar reg_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond) ::testfw::check((cond), __FILE__, __LINE__, #cond)
#define CHECK_NEAR(a, b, eps) \
    ::testfw::check(std::fabs((a) - (b)) <= (eps), __FILE__, __LINE__, #a " ~= " #b)
