load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

# Container types alias - no dependencies, can be used anywhere
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
        "staged_metric.hpp",
    ],
    deps = [
        ":container_types",
        ":order_stage",
    ],
)

cc_library(
    name = "multi_group",
    hdrs = [
        "key_extractors.hpp",
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
