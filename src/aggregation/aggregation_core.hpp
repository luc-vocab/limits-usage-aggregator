#pragma once

#include "grouping.hpp"
#include "aggregation_traits.hpp"
#include <unordered_map>
#include <optional>
#include <tuple>
#include <functional>

namespace aggregation {

// ============================================================================
// AggregationBucket - A single aggregation at a specific grouping level
// ============================================================================

template<typename Key, typename Combiner>
class AggregationBucket {
public:
    using key_type = Key;
    using value_type = typename Combiner::value_type;
    using combiner_type = Combiner;

private:
    std::unordered_map<Key, value_type> values_;

public:
    // Get current value for a key (returns identity if not present)
    value_type get(const Key& key) const {
        auto it = values_.find(key);
        if (it == values_.end()) {
            return Combiner::identity();
        }
        return it->second;
    }

    // Check if key exists
    bool contains(const Key& key) const {
        return values_.find(key) != values_.end();
    }

    // Add/combine a value - O(1)
    void add(const Key& key, const value_type& delta) {
        auto it = values_.find(key);
        if (it == values_.end()) {
            values_[key] = Combiner::combine(Combiner::identity(), delta);
        } else {
            it->second = Combiner::combine(it->second, delta);
        }
    }

    // Remove/uncombine a value - O(1)
    // Only available for combiners that support uncombine
    template<typename C = Combiner>
    std::enable_if_t<has_uncombine_v<C>> remove(const Key& key, const value_type& delta) {
        auto it = values_.find(key);
        if (it != values_.end()) {
            it->second = Combiner::uncombine(it->second, delta);
            // Optionally clean up if back to identity
            if (it->second == Combiner::identity()) {
                values_.erase(it);
            }
        }
    }

    // Update: remove old value and add new value - O(1)
    template<typename C = Combiner>
    std::enable_if_t<has_uncombine_v<C>> update(const Key& key,
                                                  const value_type& old_delta,
                                                  const value_type& new_delta) {
        remove(key, old_delta);
        add(key, new_delta);
    }

    // Get all keys with non-identity values
    std::vector<Key> keys() const {
        std::vector<Key> result;
        result.reserve(values_.size());
        for (const auto& [key, _] : values_) {
            result.push_back(key);
        }
        return result;
    }

    // Get number of tracked keys
    size_t size() const {
        return values_.size();
    }

    // Clear all values
    void clear() {
        values_.clear();
    }

    // Iterate over all values
    template<typename Func>
    void for_each(Func&& func) const {
        for (const auto& [key, value] : values_) {
            func(key, value);
        }
    }
};

// ============================================================================
// AggregationEngine - Holds multiple aggregation buckets
// ============================================================================

template<typename... Aggregations>
class AggregationEngine {
private:
    std::tuple<Aggregations...> aggregations_;

public:
    // Get a specific aggregation by type
    template<typename Aggregation>
    Aggregation& get() {
        return std::get<Aggregation>(aggregations_);
    }

    template<typename Aggregation>
    const Aggregation& get() const {
        return std::get<Aggregation>(aggregations_);
    }

    // Get aggregation by index
    template<size_t I>
    auto& get_by_index() {
        return std::get<I>(aggregations_);
    }

    template<size_t I>
    const auto& get_by_index() const {
        return std::get<I>(aggregations_);
    }

    // Clear all aggregations
    void clear() {
        std::apply([](auto&... aggs) {
            (aggs.clear(), ...);
        }, aggregations_);
    }

    // Get number of aggregation types
    static constexpr size_t aggregation_count() {
        return sizeof...(Aggregations);
    }
};

// ============================================================================
// Type aliases for common aggregation patterns
// ============================================================================

// Global delta tracking (gross and net)
using GlobalDeltaBucket = AggregationBucket<GlobalKey, DeltaCombiner>;

// Per-underlyer delta tracking
using UnderlyerDeltaBucket = AggregationBucket<UnderlyerKey, DeltaCombiner>;

// Per-instrument order count
using InstrumentOrderCountBucket = AggregationBucket<InstrumentSideKey, CountCombiner>;

// Per-underlyer quoted instrument count
using UnderlyerInstrumentCountBucket = AggregationBucket<UnderlyerKey, CountCombiner>;

// Per-strategy notional
using StrategyNotionalBucket = AggregationBucket<StrategyKey, SumCombiner<double>>;

// Per-portfolio notional
using PortfolioNotionalBucket = AggregationBucket<PortfolioKey, SumCombiner<double>>;

} // namespace aggregation
