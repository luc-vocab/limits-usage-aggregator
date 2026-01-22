#pragma once

#include "../aggregation/multi_group_aggregator.hpp"
#include "../aggregation/order_stage.hpp"
#include "../fix/fix_types.hpp"
#include "../instrument/instrument.hpp"
#include <unordered_set>

// Forward declarations
namespace engine {
    struct TrackedOrder;
    class OrderBook;
}

namespace metrics {

// ============================================================================
// OrderCountData - Internal storage for order count tracking at a single stage
// ============================================================================

struct OrderCountData {
    // Order counts per instrument-side combination
    aggregation::MultiGroupAggregator<
        aggregation::CountCombiner,
        aggregation::InstrumentSideKey
    > order_counts;

    // Track which instruments have orders per underlyer (for quoted count)
    std::unordered_map<std::string, std::unordered_set<std::string>> instruments_per_underlyer;
    aggregation::AggregationBucket<aggregation::UnderlyerKey, aggregation::CountCombiner> quoted_instruments;

    bool instrument_has_orders(const std::string& symbol) const {
        aggregation::InstrumentSideKey bid_key{symbol, static_cast<int>(fix::Side::BID)};
        aggregation::InstrumentSideKey ask_key{symbol, static_cast<int>(fix::Side::ASK)};
        return order_counts.get(bid_key) > 0 || order_counts.get(ask_key) > 0;
    }

    void add(const std::string& symbol, const std::string& underlyer, fix::Side side) {
        aggregation::InstrumentSideKey key{symbol, static_cast<int>(side)};
        order_counts.bucket<aggregation::InstrumentSideKey>().add(key, 1);

        auto& instruments = instruments_per_underlyer[underlyer];
        if (instruments.find(symbol) == instruments.end()) {
            instruments.insert(symbol);
            quoted_instruments.add(aggregation::UnderlyerKey{underlyer}, 1);
        }
    }

    void remove(const std::string& symbol, const std::string& underlyer, fix::Side side) {
        aggregation::InstrumentSideKey key{symbol, static_cast<int>(side)};
        order_counts.bucket<aggregation::InstrumentSideKey>().remove(key, 1);

        if (!instrument_has_orders(symbol)) {
            auto it = instruments_per_underlyer.find(underlyer);
            if (it != instruments_per_underlyer.end()) {
                it->second.erase(symbol);
                quoted_instruments.remove(aggregation::UnderlyerKey{underlyer}, 1);
                if (it->second.empty()) {
                    instruments_per_underlyer.erase(it);
                }
            }
        }
    }

    int64_t bid_order_count(const std::string& symbol) const {
        return order_counts.get(aggregation::InstrumentSideKey{symbol, static_cast<int>(fix::Side::BID)});
    }

    int64_t ask_order_count(const std::string& symbol) const {
        return order_counts.get(aggregation::InstrumentSideKey{symbol, static_cast<int>(fix::Side::ASK)});
    }

    int64_t total_order_count(const std::string& symbol) const {
        return bid_order_count(symbol) + ask_order_count(symbol);
    }

    int64_t quoted_instruments_count(const std::string& underlyer) const {
        return quoted_instruments.get(aggregation::UnderlyerKey{underlyer});
    }

    void clear() {
        order_counts.clear();
        instruments_per_underlyer.clear();
        quoted_instruments.clear();
    }
};

// ============================================================================
// Order Count Metrics - Tracks order counts per instrument/side
// ============================================================================
//
// Uses MultiGroupAggregator for order counts per instrument-side.
// Quoted instruments tracking is handled separately due to its special
// first-add/last-remove logic.
//
// Template parameters:
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//              Default is AllStages which tracks all three stages.
//

template<typename... Stages>
class OrderCountMetrics {
    using Config = aggregation::StageConfig<Stages...>;

private:
    // Per-stage data storage
    OrderCountData position_data_;
    OrderCountData open_data_;
    OrderCountData in_flight_data_;

    // Get data reference for a given stage
    OrderCountData& get_stage_data(aggregation::OrderStage stage) {
        switch (stage) {
            case aggregation::OrderStage::POSITION: return position_data_;
            case aggregation::OrderStage::OPEN: return open_data_;
            case aggregation::OrderStage::IN_FLIGHT: return in_flight_data_;
            default: return position_data_;
        }
    }

    const OrderCountData& get_stage_data(aggregation::OrderStage stage) const {
        switch (stage) {
            case aggregation::OrderStage::POSITION: return position_data_;
            case aggregation::OrderStage::OPEN: return open_data_;
            case aggregation::OrderStage::IN_FLIGHT: return in_flight_data_;
            default: return position_data_;
        }
    }

public:
    // ========================================================================
    // InstrumentProvider interface (for consistency, not used by OrderCount)
    // ========================================================================

    template<typename Provider>
    void set_instrument_provider(const Provider* /*provider*/) {
        // OrderCountMetrics doesn't need InstrumentProvider
    }

    // ========================================================================
    // Stage configuration info
    // ========================================================================

    static constexpr bool tracks_position() { return Config::track_position; }
    static constexpr bool tracks_open() { return Config::track_open; }
    static constexpr bool tracks_in_flight() { return Config::track_in_flight; }

    // ========================================================================
    // Per-stage accessors
    // ========================================================================

    // Position stage accessors
    const OrderCountData& position() const { return position_data_; }
    int64_t position_bid_order_count(const std::string& symbol) const {
        return position_data_.bid_order_count(symbol);
    }
    int64_t position_ask_order_count(const std::string& symbol) const {
        return position_data_.ask_order_count(symbol);
    }
    int64_t position_total_order_count(const std::string& symbol) const {
        return position_data_.total_order_count(symbol);
    }
    int64_t position_quoted_instruments_count(const std::string& underlyer) const {
        return position_data_.quoted_instruments_count(underlyer);
    }

    // Open stage accessors
    const OrderCountData& open_orders() const { return open_data_; }
    int64_t open_bid_order_count(const std::string& symbol) const {
        return open_data_.bid_order_count(symbol);
    }
    int64_t open_ask_order_count(const std::string& symbol) const {
        return open_data_.ask_order_count(symbol);
    }
    int64_t open_total_order_count(const std::string& symbol) const {
        return open_data_.total_order_count(symbol);
    }
    int64_t open_quoted_instruments_count(const std::string& underlyer) const {
        return open_data_.quoted_instruments_count(underlyer);
    }

    // In-flight stage accessors
    const OrderCountData& in_flight() const { return in_flight_data_; }
    int64_t in_flight_bid_order_count(const std::string& symbol) const {
        return in_flight_data_.bid_order_count(symbol);
    }
    int64_t in_flight_ask_order_count(const std::string& symbol) const {
        return in_flight_data_.ask_order_count(symbol);
    }
    int64_t in_flight_total_order_count(const std::string& symbol) const {
        return in_flight_data_.total_order_count(symbol);
    }
    int64_t in_flight_quoted_instruments_count(const std::string& underlyer) const {
        return in_flight_data_.quoted_instruments_count(underlyer);
    }

    // ========================================================================
    // Combined/total accessors (order exposure - open + in-flight only)
    // ========================================================================
    //
    // For pre-trade risk checking, order counts exclude position stage since
    // position represents realized fills, not active orders.
    //

    int64_t bid_order_count(const std::string& symbol) const {
        int64_t total = 0;
        if constexpr (Config::track_open) total += open_bid_order_count(symbol);
        if constexpr (Config::track_in_flight) total += in_flight_bid_order_count(symbol);
        return total;
    }

    int64_t ask_order_count(const std::string& symbol) const {
        int64_t total = 0;
        if constexpr (Config::track_open) total += open_ask_order_count(symbol);
        if constexpr (Config::track_in_flight) total += in_flight_ask_order_count(symbol);
        return total;
    }

    int64_t total_order_count(const std::string& symbol) const {
        return bid_order_count(symbol) + ask_order_count(symbol);
    }

    int64_t quoted_instruments_count(const std::string& underlyer) const {
        int64_t total = 0;
        if constexpr (Config::track_open) total += open_quoted_instruments_count(underlyer);
        if constexpr (Config::track_in_flight) total += in_flight_quoted_instruments_count(underlyer);
        return total;
    }

    // ========================================================================
    // Total accessors (including position stage)
    // ========================================================================

    int64_t total_bid_order_count_all_stages(const std::string& symbol) const {
        int64_t total = 0;
        if constexpr (Config::track_position) total += position_bid_order_count(symbol);
        if constexpr (Config::track_open) total += open_bid_order_count(symbol);
        if constexpr (Config::track_in_flight) total += in_flight_bid_order_count(symbol);
        return total;
    }

    int64_t total_ask_order_count_all_stages(const std::string& symbol) const {
        int64_t total = 0;
        if constexpr (Config::track_position) total += position_ask_order_count(symbol);
        if constexpr (Config::track_open) total += open_ask_order_count(symbol);
        if constexpr (Config::track_in_flight) total += in_flight_ask_order_count(symbol);
        return total;
    }

    int64_t total_quoted_instruments_count_all_stages(const std::string& underlyer) const {
        int64_t total = 0;
        if constexpr (Config::track_position) total += position_quoted_instruments_count(underlyer);
        if constexpr (Config::track_open) total += open_quoted_instruments_count(underlyer);
        if constexpr (Config::track_in_flight) total += in_flight_quoted_instruments_count(underlyer);
        return total;
    }

    std::vector<aggregation::UnderlyerKey> underlyers() const {
        std::set<std::string> all_underlyers;
        auto collect = [&all_underlyers](const OrderCountData& data) {
            for (const auto& key : data.quoted_instruments.keys()) {
                all_underlyers.insert(key.underlyer);
            }
        };
        if constexpr (Config::track_position) collect(position_data_);
        if constexpr (Config::track_open) collect(open_data_);
        if constexpr (Config::track_in_flight) collect(in_flight_data_);

        std::vector<aggregation::UnderlyerKey> result;
        for (const auto& u : all_underlyers) {
            result.push_back(aggregation::UnderlyerKey{u});
        }
        return result;
    }

    // ========================================================================
    // Generic metric interface (used by template RiskAggregationEngine)
    // ========================================================================

    // Called when order is sent (PENDING_NEW state -> IN_FLIGHT stage)
    void on_order_added(const engine::TrackedOrder& order);

    // Called when order is fully removed (nack, cancel, full fill)
    void on_order_removed(const engine::TrackedOrder& order);

    // Called when order is modified (update ack) - no-op for order count
    void on_order_updated(const engine::TrackedOrder& /*order*/, int64_t /*old_qty*/) {
        // Order count doesn't change on update
    }

    // Called on partial fill - no-op for order count
    void on_partial_fill(const engine::TrackedOrder& /*order*/, int64_t /*filled_qty*/) {
        // Order count doesn't change on partial fill
    }

    // Called on full fill - no-op for order count (handled by on_order_removed)
    void on_full_fill(const engine::TrackedOrder& /*order*/, int64_t /*filled_qty*/) {
        // Order count change handled by on_order_removed
    }

    // Called when order state changes (for stage transitions)
    void on_state_change(const engine::TrackedOrder& order,
                         engine::OrderState old_state,
                         engine::OrderState new_state);

    // Called when order is modified AND changes state (replace ack)
    // For order count, this is the same as a simple state change
    void on_order_updated_with_state_change(const engine::TrackedOrder& order,
                                             int64_t /*old_qty*/,
                                             engine::OrderState old_state,
                                             engine::OrderState new_state) {
        on_state_change(order, old_state, new_state);
    }

    // ========================================================================
    // Direct interface (for backward compatibility and direct usage)
    // ========================================================================

    void add_order(const std::string& symbol, const std::string& underlyer, fix::Side side) {
        // Default: add to in_flight
        in_flight_data_.add(symbol, underlyer, side);
    }

    void remove_order(const std::string& symbol, const std::string& underlyer, fix::Side side) {
        // Default: remove from in_flight
        in_flight_data_.remove(symbol, underlyer, side);
    }

    void clear() {
        position_data_.clear();
        open_data_.clear();
        in_flight_data_.clear();
    }
};

} // namespace metrics

// Include TrackedOrder definition and implement generic interface
#include "../engine/order_state.hpp"

namespace metrics {

template<typename... Stages>
void OrderCountMetrics<Stages...>::on_order_added(const engine::TrackedOrder& order) {
    // New orders always start in IN_FLIGHT stage
    in_flight_data_.add(order.symbol, order.underlyer, order.side);
}

template<typename... Stages>
void OrderCountMetrics<Stages...>::on_order_removed(const engine::TrackedOrder& order) {
    // Remove from appropriate stage based on current state
    auto stage = aggregation::stage_from_order_state(order.state);
    get_stage_data(stage).remove(order.symbol, order.underlyer, order.side);
}

template<typename... Stages>
void OrderCountMetrics<Stages...>::on_state_change(const engine::TrackedOrder& order,
                                                    engine::OrderState old_state,
                                                    engine::OrderState new_state) {
    auto old_stage = aggregation::stage_from_order_state(old_state);
    auto new_stage = aggregation::stage_from_order_state(new_state);

    if (old_stage != new_stage && aggregation::is_active_order_state(new_state)) {
        // Move from old stage to new stage
        get_stage_data(old_stage).remove(order.symbol, order.underlyer, order.side);
        get_stage_data(new_stage).add(order.symbol, order.underlyer, order.side);
    }
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

template<typename Derived, typename... Stages>
class AccessorMixin<Derived, metrics::OrderCountMetrics<Stages...>> {
protected:
    const metrics::OrderCountMetrics<Stages...>& order_count_metrics_() const {
        return static_cast<const Derived*>(this)->template get_metric<metrics::OrderCountMetrics<Stages...>>();
    }

public:
    // Combined accessors
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

    // Per-stage accessors
    int64_t position_bid_order_count(const std::string& symbol) const {
        return order_count_metrics_().position_bid_order_count(symbol);
    }

    int64_t open_bid_order_count(const std::string& symbol) const {
        return order_count_metrics_().open_bid_order_count(symbol);
    }

    int64_t in_flight_bid_order_count(const std::string& symbol) const {
        return order_count_metrics_().in_flight_bid_order_count(symbol);
    }
};

} // namespace engine
