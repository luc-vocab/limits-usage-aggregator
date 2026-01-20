#include <iostream>
#include <vector>
#include <string>
#include <cstring>

#include "test_framework.hpp"

// Forward declarations for test suites
test::TestSuite run_fix_message_tests();
test::TestSuite run_aggregation_tests();
test::TestSuite run_integration_tests();

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS]\n"
              << "\nOptions:\n"
              << "  --help           Show this help message\n"
              << "  --filter=NAME    Run only test suites containing NAME\n"
              << "  --list           List available test suites\n"
              << "\nExamples:\n"
              << "  " << program_name << "                     # Run all tests\n"
              << "  " << program_name << " --filter=fix        # Run FIX message tests\n"
              << "  " << program_name << " --filter=aggregation # Run aggregation tests\n"
              << "  " << program_name << " --filter=integration # Run integration tests\n";
}

int main(int argc, char* argv[]) {
    std::string filter;
    bool list_only = false;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (std::strcmp(argv[i], "--list") == 0) {
            list_only = true;
        } else if (std::strncmp(argv[i], "--filter=", 9) == 0) {
            filter = argv[i] + 9;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    // Available test suites
    struct TestSuiteEntry {
        std::string name;
        std::function<test::TestSuite()> runner;
    };

    std::vector<TestSuiteEntry> suites = {
        {"fix", run_fix_message_tests},
        {"aggregation", run_aggregation_tests},
        {"integration", run_integration_tests}
    };

    if (list_only) {
        std::cout << "Available test suites:\n";
        for (const auto& suite : suites) {
            std::cout << "  " << suite.name << "\n";
        }
        return 0;
    }

    std::cout << "========================================\n";
    std::cout << "Pre-Trade Risk Aggregation Engine Tests\n";
    std::cout << "========================================\n";

    int total_passed = 0;
    int total_failed = 0;
    std::vector<test::TestSuite> results;

    for (const auto& entry : suites) {
        // Apply filter if specified
        if (!filter.empty() && entry.name.find(filter) == std::string::npos) {
            continue;
        }

        auto suite = entry.runner();
        suite.print_results();
        total_passed += suite.passed();
        total_failed += suite.failed();
        results.push_back(std::move(suite));
    }

    std::cout << "\n========================================\n";
    std::cout << "Summary\n";
    std::cout << "========================================\n";
    std::cout << "Total Passed: " << total_passed << "\n";
    std::cout << "Total Failed: " << total_failed << "\n";
    std::cout << "Total Tests:  " << (total_passed + total_failed) << "\n";

    if (total_failed == 0) {
        std::cout << "\nAll tests PASSED!\n";
        return 0;
    } else {
        std::cout << "\nSome tests FAILED!\n";
        return 1;
    }
}

// Include test source files
#include "fix_message_tests.cpp"
#include "aggregation_tests.cpp"
#include "integration_tests.cpp"
