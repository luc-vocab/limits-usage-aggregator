#pragma once

#include "../aggregation/order_stage.hpp"
#include "../instrument/instrument.hpp"
#include "../fix/fix_types.hpp"
#include <string>

// Forward declarations
namespace engine {
    struct TrackedOrder;
    enum class OrderState;
}

namespace metrics {

// ============================================================================
// StagedMetrics - Tracks a metric across three order stages
// ============================================================================
//
// This template wrapper holds three instances of a base metric class,
// one for each order stage:
// - POSITION: Filled contracts, SOD positions, external position updates
// - OPEN: Acknowledged, live orders
// - IN_FLIGHT: Orders pending acknowledgment or modification
//
// Usage:
//   StagedMetrics<DeltaMetrics> staged_delta;
//   staged_delta.position().global_gross_delta();  // Position delta
//   staged_delta.open_orders().global_gross_delta();  // Open orders delta
//   staged_delta.in_flight().global_gross_delta();  // In-flight delta
//   staged_delta.total_gross_delta();  // Sum of all stages
//

template<typename BaseMetric>
class StagedMetrics {
private:
    BaseMetric position_;
    BaseMetric open_;
    BaseMetric in_flight_;

public:
    StagedMetrics() = default;

    // ========================================================================
    // InstrumentProvider setup
    // ========================================================================

    template<typename Provider>
    void set_instrument_provider(const Provider* provider) {
        position_.set_instrument_provider(provider);
        open_.set_instrument_provider(provider);
        in_flight_.set_instrument_provider(provider);
    }

    // ========================================================================
    // Stage accessors
    // ========================================================================

    BaseMetric& position() { return position_; }
    BaseMetric& open_orders() { return open_; }
    BaseMetric& in_flight() { return in_flight_; }

    const BaseMetric& position() const { return position_; }
    const BaseMetric& open_orders() const { return open_; }
    const BaseMetric& in_flight() const { return in_flight_; }

    // Get metric by stage
    BaseMetric& stage(aggregation::OrderStage s) {
        switch (s) {
            case aggregation::OrderStage::POSITION: return position_;
            case aggregation::OrderStage::OPEN: return open_;
            case aggregation::OrderStage::IN_FLIGHT: return in_flight_;
            default: return position_;
        }
    }

    const BaseMetric& stage(aggregation::OrderStage s) const {
        switch (s) {
            case aggregation::OrderStage::POSITION: return position_;
            case aggregation::OrderStage::OPEN: return open_;
            case aggregation::OrderStage::IN_FLIGHT: return in_flight_;
            default: return position_;
        }
    }

    // ========================================================================
    // Order lifecycle callbacks (route to appropriate stage)
    // ========================================================================

    // Called when order is sent (PENDING_NEW state -> IN_FLIGHT)
    void on_order_added(const engine::TrackedOrder& order) {
        in_flight_.on_order_added(order);
    }

    // Called when order is fully removed (nack, cancel, full fill)
    // Note: For full fills, on_full_fill should be called first to credit position
    void on_order_removed(const engine::TrackedOrder& order) {
        auto stage = aggregation::stage_from_order_state(order.state);
        this->stage(stage).on_order_removed(order);
    }

    // Called when order is modified (update ack)
    void on_order_updated(const engine::TrackedOrder& order, int64_t old_qty) {
        auto stage = aggregation::stage_from_order_state(order.state);
        this->stage(stage).on_order_updated(order, old_qty);
    }

    // Called on partial fill - reduces open stage, credits position stage
    void on_partial_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
        // Reduce open orders by filled quantity
        open_.on_partial_fill(order, filled_qty);
        // Credit position stage with filled quantity
        add_to_position(order.symbol, order.underlyer, order.side, filled_qty);
    }

    // Called on full fill - credits position stage before order removal
    void on_full_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
        // Credit position stage with final fill quantity
        add_to_position(order.symbol, order.underlyer, order.side, filled_qty);
        // Note: The order will be removed from open/in_flight via on_order_removed
    }

    // Called when order state changes (for stage transitions)
    void on_state_change(const engine::TrackedOrder& order,
                         engine::OrderState old_state,
                         engine::OrderState new_state) {
        auto old_stage = aggregation::stage_from_order_state(old_state);
        auto new_stage = aggregation::stage_from_order_state(new_state);

        if (old_stage != new_stage && aggregation::is_active_order_state(new_state)) {
            // Move quantity from old stage to new stage
            this->stage(old_stage).on_order_removed(order);
            this->stage(new_stage).on_order_added(order);
        }
    }

    // ========================================================================
    // Manual position management
    // ========================================================================

    // Add to position stage (for fills, SOD loading, external updates)
    void add_to_position(const std::string& symbol, const std::string& underlyer,
                         fix::Side side, int64_t quantity) {
        position_.add_order(symbol, underlyer, quantity, side);
    }

    // Remove from position stage
    void remove_from_position(const std::string& symbol, const std::string& underlyer,
                              fix::Side side, int64_t quantity) {
        position_.remove_order(symbol, underlyer, quantity, side);
    }

    // Set absolute position (replaces existing)
    void set_position(const std::string& symbol, const std::string& underlyer,
                      fix::Side side, int64_t quantity) {
        // Clear existing and set new
        // This is a simplified implementation - a full one would track by symbol
        add_to_position(symbol, underlyer, side, quantity);
    }

    // Adjust position by delta
    void adjust_position(const std::string& symbol, const std::string& underlyer,
                         fix::Side side, int64_t delta) {
        if (delta > 0) {
            add_to_position(symbol, underlyer, side, delta);
        } else if (delta < 0) {
            remove_from_position(symbol, underlyer, side, -delta);
        }
    }

    // Clear all positions
    void clear_positions() {
        position_.clear();
    }

    // ========================================================================
    // Clear all stages
    // ========================================================================

    void clear() {
        position_.clear();
        open_.clear();
        in_flight_.clear();
    }
};

} // namespace metrics

// Include DeltaMetrics for partial specialization
#include "delta_metrics.hpp"

namespace metrics {

// ============================================================================
// Partial specialization for DeltaMetrics<Provider> - adds combined delta accessors
// ============================================================================

template<typename Provider>
class StagedMetrics<DeltaMetrics<Provider>> {
private:
    using MetricType = DeltaMetrics<Provider>;
    MetricType position_;
    MetricType open_;
    MetricType in_flight_;

public:
    StagedMetrics() = default;

    void set_instrument_provider(const Provider* provider) {
        position_.set_instrument_provider(provider);
        open_.set_instrument_provider(provider);
        in_flight_.set_instrument_provider(provider);
    }

    // Stage accessors
    MetricType& position() { return position_; }
    MetricType& open_orders() { return open_; }
    MetricType& in_flight() { return in_flight_; }
    const MetricType& position() const { return position_; }
    const MetricType& open_orders() const { return open_; }
    const MetricType& in_flight() const { return in_flight_; }

    MetricType& stage(aggregation::OrderStage s) {
        switch (s) {
            case aggregation::OrderStage::POSITION: return position_;
            case aggregation::OrderStage::OPEN: return open_;
            case aggregation::OrderStage::IN_FLIGHT: return in_flight_;
            default: return position_;
        }
    }

    const MetricType& stage(aggregation::OrderStage s) const {
        switch (s) {
            case aggregation::OrderStage::POSITION: return position_;
            case aggregation::OrderStage::OPEN: return open_;
            case aggregation::OrderStage::IN_FLIGHT: return in_flight_;
            default: return position_;
        }
    }

    // Combined delta accessors (sum of all stages)
    double total_gross_delta() const {
        return position_.global_gross_delta() +
               open_.global_gross_delta() +
               in_flight_.global_gross_delta();
    }

    double total_net_delta() const {
        return position_.global_net_delta() +
               open_.global_net_delta() +
               in_flight_.global_net_delta();
    }

    double total_underlyer_gross_delta(const std::string& underlyer) const {
        return position_.underlyer_gross_delta(underlyer) +
               open_.underlyer_gross_delta(underlyer) +
               in_flight_.underlyer_gross_delta(underlyer);
    }

    double total_underlyer_net_delta(const std::string& underlyer) const {
        return position_.underlyer_net_delta(underlyer) +
               open_.underlyer_net_delta(underlyer) +
               in_flight_.underlyer_net_delta(underlyer);
    }

    // Order lifecycle callbacks
    void on_order_added(const engine::TrackedOrder& order) {
        in_flight_.on_order_added(order);
    }

    void on_order_removed(const engine::TrackedOrder& order) {
        auto s = aggregation::stage_from_order_state(order.state);
        stage(s).on_order_removed(order);
    }

    void on_order_updated(const engine::TrackedOrder& order, int64_t old_qty) {
        auto s = aggregation::stage_from_order_state(order.state);
        stage(s).on_order_updated(order, old_qty);
    }

    void on_partial_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
        open_.on_partial_fill(order, filled_qty);
        position_.add_order(order.symbol, order.underlyer, filled_qty, order.side);
    }

    void on_full_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
        position_.add_order(order.symbol, order.underlyer, filled_qty, order.side);
    }

    void on_state_change(const engine::TrackedOrder& order,
                         engine::OrderState old_state,
                         engine::OrderState new_state) {
        auto old_stage = aggregation::stage_from_order_state(old_state);
        auto new_stage = aggregation::stage_from_order_state(new_state);

        if (old_stage != new_stage && aggregation::is_active_order_state(new_state)) {
            stage(old_stage).on_order_removed(order);
            stage(new_stage).on_order_added(order);
        }
    }

    // Position management
    void add_to_position(const std::string& symbol, const std::string& underlyer,
                         fix::Side side, int64_t quantity) {
        position_.add_order(symbol, underlyer, quantity, side);
    }

    void remove_from_position(const std::string& symbol, const std::string& underlyer,
                              fix::Side side, int64_t quantity) {
        position_.remove_order(symbol, underlyer, quantity, side);
    }

    void set_position(const std::string& symbol, const std::string& underlyer,
                      fix::Side side, int64_t quantity) {
        add_to_position(symbol, underlyer, side, quantity);
    }

    void adjust_position(const std::string& symbol, const std::string& underlyer,
                         fix::Side side, int64_t delta) {
        if (delta > 0) {
            add_to_position(symbol, underlyer, side, delta);
        } else if (delta < 0) {
            remove_from_position(symbol, underlyer, side, -delta);
        }
    }

    void clear_positions() { position_.clear(); }
    void clear() {
        position_.clear();
        open_.clear();
        in_flight_.clear();
    }
};

} // namespace metrics
