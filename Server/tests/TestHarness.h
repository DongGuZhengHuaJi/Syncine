#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test {

using TestFunction = void (*)();

struct TestCase {
    std::string name;
    TestFunction function;
};

inline std::vector<TestCase> &registry() {
    static std::vector<TestCase> tests;
    return tests;
}

class Registrar {
public:
    Registrar(std::string name, TestFunction function) {
        registry().push_back({std::move(name), function});
    }
};

class Failure : public std::runtime_error {
public:
    explicit Failure(const std::string &message)
        : std::runtime_error(message) {
    }
};

inline void check(bool condition, const char *expression,
                  const char *file, int line) {
    if (condition)
        return;

    std::ostringstream message;
    message << file << ':' << line << ": CHECK failed: " << expression;
    throw Failure(message.str());
}

inline int runAll(const std::string &filter = {}) {
    int failures = 0;
    int executed = 0;
    for (const TestCase &test : registry()) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos)
            continue;
        ++executed;
        try {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception &error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << executed << " test(s) passed\n";
    return 0;
}

} // namespace test

#define TEST_CASE(name)                                                        \
    static void name();                                                        \
    static ::test::Registrar registrar_##name(#name, &name);                   \
    static void name()

#define CHECK(expression)                                                      \
    ::test::check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

