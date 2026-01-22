#pragma once

#include <type_traits>

// Forward declarations for metric types
namespace metrics {
    template<typename... Stages> class OrderCountMetrics;
    template<typename Key, typename... Stages> class OrderCountMetric;
    template<typename... Stages> class QuotedInstrumentCountMetric;
    template<typename Context, typename Instrument, typename... Stages> class DeltaMetrics;
    template<typename Key, typename Context, typename Instrument, typename... Stages> class DeltaMetric;
    template<typename Key, typename Context, typename Instrument, typename... Stages> class GrossDeltaMetric;
    template<typename Key, typename Context, typename Instrument, typename... Stages> class NetDeltaMetric;
    template<typename Context, typename Instrument, typename... Stages> class NotionalMetrics;
    template<typename Key, typename Context, typename Instrument, typename... Stages> class NotionalMetric;
    template<typename Key, typename Context, typename Instrument, typename... Stages> class GrossNotionalMetric;
    template<typename Key, typename Context, typename Instrument, typename... Stages> class NetNotionalMetric;
    template<typename Key, typename Context, typename Instrument, typename... Stages> class GrossVegaMetric;
    template<typename Key, typename Context, typename Instrument, typename... Stages> class NetVegaMetric;
}

namespace engine {

// ============================================================================
// Type traits for metric detection
// ============================================================================

template<typename T, typename... Types>
struct contains_type : std::disjunction<std::is_same<T, Types>...> {};

template<typename T, typename... Types>
inline constexpr bool contains_type_v = contains_type<T, Types...>::value;

// ============================================================================
// Metric family detection traits
// ============================================================================
//
// These traits detect if a type belongs to a specific metric family,
// regardless of the stage template parameters used.
//

// OrderCountMetrics detection (both bundled and individual metric types)
template<typename T>
struct is_order_count_metric : std::false_type {};

// Detect bundled OrderCountMetrics<Stages...>
template<typename... Stages>
struct is_order_count_metric<metrics::OrderCountMetrics<Stages...>> : std::true_type {};

// Detect individual OrderCountMetric<Key, Stages...>
template<typename Key, typename... Stages>
struct is_order_count_metric<metrics::OrderCountMetric<Key, Stages...>> : std::true_type {};

// Detect QuotedInstrumentCountMetric<Stages...>
template<typename... Stages>
struct is_order_count_metric<metrics::QuotedInstrumentCountMetric<Stages...>> : std::true_type {};

template<typename T>
inline constexpr bool is_order_count_metric_v = is_order_count_metric<T>::value;

// Check if any type in a list is an OrderCountMetrics
template<typename... Types>
struct has_order_count_metric : std::disjunction<is_order_count_metric<Types>...> {};

template<typename... Types>
inline constexpr bool has_order_count_metric_v = has_order_count_metric<Types...>::value;

// DeltaMetrics detection (with Context and Instrument, supports both bundled and individual types)
template<typename T, typename Context = void, typename Instrument = void>
struct is_delta_metric : std::false_type {};

// Detect bundled DeltaMetrics<Context, Instrument, Stages...>
template<typename Context, typename Instrument, typename... Stages>
struct is_delta_metric<metrics::DeltaMetrics<Context, Instrument, Stages...>, Context, Instrument> : std::true_type {};

// Also allow matching any context/instrument for bundled DeltaMetrics
template<typename ContextT, typename InstrumentT, typename... Stages>
struct is_delta_metric<metrics::DeltaMetrics<ContextT, InstrumentT, Stages...>, void, void> : std::true_type {};

// Detect individual DeltaMetric<Key, Context, Instrument, Stages...>
template<typename Key, typename ContextT, typename InstrumentT, typename... Stages>
struct is_delta_metric<metrics::DeltaMetric<Key, ContextT, InstrumentT, Stages...>, void, void> : std::true_type {};

// Detect GrossDeltaMetric<Key, Context, Instrument, Stages...>
template<typename Key, typename ContextT, typename InstrumentT, typename... Stages>
struct is_delta_metric<metrics::GrossDeltaMetric<Key, ContextT, InstrumentT, Stages...>, void, void> : std::true_type {};

// Detect NetDeltaMetric<Key, Context, Instrument, Stages...>
template<typename Key, typename ContextT, typename InstrumentT, typename... Stages>
struct is_delta_metric<metrics::NetDeltaMetric<Key, ContextT, InstrumentT, Stages...>, void, void> : std::true_type {};

template<typename T, typename Context = void, typename Instrument = void>
inline constexpr bool is_delta_metric_v = is_delta_metric<T, Context, Instrument>::value;

// Check if any type in a list is a DeltaMetrics
template<typename... Types>
struct has_delta_metric : std::disjunction<is_delta_metric<Types, void, void>...> {};

template<typename... Types>
inline constexpr bool has_delta_metric_v = has_delta_metric<Types...>::value;

// NotionalMetrics detection (with Context and Instrument, supports both bundled and individual types)
template<typename T, typename Context = void, typename Instrument = void>
struct is_notional_metric : std::false_type {};

// Detect bundled NotionalMetrics<Context, Instrument, Stages...>
template<typename Context, typename Instrument, typename... Stages>
struct is_notional_metric<metrics::NotionalMetrics<Context, Instrument, Stages...>, Context, Instrument> : std::true_type {};

// Also allow matching any context/instrument for bundled NotionalMetrics
template<typename ContextT, typename InstrumentT, typename... Stages>
struct is_notional_metric<metrics::NotionalMetrics<ContextT, InstrumentT, Stages...>, void, void> : std::true_type {};

// Detect individual NotionalMetric<Key, Context, Instrument, Stages...>
template<typename Key, typename ContextT, typename InstrumentT, typename... Stages>
struct is_notional_metric<metrics::NotionalMetric<Key, ContextT, InstrumentT, Stages...>, void, void> : std::true_type {};

// Detect GrossNotionalMetric<Key, Context, Instrument, Stages...>
template<typename Key, typename ContextT, typename InstrumentT, typename... Stages>
struct is_notional_metric<metrics::GrossNotionalMetric<Key, ContextT, InstrumentT, Stages...>, void, void> : std::true_type {};

// Detect NetNotionalMetric<Key, Context, Instrument, Stages...>
template<typename Key, typename ContextT, typename InstrumentT, typename... Stages>
struct is_notional_metric<metrics::NetNotionalMetric<Key, ContextT, InstrumentT, Stages...>, void, void> : std::true_type {};

template<typename T, typename Context = void, typename Instrument = void>
inline constexpr bool is_notional_metric_v = is_notional_metric<T, Context, Instrument>::value;

// Check if any type in a list is a NotionalMetrics
template<typename... Types>
struct has_notional_metric : std::disjunction<is_notional_metric<Types, void, void>...> {};

template<typename... Types>
inline constexpr bool has_notional_metric_v = has_notional_metric<Types...>::value;

// VegaMetrics detection
template<typename T, typename Context = void, typename Instrument = void>
struct is_vega_metric : std::false_type {};

// Detect GrossVegaMetric<Key, Context, Instrument, Stages...>
template<typename Key, typename ContextT, typename InstrumentT, typename... Stages>
struct is_vega_metric<metrics::GrossVegaMetric<Key, ContextT, InstrumentT, Stages...>, void, void> : std::true_type {};

// Detect NetVegaMetric<Key, Context, Instrument, Stages...>
template<typename Key, typename ContextT, typename InstrumentT, typename... Stages>
struct is_vega_metric<metrics::NetVegaMetric<Key, ContextT, InstrumentT, Stages...>, void, void> : std::true_type {};

template<typename T, typename Context = void, typename Instrument = void>
inline constexpr bool is_vega_metric_v = is_vega_metric<T, Context, Instrument>::value;

// Check if any type in a list is a VegaMetric
template<typename... Types>
struct has_vega_metric : std::disjunction<is_vega_metric<Types, void, void>...> {};

template<typename... Types>
inline constexpr bool has_vega_metric_v = has_vega_metric<Types...>::value;

// ============================================================================
// Metric type finder
// ============================================================================
//
// Given a list of metrics, finds the actual type that matches a metric family.
// This allows us to get the concrete type with its stage parameters.
//

// Helper to find the first type matching a predicate
template<template<typename> class Pred, typename... Types>
struct find_matching_type;

template<template<typename> class Pred>
struct find_matching_type<Pred> {
    // No matching type found - use void
    using type = void;
};

template<template<typename> class Pred, typename T, typename... Rest>
struct find_matching_type<Pred, T, Rest...> {
    using type = std::conditional_t<
        Pred<T>::value,
        T,
        typename find_matching_type<Pred, Rest...>::type
    >;
};

// Find the OrderCountMetrics type in a parameter pack
template<typename... Types>
using order_count_metric_t = typename find_matching_type<is_order_count_metric, Types...>::type;

// Find the DeltaMetrics type in a parameter pack
template<typename... Types>
using delta_metric_t = typename find_matching_type<is_delta_metric, Types...>::type;

// Find the NotionalMetrics type in a parameter pack
template<typename... Types>
using notional_metric_t = typename find_matching_type<is_notional_metric, Types...>::type;

// Forward declaration of the generic engine
template<typename ContextType, typename Instrument, typename... Metrics>
class GenericRiskAggregationEngine;

// ============================================================================
// Accessor Mixin Templates - Provide accessor methods via CRTP
// ============================================================================
//
// Primary template: no accessors by default
// Specializations for each metric type provide the relevant accessor methods.
//
// Each metric type should define its own specialization in its header file
// to keep the metric-specific code together with the metric implementation.
//
// Example specialization pattern (in a metric header file):
//
//   namespace engine {
//   template<typename Derived>
//   class AccessorMixin<Derived, metrics::MyMetric> {
//   protected:
//       const metrics::MyMetric& my_metric_() const {
//           return static_cast<const Derived*>(this)->template get_metric<metrics::MyMetric>();
//       }
//   public:
//       double some_value() const { return my_metric_().some_value(); }
//   };
//   } // namespace engine
//

template<typename Derived, typename Metric>
class AccessorMixin {
    // Empty by default - no accessors for unknown metric types
};

} // namespace engine
