load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

# Container types - kept separate to avoid circular dependency with engine
cc_library(
    name = "container_types",
    hdrs = ["container_types.hpp"],
)

cc_library(
    name = "aggregation",
    hdrs = [
        "aggregation_core.hpp",
        "aggregation_traits.hpp",
        "grouping.hpp",
        "key_extractors.hpp",
        "order_stage.hpp",
        "staged_metric.hpp",
    ],
    deps = [
        ":container_types",
        "//src/engine:engine_core",
    ],
)
