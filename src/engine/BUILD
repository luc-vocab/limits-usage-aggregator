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

# Limits configuration - types for runtime limit checking
cc_library(
    name = "limits_config",
    hdrs = ["limits_config.hpp"],
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

# Engine with limits support
cc_library(
    name = "risk_engine_with_limits",
    hdrs = ["risk_engine_with_limits.hpp"],
    deps = [
        ":generic_aggregation_engine",
        ":limits_config",
        "//src/metrics",
    ],
)

# Full engine with standard metric type aliases
cc_library(
    name = "engine",
    hdrs = [
        "risk_engine.hpp",
        "risk_engine_with_limits.hpp",
        "limits_config.hpp",
    ],
    deps = [
        ":generic_aggregation_engine",
        ":limits_config",
        ":risk_engine_with_limits",
        "//src/metrics",
    ],
)
