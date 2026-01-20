load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "test_runner",
    srcs = [
        "fix_message_tests.cpp",
        "aggregation_tests.cpp",
        "integration_tests.cpp",
    ],
    deps = [
        "//src/aggregation",
        "//src/engine",
        "//src/fix",
        "//src/metrics",
        "@googletest//:gtest_main",
    ],
)
