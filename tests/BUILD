load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "test_runner",
    srcs = [
        "fix_message_tests.cpp",
        "aggregation_tests.cpp",
        "integration_tests.cpp",
        "integration_test_option_underlyer_count.cpp",
        "integration_test_order_count_by_instrument_side.cpp",
        "integration_test_gross_notional.cpp",
        "integration_test_option_underlyer_refactored.cpp",
    ],
    deps = [
        "//src/aggregation",
        "//src/aggregation:multi_group",
        "//src/engine",
        "//src/fix",
        "//src/instrument",
        "//src/metrics",
        "@googletest//:gtest_main",
    ],
)
