load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

# Core engine components - no dependency on aggregation or metrics
cc_library(
    name = "engine_core",
    hdrs = [
        "accessor_mixin.hpp",
        "generic_aggregation_engine.hpp",
        "limits_config.hpp",
        "order_state.hpp",
        "pre_trade_check.hpp",
    ],
    deps = [
        "//src/aggregation:container_types",
        "//src/fix",
        "//src/instrument",
    ],
)

# Full engine with limits and metrics support
cc_library(
    name = "engine",
    hdrs = [
        "metric_limit_store.hpp",
        "risk_engine_with_limits.hpp",
    ],
    deps = [
        ":engine_core",
        "//src/aggregation",
        "//src/fix",
        "//src/metrics",
    ],
)
