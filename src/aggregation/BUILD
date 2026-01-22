load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "aggregation",
    hdrs = [
        "aggregation_core.hpp",
        "aggregation_traits.hpp",
        "grouping.hpp",
    ],
)

cc_library(
    name = "multi_group",
    hdrs = [
        "key_extractors.hpp",
        "multi_group_aggregator.hpp",
    ],
    deps = [
        ":aggregation",
        "//src/engine:order_state",
    ],
)

cc_library(
    name = "order_stage",
    hdrs = ["order_stage.hpp"],
    deps = [
        "//src/engine:order_state",
    ],
)
