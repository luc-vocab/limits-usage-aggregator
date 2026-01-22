load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "test_runner",
    srcs = [
        "fix_message_tests.cpp",
        "integration_test_order_count_by_instrument_side.cpp",
        "integration_test_gross_notional.cpp",
        "integration_test_option_underlyer_refactored.cpp",
        "integration_test_options_gross_net_check.cpp",
        "integration_test_pre_trade_check_updates.cpp",
        "integration_test_vega_delta_combined.cpp",
    ],
    deps = [
        "//src/aggregation",
        "//src/engine",
        "//src/fix",
        "//src/instrument",
        "//src/metrics",
        "@googletest//:gtest_main",
    ],
)
