#pragma once

#include <type_traits>

namespace engine {

// ============================================================================
// Type traits for metric detection
// ============================================================================

template<typename T, typename... Types>
struct contains_type : std::disjunction<std::is_same<T, Types>...> {};

template<typename T, typename... Types>
inline constexpr bool contains_type_v = contains_type<T, Types...>::value;

// Forward declaration of the generic engine
template<typename... Metrics>
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
