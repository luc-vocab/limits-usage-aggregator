load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "metrics",
    hdrs = [
        "base_exposure_metric.hpp",
        "delta_metric.hpp",
        "metric_policies.hpp",
        "notional_metric.hpp",
        "order_count_metric.hpp",
        "vega_metric.hpp",
    ],
    deps = [
        "//src/aggregation",
        "//src/aggregation:multi_group",
        "//src/aggregation:order_stage",
        "//src/engine:order_state",
        "//src/instrument",
    ],
)
