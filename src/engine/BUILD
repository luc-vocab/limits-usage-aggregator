load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "engine",
    hdrs = [
        "order_state.hpp",
        "risk_engine.hpp",
    ],
    deps = [
        "//src/aggregation",
        "//src/fix",
        "//src/metrics",
    ],
)
