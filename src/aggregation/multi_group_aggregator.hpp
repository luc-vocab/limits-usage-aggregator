#pragma once

#include "aggregation_core.hpp"
#include "key_extractors.hpp"
#include <tuple>
#include <type_traits>

namespace aggregation {

// ============================================================================
// Type traits for checking if a type is in a parameter pack
// ============================================================================

template<typename T, typename... Ts>
struct is_one_of : std::disjunction<std::is_same<T, Ts>...> {};

template<typename T, typename... Ts>
inline constexpr bool is_one_of_v = is_one_of<T, Ts...>::value;

// ============================================================================
// MultiGroupAggregator - Aggregates values across multiple grouping levels
// ============================================================================
//
// This template class holds multiple AggregationBucket instances, one for each
// grouping key type. It provides unified add/remove/update operations that
// automatically distribute values to all applicable buckets based on the
// TrackedOrder and KeyExtractor configuration.
//
// Template Parameters:
// - Combiner: The combiner type (e.g., SumCombiner<double>, DeltaCombiner)
// - Keys: The grouping key types (e.g., GlobalKey, UnderlyerKey, StrategyKey)
//
// Example Usage:
//   MultiGroupAggregator<DeltaCombiner, GlobalKey, UnderlyerKey> delta_agg;
//   delta_agg.add(order, DeltaValue{100.0, 50.0});
//   auto global_delta = delta_agg.get(GlobalKey::instance());
//   auto underlyer_delta = delta_agg.get(UnderlyerKey{"AAPL"});
//

template<typename Combiner, typename... Keys>
class MultiGroupAggregator {
public:
    using value_type = typename Combiner::value_type;
    using combiner_type = Combiner;

private:
    std::tuple<AggregationBucket<Keys, Combiner>...> buckets_;

    // Helper to add to a single bucket if applicable
    template<typename Key>
    void add_to_bucket(const engine::TrackedOrder& order, const value_type& value) {
        if (KeyExtractor<Key>::is_applicable(order)) {
            auto key = KeyExtractor<Key>::extract(order);
            std::get<AggregationBucket<Key, Combiner>>(buckets_).add(key, value);
        }
    }

    // Helper to remove from a single bucket if applicable
    template<typename Key>
    void remove_from_bucket(const engine::TrackedOrder& order, const value_type& value) {
        if (KeyExtractor<Key>::is_applicable(order)) {
            auto key = KeyExtractor<Key>::extract(order);
            std::get<AggregationBucket<Key, Combiner>>(buckets_).remove(key, value);
        }
    }

public:
    // ========================================================================
    // Modifiers - O(1) operations on all applicable buckets
    // ========================================================================

    // Add value to all applicable buckets based on TrackedOrder
    void add(const engine::TrackedOrder& order, const value_type& value) {
        (add_to_bucket<Keys>(order, value), ...);
    }

    // Remove value from all applicable buckets
    // Only available for combiners that support uncombine
    template<typename C = Combiner>
    std::enable_if_t<has_uncombine_v<C>>
    remove(const engine::TrackedOrder& order, const value_type& value) {
        (remove_from_bucket<Keys>(order, value), ...);
    }

    // Update: remove old value and add new value in all applicable buckets
    template<typename C = Combiner>
    std::enable_if_t<has_uncombine_v<C>>
    update(const engine::TrackedOrder& order,
           const value_type& old_value,
           const value_type& new_value) {
        remove(order, old_value);
        add(order, new_value);
    }

    // ========================================================================
    // Accessors - O(1) lookup for any grouping level
    // ========================================================================

    // Get value for a specific key
    template<typename Key>
    value_type get(const Key& key) const {
        static_assert(is_one_of_v<Key, Keys...>,
                      "Key type not tracked by this MultiGroupAggregator");
        return std::get<AggregationBucket<Key, Combiner>>(buckets_).get(key);
    }

    // Get the bucket for a specific key type (const)
    template<typename Key>
    const AggregationBucket<Key, Combiner>& bucket() const {
        static_assert(is_one_of_v<Key, Keys...>,
                      "Key type not tracked by this MultiGroupAggregator");
        return std::get<AggregationBucket<Key, Combiner>>(buckets_);
    }

    // Get the bucket for a specific key type (non-const)
    template<typename Key>
    AggregationBucket<Key, Combiner>& bucket() {
        static_assert(is_one_of_v<Key, Keys...>,
                      "Key type not tracked by this MultiGroupAggregator");
        return std::get<AggregationBucket<Key, Combiner>>(buckets_);
    }

    // ========================================================================
    // Type introspection
    // ========================================================================

    // Check if this aggregator tracks a specific key type (compile-time)
    template<typename Key>
    static constexpr bool has_key() {
        return is_one_of_v<Key, Keys...>;
    }

    // Get number of grouping levels
    static constexpr size_t key_count() {
        return sizeof...(Keys);
    }

    // ========================================================================
    // Utility
    // ========================================================================

    // Clear all buckets
    void clear() {
        std::apply([](auto&... buckets) {
            (buckets.clear(), ...);
        }, buckets_);
    }

    // Get all keys for a specific bucket type
    template<typename Key>
    std::vector<Key> keys() const {
        static_assert(is_one_of_v<Key, Keys...>,
                      "Key type not tracked by this MultiGroupAggregator");
        return std::get<AggregationBucket<Key, Combiner>>(buckets_).keys();
    }
};

} // namespace aggregation
