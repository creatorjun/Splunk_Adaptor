// tests/TestSupport.hpp
#pragma once

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace test {

inline void check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        throw std::runtime_error(std::string(file) + ":" + std::to_string(line) + ": " + expression);
    }
}

inline bool near(double left, double right, double tolerance = 0.000001) {
    return std::abs(left - right) <= tolerance;
}

template <typename Function>
int run(Function function) {
    try {
        function();
        std::cout << "PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

}

#define CHECK(expression) test::check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
