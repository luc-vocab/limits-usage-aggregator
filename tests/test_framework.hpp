#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <cmath>
#include <sstream>
#include <type_traits>

namespace test {

// Helper to detect if a type is streamable
template<typename T, typename = void>
struct is_streamable : std::false_type {};

template<typename T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<T>())>> : std::true_type {};

template<typename T>
inline constexpr bool is_streamable_v = is_streamable<T>::value;

// Helper to convert value to string for error messages
template<typename T>
std::string to_string_helper(const T& value) {
    if constexpr (is_streamable_v<T>) {
        std::ostringstream oss;
        oss << value;
        return oss.str();
    } else if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<std::underlying_type_t<T>>(value));
    } else {
        return "[non-printable]";
    }
}

// Test result tracking
struct TestResult {
    std::string name;
    bool passed;
    std::string message;
};

class TestSuite {
private:
    std::string suite_name_;
    std::vector<TestResult> results_;
    int passed_ = 0;
    int failed_ = 0;

public:
    explicit TestSuite(const std::string& name) : suite_name_(name) {}

    void run_test(const std::string& test_name, std::function<void()> test_func) {
        TestResult result;
        result.name = test_name;
        try {
            test_func();
            result.passed = true;
            passed_++;
        } catch (const std::exception& e) {
            result.passed = false;
            result.message = e.what();
            failed_++;
        }
        results_.push_back(result);
    }

    void print_results() const {
        std::cout << "\n=== " << suite_name_ << " ===" << std::endl;
        for (const auto& result : results_) {
            std::cout << (result.passed ? "[PASS] " : "[FAIL] ") << result.name;
            if (!result.passed && !result.message.empty()) {
                std::cout << " - " << result.message;
            }
            std::cout << std::endl;
        }
        std::cout << "Passed: " << passed_ << "/" << (passed_ + failed_) << std::endl;
    }

    int passed() const { return passed_; }
    int failed() const { return failed_; }
};

// Test assertion exception
class AssertionError : public std::runtime_error {
public:
    explicit AssertionError(const std::string& msg) : std::runtime_error(msg) {}
};

// Assertion helpers
inline void assert_true(bool condition, const std::string& message = "Expected true") {
    if (!condition) {
        throw AssertionError(message);
    }
}

inline void assert_false(bool condition, const std::string& message = "Expected false") {
    if (condition) {
        throw AssertionError(message);
    }
}

template<typename T>
void assert_equal(const T& expected, const T& actual, const std::string& message = "") {
    if (expected != actual) {
        std::string error_msg = "Expected: " + to_string_helper(expected) +
                                ", Actual: " + to_string_helper(actual);
        if (!message.empty()) {
            error_msg += " (" + message + ")";
        }
        throw AssertionError(error_msg);
    }
}

inline void assert_double_equal(double expected, double actual, double epsilon = 1e-9,
                                 const std::string& message = "") {
    if (std::abs(expected - actual) > epsilon) {
        std::ostringstream oss;
        oss << "Expected: " << expected << ", Actual: " << actual;
        if (!message.empty()) {
            oss << " (" << message << ")";
        }
        throw AssertionError(oss.str());
    }
}

template<typename ExceptionType>
void assert_throws(std::function<void()> func, const std::string& message = "Expected exception") {
    try {
        func();
        throw AssertionError(message + " - No exception thrown");
    } catch (const ExceptionType&) {
        // Expected
    } catch (const std::exception& e) {
        throw AssertionError(message + " - Wrong exception type: " + e.what());
    }
}

inline void assert_not_null(const void* ptr, const std::string& message = "Expected non-null") {
    if (ptr == nullptr) {
        throw AssertionError(message);
    }
}

inline void assert_null(const void* ptr, const std::string& message = "Expected null") {
    if (ptr != nullptr) {
        throw AssertionError(message);
    }
}

} // namespace test
