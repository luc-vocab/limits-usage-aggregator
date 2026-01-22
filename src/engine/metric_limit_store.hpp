#pragma once

#include "limits_config.hpp"
#include "pre_trade_check.hpp"
#include "../aggregation/grouping.hpp"
#include "../aggregation/aggregation_core.hpp"
#include "../fix/fix_messages.hpp"
#include <tuple>
#include <type_traits>
#include <string>

namespace engine {

// ============================================================================
// Type index finder - find index of type in parameter pack
// ============================================================================

namespace detail {

template<typename T, size_t I, typename... Types>
struct index_of_impl;

// Base case: type found at current position
template<typename T, size_t I, typename... Rest>
struct index_of_impl<T, I, T, Rest...> {
    static constexpr size_t value = I;
};

// Recursive case: keep searching
template<typename T, size_t I, typename First, typename... Rest>
struct index_of_impl<T, I, First, Rest...> {
    static constexpr size_t value = index_of_impl<T, I + 1, Rest...>::value;
};

} // namespace detail

template<typename T, typename... Types>
struct index_of {
    static constexpr size_t value = detail::index_of_impl<T, 0, Types...>::value;
};

template<typename T, typename... Types>
inline constexpr size_t index_of_v = index_of<T, Types...>::value;

// ============================================================================
// Key to string conversion - for LimitBreachInfo
// ============================================================================

namespace detail {

template<typename Key>
std::string key_to_string(const Key& key) {
    if constexpr (std::is_same_v<Key, std::string>) {
        return key;
    } else if constexpr (std::is_same_v<Key, aggregation::GlobalKey>) {
        return "global";
    } else if constexpr (std::is_same_v<Key, aggregation::UnderlyerKey>) {
        return key.underlyer;
    } else if constexpr (std::is_same_v<Key, aggregation::InstrumentKey>) {
        return key.symbol;
    } else if constexpr (std::is_same_v<Key, aggregation::StrategyKey>) {
        return key.strategy_id;
    } else if constexpr (std::is_same_v<Key, aggregation::PortfolioKey>) {
        return key.portfolio_id;
    } else if constexpr (std::is_same_v<Key, aggregation::InstrumentSideKey>) {
        return key.symbol + ":" + std::to_string(key.side);
    } else {
        return "unknown";
    }
}

// Convert metric value to double for breach checking
template<typename T>
double to_double(const T& value) {
    return static_cast<double>(value);
}

} // namespace detail

// ============================================================================
// MetricLimitStores - Container holding one LimitStore per Metric
// ============================================================================
//
// Each metric type must have a `key_type` typedef defining the key type
// for its limit store.
//
// Usage:
//   MetricLimitStores<DeltaMetric<UnderlyerKey>, OrderCountMetric<InstrumentSideKey>> stores;
//   stores.get<DeltaMetric<UnderlyerKey>>().set_limit(UnderlyerKey{"AAPL"}, 1000.0);
//

template<typename... Metrics>
class MetricLimitStores {
private:
    std::tuple<LimitStore<typename Metrics::key_type>...> stores_;

public:
    MetricLimitStores() = default;

    // Get the limit store for a specific metric type
    template<typename Metric>
    LimitStore<typename Metric::key_type>& get() {
        constexpr size_t idx = index_of_v<Metric, Metrics...>;
        return std::get<idx>(stores_);
    }

    template<typename Metric>
    const LimitStore<typename Metric::key_type>& get() const {
        constexpr size_t idx = index_of_v<Metric, Metrics...>;
        return std::get<idx>(stores_);
    }

    // Reset all limit stores
    void reset() {
        std::apply([](auto&... stores) { (stores.reset(), ...); }, stores_);
    }

    // Clear all limit stores (keeps defaults)
    void clear() {
        std::apply([](auto&... stores) { (stores.clear(), ...); }, stores_);
    }
};

// ============================================================================
// Metric traits for limit checking
// ============================================================================

// Trait to detect if a metric provides compute_order_contribution
template<typename Metric, typename = void>
struct has_compute_order_contribution : std::false_type {};

template<typename Metric>
struct has_compute_order_contribution<Metric, std::void_t<decltype(
    Metric::compute_order_contribution(
        std::declval<const fix::NewOrderSingle&>(),
        std::declval<const typename Metric::provider_type*>()
    )
)>> : std::true_type {};

template<typename Metric>
inline constexpr bool has_compute_order_contribution_v = has_compute_order_contribution<Metric>::value;

// Trait to detect if a metric provides extract_key
template<typename Metric, typename = void>
struct has_extract_key : std::false_type {};

template<typename Metric>
struct has_extract_key<Metric, std::void_t<decltype(
    Metric::extract_key(std::declval<const fix::NewOrderSingle&>())
)>> : std::true_type {};

template<typename Metric>
inline constexpr bool has_extract_key_v = has_extract_key<Metric>::value;

// Trait to get the limit type enum for a metric
template<typename Metric>
struct metric_limit_type {
    static constexpr LimitType value = LimitType::ORDER_COUNT; // Default
};

} // namespace engine
