#pragma once

#include "../aggregation/multi_group_aggregator.hpp"
#include "../aggregation/order_stage.hpp"
#include "../fix/fix_types.hpp"
#include "../instrument/instrument.hpp"
#include <unordered_map>
#include <set>

// Forward declarations
namespace engine {
    struct TrackedOrder;
    class OrderBook;
}

namespace metrics {

// ============================================================================
// DeltaData - Internal storage for delta tracking at a single stage
// ============================================================================

template<typename Provider>
struct DeltaData {
    // Track quantities per instrument and side
    std::unordered_map<std::string, int64_t> instrument_bid_qty;
    std::unordered_map<std::string, int64_t> instrument_ask_qty;

    // Track which instruments belong to which underlyer
    std::unordered_map<std::string, std::set<std::string>> underlyer_instruments;

    // Global quantity totals
    int64_t global_bid_qty = 0;
    int64_t global_ask_qty = 0;

    void clear() {
        instrument_bid_qty.clear();
        instrument_ask_qty.clear();
        underlyer_instruments.clear();
        global_bid_qty = 0;
        global_ask_qty = 0;
    }

    void add(const std::string& symbol, const std::string& underlyer,
             int64_t quantity, fix::Side side) {
        underlyer_instruments[underlyer].insert(symbol);

        if (side == fix::Side::BID) {
            instrument_bid_qty[symbol] += quantity;
            global_bid_qty += quantity;
        } else {
            instrument_ask_qty[symbol] += quantity;
            global_ask_qty += quantity;
        }
    }

    void remove(const std::string& symbol, const std::string& underlyer,
                int64_t quantity, fix::Side side) {
        if (side == fix::Side::BID) {
            auto it = instrument_bid_qty.find(symbol);
            if (it != instrument_bid_qty.end()) {
                it->second -= quantity;
                global_bid_qty -= quantity;
                if (it->second <= 0) {
                    instrument_bid_qty.erase(it);
                }
            }
        } else {
            auto it = instrument_ask_qty.find(symbol);
            if (it != instrument_ask_qty.end()) {
                it->second -= quantity;
                global_ask_qty -= quantity;
                if (it->second <= 0) {
                    instrument_ask_qty.erase(it);
                }
            }
        }

        // Clean up underlyer mapping if no more quantities
        bool has_bid = instrument_bid_qty.find(symbol) != instrument_bid_qty.end();
        bool has_ask = instrument_ask_qty.find(symbol) != instrument_ask_qty.end();

        if (!has_bid && !has_ask) {
            auto it = underlyer_instruments.find(underlyer);
            if (it != underlyer_instruments.end()) {
                it->second.erase(symbol);
                if (it->second.empty()) {
                    underlyer_instruments.erase(it);
                }
            }
        }
    }

    // Compute delta for this stage using the provider
    aggregation::DeltaValue compute_delta(const Provider* provider) const {
        double gross = 0.0;
        double net = 0.0;

        for (const auto& [symbol, qty] : instrument_bid_qty) {
            double delta_exp = provider ?
                instrument::compute_delta_exposure(*provider, symbol, qty) : 0.0;
            gross += std::abs(delta_exp);
            net += delta_exp;  // Bids are positive
        }

        for (const auto& [symbol, qty] : instrument_ask_qty) {
            double delta_exp = provider ?
                instrument::compute_delta_exposure(*provider, symbol, qty) : 0.0;
            gross += std::abs(delta_exp);
            net -= delta_exp;  // Asks are negative
        }

        return aggregation::DeltaValue{gross, net};
    }

    aggregation::DeltaValue compute_underlyer_delta(const Provider* provider,
                                                     const std::string& underlyer) const {
        double gross = 0.0;
        double net = 0.0;

        auto it = underlyer_instruments.find(underlyer);
        if (it == underlyer_instruments.end()) {
            return aggregation::DeltaValue{0.0, 0.0};
        }

        for (const auto& symbol : it->second) {
            auto bid_it = instrument_bid_qty.find(symbol);
            if (bid_it != instrument_bid_qty.end()) {
                double delta_exp = provider ?
                    instrument::compute_delta_exposure(*provider, symbol, bid_it->second) : 0.0;
                gross += std::abs(delta_exp);
                net += delta_exp;
            }

            auto ask_it = instrument_ask_qty.find(symbol);
            if (ask_it != instrument_ask_qty.end()) {
                double delta_exp = provider ?
                    instrument::compute_delta_exposure(*provider, symbol, ask_it->second) : 0.0;
                gross += std::abs(delta_exp);
                net -= delta_exp;
            }
        }

        return aggregation::DeltaValue{gross, net};
    }
};

// ============================================================================
// Delta Metrics - Tracks gross and net delta at various grouping levels
// ============================================================================
//
// Uses quantity-based tracking with lazy delta computation via InstrumentProvider.
// Delta exposure is computed as: quantity * delta * contract_size * underlyer_spot * fx_rate
//
// Template parameters:
//   Provider: Must satisfy the InstrumentProvider concept
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//              Default is AllStages which tracks all three stages.
//
// Tracks quantities at:
// - Global level (system-wide totals)
// - Per-underlyer level (e.g., AAPL, MSFT)
// - Per-stage level (position, open, in-flight)
//

template<typename Provider, typename... Stages>
class DeltaMetrics {
    static_assert(instrument::is_option_provider_v<Provider>,
                  "Provider must satisfy option provider requirements (underlyer, delta support)");

    using Config = aggregation::StageConfig<Stages...>;

private:
    const Provider* provider_ = nullptr;

    // Per-stage data storage
    DeltaData<Provider> position_data_;
    DeltaData<Provider> open_data_;
    DeltaData<Provider> in_flight_data_;

    // Get data reference for a given stage
    DeltaData<Provider>& get_stage_data(aggregation::OrderStage stage) {
        switch (stage) {
            case aggregation::OrderStage::POSITION: return position_data_;
            case aggregation::OrderStage::OPEN: return open_data_;
            case aggregation::OrderStage::IN_FLIGHT: return in_flight_data_;
            default: return position_data_;
        }
    }

    const DeltaData<Provider>& get_stage_data(aggregation::OrderStage stage) const {
        switch (stage) {
            case aggregation::OrderStage::POSITION: return position_data_;
            case aggregation::OrderStage::OPEN: return open_data_;
            case aggregation::OrderStage::IN_FLIGHT: return in_flight_data_;
            default: return position_data_;
        }
    }

public:
    DeltaMetrics() = default;

    void set_instrument_provider(const Provider* provider) {
        provider_ = provider;
    }

    const Provider* instrument_provider() const {
        return provider_;
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
    const DeltaData<Provider>& position() const { return position_data_; }
    double position_gross_delta() const {
        return position_data_.compute_delta(provider_).gross;
    }
    double position_net_delta() const {
        return position_data_.compute_delta(provider_).net;
    }
    double position_underlyer_gross_delta(const std::string& underlyer) const {
        return position_data_.compute_underlyer_delta(provider_, underlyer).gross;
    }
    double position_underlyer_net_delta(const std::string& underlyer) const {
        return position_data_.compute_underlyer_delta(provider_, underlyer).net;
    }
    int64_t position_bid_quantity() const { return position_data_.global_bid_qty; }
    int64_t position_ask_quantity() const { return position_data_.global_ask_qty; }

    // Open stage accessors
    const DeltaData<Provider>& open_orders() const { return open_data_; }
    double open_gross_delta() const {
        return open_data_.compute_delta(provider_).gross;
    }
    double open_net_delta() const {
        return open_data_.compute_delta(provider_).net;
    }
    double open_underlyer_gross_delta(const std::string& underlyer) const {
        return open_data_.compute_underlyer_delta(provider_, underlyer).gross;
    }
    double open_underlyer_net_delta(const std::string& underlyer) const {
        return open_data_.compute_underlyer_delta(provider_, underlyer).net;
    }
    int64_t open_bid_quantity() const { return open_data_.global_bid_qty; }
    int64_t open_ask_quantity() const { return open_data_.global_ask_qty; }

    // In-flight stage accessors
    const DeltaData<Provider>& in_flight() const { return in_flight_data_; }
    double in_flight_gross_delta() const {
        return in_flight_data_.compute_delta(provider_).gross;
    }
    double in_flight_net_delta() const {
        return in_flight_data_.compute_delta(provider_).net;
    }
    double in_flight_underlyer_gross_delta(const std::string& underlyer) const {
        return in_flight_data_.compute_underlyer_delta(provider_, underlyer).gross;
    }
    double in_flight_underlyer_net_delta(const std::string& underlyer) const {
        return in_flight_data_.compute_underlyer_delta(provider_, underlyer).net;
    }
    int64_t in_flight_bid_quantity() const { return in_flight_data_.global_bid_qty; }
    int64_t in_flight_ask_quantity() const { return in_flight_data_.global_ask_qty; }

    // ========================================================================
    // Combined/total accessors (sum across all tracked stages)
    // ========================================================================

    // Total delta across all stages (including position)
    double total_gross_delta() const {
        double total = 0.0;
        if constexpr (Config::track_position) total += position_gross_delta();
        if constexpr (Config::track_open) total += open_gross_delta();
        if constexpr (Config::track_in_flight) total += in_flight_gross_delta();
        return total;
    }

    double total_net_delta() const {
        double total = 0.0;
        if constexpr (Config::track_position) total += position_net_delta();
        if constexpr (Config::track_open) total += open_net_delta();
        if constexpr (Config::track_in_flight) total += in_flight_net_delta();
        return total;
    }

    double total_underlyer_gross_delta(const std::string& underlyer) const {
        double total = 0.0;
        if constexpr (Config::track_position) total += position_underlyer_gross_delta(underlyer);
        if constexpr (Config::track_open) total += open_underlyer_gross_delta(underlyer);
        if constexpr (Config::track_in_flight) total += in_flight_underlyer_gross_delta(underlyer);
        return total;
    }

    double total_underlyer_net_delta(const std::string& underlyer) const {
        double total = 0.0;
        if constexpr (Config::track_position) total += position_underlyer_net_delta(underlyer);
        if constexpr (Config::track_open) total += open_underlyer_net_delta(underlyer);
        if constexpr (Config::track_in_flight) total += in_flight_underlyer_net_delta(underlyer);
        return total;
    }

    // ========================================================================
    // Order exposure accessors (open + in-flight only, excludes position)
    // ========================================================================
    //
    // For pre-trade risk checking, "order exposure" excludes realized positions.
    // These accessors return the risk from pending orders only.
    //

    double order_exposure_gross_delta() const {
        double total = 0.0;
        if constexpr (Config::track_open) total += open_gross_delta();
        if constexpr (Config::track_in_flight) total += in_flight_gross_delta();
        return total;
    }

    double order_exposure_net_delta() const {
        double total = 0.0;
        if constexpr (Config::track_open) total += open_net_delta();
        if constexpr (Config::track_in_flight) total += in_flight_net_delta();
        return total;
    }

    double order_exposure_underlyer_gross_delta(const std::string& underlyer) const {
        double total = 0.0;
        if constexpr (Config::track_open) total += open_underlyer_gross_delta(underlyer);
        if constexpr (Config::track_in_flight) total += in_flight_underlyer_gross_delta(underlyer);
        return total;
    }

    double order_exposure_underlyer_net_delta(const std::string& underlyer) const {
        double total = 0.0;
        if constexpr (Config::track_open) total += open_underlyer_net_delta(underlyer);
        if constexpr (Config::track_in_flight) total += in_flight_underlyer_net_delta(underlyer);
        return total;
    }

    // Backward-compatible accessors - return ORDER EXPOSURE (not total with position)
    // For pre-trade risk checks, we want order exposure, not realized position.
    double global_gross_delta() const { return order_exposure_gross_delta(); }
    double global_net_delta() const { return order_exposure_net_delta(); }
    aggregation::DeltaValue global_delta() const {
        return aggregation::DeltaValue{order_exposure_gross_delta(), order_exposure_net_delta()};
    }
    double underlyer_gross_delta(const std::string& underlyer) const {
        return order_exposure_underlyer_gross_delta(underlyer);
    }
    double underlyer_net_delta(const std::string& underlyer) const {
        return order_exposure_underlyer_net_delta(underlyer);
    }
    aggregation::DeltaValue underlyer_delta(const std::string& underlyer) const {
        return aggregation::DeltaValue{order_exposure_underlyer_gross_delta(underlyer),
                                        order_exposure_underlyer_net_delta(underlyer)};
    }

    // Quantity accessors (order exposure - excludes position)
    int64_t global_bid_quantity() const {
        int64_t total = 0;
        if constexpr (Config::track_open) total += open_bid_quantity();
        if constexpr (Config::track_in_flight) total += in_flight_bid_quantity();
        return total;
    }

    int64_t global_ask_quantity() const {
        int64_t total = 0;
        if constexpr (Config::track_open) total += open_ask_quantity();
        if constexpr (Config::track_in_flight) total += in_flight_ask_quantity();
        return total;
    }

    int64_t global_quantity() const {
        return global_bid_quantity() + global_ask_quantity();
    }

    // Total quantity accessors (including position)
    int64_t total_bid_quantity() const {
        int64_t total = 0;
        if constexpr (Config::track_position) total += position_bid_quantity();
        if constexpr (Config::track_open) total += open_bid_quantity();
        if constexpr (Config::track_in_flight) total += in_flight_bid_quantity();
        return total;
    }

    int64_t total_ask_quantity() const {
        int64_t total = 0;
        if constexpr (Config::track_position) total += position_ask_quantity();
        if constexpr (Config::track_open) total += open_ask_quantity();
        if constexpr (Config::track_in_flight) total += in_flight_ask_quantity();
        return total;
    }

    int64_t total_quantity() const {
        return total_bid_quantity() + total_ask_quantity();
    }

    std::vector<aggregation::UnderlyerKey> underlyers() const {
        std::set<std::string> all_underlyers;
        auto collect = [&all_underlyers](const DeltaData<Provider>& data) {
            for (const auto& [underlyer, _] : data.underlyer_instruments) {
                all_underlyers.insert(underlyer);
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

    // Called when order is modified (update ack)
    void on_order_updated(const engine::TrackedOrder& order, int64_t old_qty);

    // Called on partial fill - reduces open stage, credits position stage
    void on_partial_fill(const engine::TrackedOrder& order, int64_t filled_qty);

    // Called on full fill - credits position stage before order removal
    void on_full_fill(const engine::TrackedOrder& order, int64_t filled_qty);

    // Called when order state changes (for stage transitions)
    void on_state_change(const engine::TrackedOrder& order,
                         engine::OrderState old_state,
                         engine::OrderState new_state);

    // Called when order is modified AND changes state (replace ack)
    // Combines stage transition with quantity update
    void on_order_updated_with_state_change(const engine::TrackedOrder& order,
                                             int64_t old_qty,
                                             engine::OrderState old_state,
                                             engine::OrderState new_state);

    // ========================================================================
    // Direct interface (for position management and direct usage)
    // ========================================================================

    void add_order(const std::string& symbol, const std::string& underlyer,
                   int64_t quantity, fix::Side side) {
        // Default: add to in_flight (for backward compatibility)
        in_flight_data_.add(symbol, underlyer, quantity, side);
    }

    void remove_order(const std::string& symbol, const std::string& underlyer,
                      int64_t quantity, fix::Side side) {
        // Default: remove from in_flight (for backward compatibility)
        in_flight_data_.remove(symbol, underlyer, quantity, side);
    }

    // Position management
    void add_to_position(const std::string& symbol, const std::string& underlyer,
                         fix::Side side, int64_t quantity) {
        position_data_.add(symbol, underlyer, quantity, side);
    }

    void remove_from_position(const std::string& symbol, const std::string& underlyer,
                              fix::Side side, int64_t quantity) {
        position_data_.remove(symbol, underlyer, quantity, side);
    }

    void adjust_position(const std::string& symbol, const std::string& underlyer,
                         fix::Side side, int64_t delta) {
        if (delta > 0) {
            add_to_position(symbol, underlyer, side, delta);
        } else if (delta < 0) {
            remove_from_position(symbol, underlyer, side, -delta);
        }
    }

    void clear_positions() {
        position_data_.clear();
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

template<typename Provider, typename... Stages>
void DeltaMetrics<Provider, Stages...>::on_order_added(const engine::TrackedOrder& order) {
    // New orders always start in IN_FLIGHT stage
    in_flight_data_.add(order.symbol, order.underlyer, order.leaves_qty, order.side);
}

template<typename Provider, typename... Stages>
void DeltaMetrics<Provider, Stages...>::on_order_removed(const engine::TrackedOrder& order) {
    // Remove from appropriate stage based on current state
    auto stage = aggregation::stage_from_order_state(order.state);
    get_stage_data(stage).remove(order.symbol, order.underlyer, order.leaves_qty, order.side);
}

template<typename Provider, typename... Stages>
void DeltaMetrics<Provider, Stages...>::on_order_updated(const engine::TrackedOrder& order, int64_t old_qty) {
    // Update in appropriate stage based on current state
    auto stage = aggregation::stage_from_order_state(order.state);
    auto& data = get_stage_data(stage);
    data.remove(order.symbol, order.underlyer, old_qty, order.side);
    data.add(order.symbol, order.underlyer, order.leaves_qty, order.side);
}

template<typename Provider, typename... Stages>
void DeltaMetrics<Provider, Stages...>::on_partial_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
    // Reduce open stage by filled quantity
    open_data_.remove(order.symbol, order.underlyer, filled_qty, order.side);
    // Credit position stage with filled quantity
    position_data_.add(order.symbol, order.underlyer, filled_qty, order.side);
}

template<typename Provider, typename... Stages>
void DeltaMetrics<Provider, Stages...>::on_full_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
    // Credit position stage with final fill quantity
    position_data_.add(order.symbol, order.underlyer, filled_qty, order.side);
    // Note: The order will be removed from open/in_flight via on_order_removed
}

template<typename Provider, typename... Stages>
void DeltaMetrics<Provider, Stages...>::on_state_change(const engine::TrackedOrder& order,
                                                         engine::OrderState old_state,
                                                         engine::OrderState new_state) {
    auto old_stage = aggregation::stage_from_order_state(old_state);
    auto new_stage = aggregation::stage_from_order_state(new_state);

    if (old_stage != new_stage && aggregation::is_active_order_state(new_state)) {
        // Move quantity from old stage to new stage
        get_stage_data(old_stage).remove(order.symbol, order.underlyer, order.leaves_qty, order.side);
        get_stage_data(new_stage).add(order.symbol, order.underlyer, order.leaves_qty, order.side);
    }
}

template<typename Provider, typename... Stages>
void DeltaMetrics<Provider, Stages...>::on_order_updated_with_state_change(
    const engine::TrackedOrder& order,
    int64_t old_qty,
    engine::OrderState old_state,
    engine::OrderState new_state) {

    auto old_stage = aggregation::stage_from_order_state(old_state);
    auto new_stage = aggregation::stage_from_order_state(new_state);

    // Remove old_qty from old stage, add new_qty (leaves_qty) to new stage
    get_stage_data(old_stage).remove(order.symbol, order.underlyer, old_qty, order.side);
    get_stage_data(new_stage).add(order.symbol, order.underlyer, order.leaves_qty, order.side);
}

} // namespace metrics

// ============================================================================
// AccessorMixin specialization for DeltaMetrics
// ============================================================================
//
// This specialization provides CRTP-based accessor methods for engines
// that include DeltaMetrics.
//

#include "../engine/accessor_mixin.hpp"

namespace engine {

template<typename Derived, typename Provider, typename... Stages>
class AccessorMixin<Derived, metrics::DeltaMetrics<Provider, Stages...>> {
protected:
    const metrics::DeltaMetrics<Provider, Stages...>& delta_metrics_() const {
        return static_cast<const Derived*>(this)->template get_metric<metrics::DeltaMetrics<Provider, Stages...>>();
    }

public:
    // Combined/total accessors
    double global_gross_delta() const {
        return delta_metrics_().global_gross_delta();
    }

    double global_net_delta() const {
        return delta_metrics_().global_net_delta();
    }

    double underlyer_gross_delta(const std::string& underlyer) const {
        return delta_metrics_().underlyer_gross_delta(underlyer);
    }

    double underlyer_net_delta(const std::string& underlyer) const {
        return delta_metrics_().underlyer_net_delta(underlyer);
    }

    aggregation::DeltaValue global_delta() const {
        return delta_metrics_().global_delta();
    }

    aggregation::DeltaValue underlyer_delta(const std::string& underlyer) const {
        return delta_metrics_().underlyer_delta(underlyer);
    }

    // Per-stage accessors
    double position_gross_delta() const {
        return delta_metrics_().position_gross_delta();
    }

    double position_net_delta() const {
        return delta_metrics_().position_net_delta();
    }

    double open_gross_delta() const {
        return delta_metrics_().open_gross_delta();
    }

    double open_net_delta() const {
        return delta_metrics_().open_net_delta();
    }

    double in_flight_gross_delta() const {
        return delta_metrics_().in_flight_gross_delta();
    }

    double in_flight_net_delta() const {
        return delta_metrics_().in_flight_net_delta();
    }

    // Quantity accessors
    int64_t global_bid_quantity() const {
        return delta_metrics_().global_bid_quantity();
    }

    int64_t global_ask_quantity() const {
        return delta_metrics_().global_ask_quantity();
    }
};

} // namespace engine
