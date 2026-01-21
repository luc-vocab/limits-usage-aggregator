load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

# Base accessor mixin template - no dependencies
# This is the foundation for CRTP-based metric accessors
cc_library(
    name = "accessor_mixin",
    hdrs = ["accessor_mixin.hpp"],
)

# Order state tracking - tracks order lifecycle and state
cc_library(
    name = "order_state",
    hdrs = ["order_state.hpp"],
    deps = [
        "//src/aggregation",
        "//src/fix",
    ],
)

# Generic aggregation engine - template-based, metric-agnostic
cc_library(
    name = "generic_aggregation_engine",
    hdrs = ["generic_aggregation_engine.hpp"],
    deps = [
        ":accessor_mixin",
        ":order_state",
        "//src/fix",
    ],
)

# Full engine with standard metric type aliases
cc_library(
    name = "engine",
    hdrs = ["risk_engine.hpp"],
    deps = [
        ":generic_aggregation_engine",
        "//src/metrics",
    ],
)
