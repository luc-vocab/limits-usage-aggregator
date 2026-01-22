#pragma once

#include "../aggregation/multi_group_aggregator.hpp"
#include "../fix/fix_types.hpp"
#include <unordered_set>

// Forward declarations
namespace engine {
    struct TrackedOrder;
    class OrderBook;
}

namespace metrics {

// ============================================================================
// Order Count Metrics - Tracks order counts per instrument/side
// ============================================================================
//
// Uses MultiGroupAggregator for order counts per instrument-side.
// Quoted instruments tracking is handled separately due to its special
// first-add/last-remove logic.
//

class OrderCountMetrics {
private:
    aggregation::MultiGroupAggregator<
        aggregation::CountCombiner,
        aggregation::InstrumentSideKey
    > order_counts_;

    // Track which instruments have orders per underlyer (for quoted count)
    // underlyer -> set of instruments
    std::unordered_map<std::string, std::unordered_set<std::string>> instruments_per_underlyer_;
    aggregation::AggregationBucket<aggregation::UnderlyerKey, aggregation::CountCombiner> quoted_instruments_;

    // Helper to check if an instrument still has orders
    bool instrument_has_orders(const std::string& symbol) const {
        aggregation::InstrumentSideKey bid_key{symbol, static_cast<int>(fix::Side::BID)};
        aggregation::InstrumentSideKey ask_key{symbol, static_cast<int>(fix::Side::ASK)};
        return order_counts_.get(bid_key) > 0 || order_counts_.get(ask_key) > 0;
    }

public:
    // ========================================================================
    // Generic metric interface (used by template RiskAggregationEngine)
    // ========================================================================

    // Called when order is sent (PENDING_NEW state)
    void on_order_added(const engine::TrackedOrder& order);

    // Called when order is fully removed (nack, cancel, full fill)
    void on_order_removed(const engine::TrackedOrder& order);

    // Called when order is modified (update ack) - no-op for order count
    void on_order_updated(const engine::TrackedOrder& /*order*/,
                          double /*old_delta_exposure*/, double /*old_notional*/) {
        // Order count doesn't change on update
    }

    // Called on partial fill - no-op for order count
    void on_partial_fill(const engine::TrackedOrder& /*order*/,
                         double /*filled_delta_exposure*/, double /*filled_notional*/) {
        // Order count doesn't change on partial fill
    }

    // ========================================================================
    // Legacy interface (for backward compatibility and direct usage)
    // ========================================================================

    void add_order(const std::string& symbol, const std::string& underlyer, fix::Side side) {
        aggregation::InstrumentSideKey key{symbol, static_cast<int>(side)};
        order_counts_.bucket<aggregation::InstrumentSideKey>().add(key, 1);

        // Track quoted instruments per underlyer
        auto& instruments = instruments_per_underlyer_[underlyer];
        if (instruments.find(symbol) == instruments.end()) {
            instruments.insert(symbol);
            // First order for this instrument under this underlyer
            quoted_instruments_.add(aggregation::UnderlyerKey{underlyer}, 1);
        }
    }

    void remove_order(const std::string& symbol, const std::string& underlyer, fix::Side side) {
        aggregation::InstrumentSideKey key{symbol, static_cast<int>(side)};
        order_counts_.bucket<aggregation::InstrumentSideKey>().remove(key, 1);

        // Check if we still have any orders for this instrument
        if (!instrument_has_orders(symbol)) {
            // No more orders for this instrument
            auto it = instruments_per_underlyer_.find(underlyer);
            if (it != instruments_per_underlyer_.end()) {
                it->second.erase(symbol);
                quoted_instruments_.remove(aggregation::UnderlyerKey{underlyer}, 1);
                if (it->second.empty()) {
                    instruments_per_underlyer_.erase(it);
                }
            }
        }
    }

    // ========================================================================
    // Accessors
    // ========================================================================

    int64_t bid_order_count(const std::string& symbol) const {
        return order_counts_.get(aggregation::InstrumentSideKey{symbol, static_cast<int>(fix::Side::BID)});
    }

    int64_t ask_order_count(const std::string& symbol) const {
        return order_counts_.get(aggregation::InstrumentSideKey{symbol, static_cast<int>(fix::Side::ASK)});
    }

    int64_t total_order_count(const std::string& symbol) const {
        return bid_order_count(symbol) + ask_order_count(symbol);
    }

    int64_t quoted_instruments_count(const std::string& underlyer) const {
        return quoted_instruments_.get(aggregation::UnderlyerKey{underlyer});
    }

    std::vector<aggregation::UnderlyerKey> underlyers() const {
        return quoted_instruments_.keys();
    }

    void clear() {
        order_counts_.clear();
        instruments_per_underlyer_.clear();
        quoted_instruments_.clear();
    }
};

} // namespace metrics

// Include TrackedOrder definition and implement generic interface
#include "../engine/order_state.hpp"

namespace metrics {

inline void OrderCountMetrics::on_order_added(const engine::TrackedOrder& order) {
    add_order(order.symbol, order.underlyer, order.side);
}

inline void OrderCountMetrics::on_order_removed(const engine::TrackedOrder& order) {
    remove_order(order.symbol, order.underlyer, order.side);
}

} // namespace metrics

// ============================================================================
// AccessorMixin specialization for OrderCountMetrics
// ============================================================================
//
// This specialization provides CRTP-based accessor methods for engines
// that include OrderCountMetrics.
//

#include "../engine/accessor_mixin.hpp"

namespace engine {

template<typename Derived>
class AccessorMixin<Derived, metrics::OrderCountMetrics> {
protected:
    const metrics::OrderCountMetrics& order_count_metrics_() const {
        return static_cast<const Derived*>(this)->template get_metric<metrics::OrderCountMetrics>();
    }

public:
    int64_t bid_order_count(const std::string& symbol) const {
        return order_count_metrics_().bid_order_count(symbol);
    }

    int64_t ask_order_count(const std::string& symbol) const {
        return order_count_metrics_().ask_order_count(symbol);
    }

    int64_t total_order_count(const std::string& symbol) const {
        return order_count_metrics_().total_order_count(symbol);
    }

    int64_t quoted_instruments_count(const std::string& underlyer) const {
        return order_count_metrics_().quoted_instruments_count(underlyer);
    }
};

} // namespace engine
