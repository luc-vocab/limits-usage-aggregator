load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "metrics",
    hdrs = [
        "delta_metrics.hpp",
        "notional_metrics.hpp",
        "order_count_metrics.hpp",
    ],
    deps = [
        "//src/aggregation",
    ],
)
