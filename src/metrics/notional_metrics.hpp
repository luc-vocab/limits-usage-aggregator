#pragma once

#include "../aggregation/multi_group_aggregator.hpp"
#include "../aggregation/order_stage.hpp"
#include "../fix/fix_types.hpp"
#include "../instrument/instrument.hpp"
#include <cmath>
#include <unordered_map>

// Forward declarations
namespace engine {
    struct TrackedOrder;
    class OrderBook;
}

namespace metrics {

// ============================================================================
// NotionalData - Internal storage for notional tracking at a single stage
// ============================================================================

template<typename Provider>
struct NotionalData {
    // Track quantities per instrument
    std::unordered_map<std::string, int64_t> instrument_quantities;

    // Track notional per strategy/portfolio (precomputed)
    std::unordered_map<std::string, double> strategy_notional;
    std::unordered_map<std::string, double> portfolio_notional;

    // Global totals
    int64_t global_qty = 0;
    double global_notional_value = 0.0;

    void clear() {
        instrument_quantities.clear();
        strategy_notional.clear();
        portfolio_notional.clear();
        global_qty = 0;
        global_notional_value = 0.0;
    }

    void add(const Provider* provider, const std::string& symbol,
             const std::string& strategy_id, const std::string& portfolio_id,
             int64_t quantity) {
        instrument_quantities[symbol] += quantity;
        global_qty += quantity;

        double notional = provider ?
            instrument::compute_notional(*provider, symbol, quantity) : 0.0;
        global_notional_value += notional;

        if (!strategy_id.empty()) {
            strategy_notional[strategy_id] += notional;
        }
        if (!portfolio_id.empty()) {
            portfolio_notional[portfolio_id] += notional;
        }
    }

    void remove(const Provider* provider, const std::string& symbol,
                const std::string& strategy_id, const std::string& portfolio_id,
                int64_t quantity) {
        auto it = instrument_quantities.find(symbol);
        if (it != instrument_quantities.end()) {
            it->second -= quantity;
            global_qty -= quantity;

            double notional = provider ?
                instrument::compute_notional(*provider, symbol, quantity) : 0.0;
            global_notional_value -= notional;

            if (!strategy_id.empty()) {
                strategy_notional[strategy_id] -= notional;
            }
            if (!portfolio_id.empty()) {
                portfolio_notional[portfolio_id] -= notional;
            }

            if (it->second <= 0) {
                instrument_quantities.erase(it);
                cleanup_mappings(strategy_id, portfolio_id);
            }
        }
    }

    double get_global_notional() const {
        return global_notional_value;
    }

    double get_strategy_notional(const std::string& strategy_id) const {
        auto it = strategy_notional.find(strategy_id);
        return it != strategy_notional.end() ? it->second : 0.0;
    }

    double get_portfolio_notional(const std::string& portfolio_id) const {
        auto it = portfolio_notional.find(portfolio_id);
        return it != portfolio_notional.end() ? it->second : 0.0;
    }

private:
    void cleanup_mappings(const std::string& strategy_id, const std::string& portfolio_id) {
        if (!strategy_id.empty()) {
            auto it = strategy_notional.find(strategy_id);
            if (it != strategy_notional.end() && std::abs(it->second) < 1e-9) {
                strategy_notional.erase(it);
            }
        }
        if (!portfolio_id.empty()) {
            auto it = portfolio_notional.find(portfolio_id);
            if (it != portfolio_notional.end() && std::abs(it->second) < 1e-9) {
                portfolio_notional.erase(it);
            }
        }
    }
};

// ============================================================================
// Notional Metrics - Tracks open order notional by strategy/portfolio
// ============================================================================
//
// Uses quantity-based tracking with precomputed notional via InstrumentProvider.
// Notional is computed as: quantity * contract_size * spot_price * fx_rate
//
// Template parameters:
//   Provider: Must satisfy the InstrumentProvider concept
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//              Default is AllStages which tracks all three stages.
//
// Tracks quantities at:
// - Global level (system-wide totals)
// - Per-strategy level (if strategy_id is non-empty)
// - Per-portfolio level (if portfolio_id is non-empty)
// - Per-stage level (position, open, in-flight)
//

template<typename Provider, typename... Stages>
class NotionalMetrics {
    static_assert(instrument::is_notional_provider_v<Provider>,
                  "Provider must satisfy notional provider requirements (spot, fx, contract_size)");

    using Config = aggregation::StageConfig<Stages...>;

private:
    const Provider* provider_ = nullptr;

    // Per-stage data storage
    NotionalData<Provider> position_data_;
    NotionalData<Provider> open_data_;
    NotionalData<Provider> in_flight_data_;

    // Get data reference for a given stage
    NotionalData<Provider>& get_stage_data(aggregation::OrderStage stage) {
        switch (stage) {
            case aggregation::OrderStage::POSITION: return position_data_;
            case aggregation::OrderStage::OPEN: return open_data_;
            case aggregation::OrderStage::IN_FLIGHT: return in_flight_data_;
            default: return position_data_;
        }
    }

    const NotionalData<Provider>& get_stage_data(aggregation::OrderStage stage) const {
        switch (stage) {
            case aggregation::OrderStage::POSITION: return position_data_;
            case aggregation::OrderStage::OPEN: return open_data_;
            case aggregation::OrderStage::IN_FLIGHT: return in_flight_data_;
            default: return position_data_;
        }
    }

public:
    NotionalMetrics() = default;

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
    const NotionalData<Provider>& position() const { return position_data_; }
    double position_notional() const { return position_data_.get_global_notional(); }
    double position_strategy_notional(const std::string& strategy_id) const {
        return position_data_.get_strategy_notional(strategy_id);
    }
    double position_portfolio_notional(const std::string& portfolio_id) const {
        return position_data_.get_portfolio_notional(portfolio_id);
    }
    int64_t position_quantity() const { return position_data_.global_qty; }

    // Open stage accessors
    const NotionalData<Provider>& open_orders() const { return open_data_; }
    double open_notional() const { return open_data_.get_global_notional(); }
    double open_strategy_notional(const std::string& strategy_id) const {
        return open_data_.get_strategy_notional(strategy_id);
    }
    double open_portfolio_notional(const std::string& portfolio_id) const {
        return open_data_.get_portfolio_notional(portfolio_id);
    }
    int64_t open_quantity() const { return open_data_.global_qty; }

    // In-flight stage accessors
    const NotionalData<Provider>& in_flight() const { return in_flight_data_; }
    double in_flight_notional() const { return in_flight_data_.get_global_notional(); }
    double in_flight_strategy_notional(const std::string& strategy_id) const {
        return in_flight_data_.get_strategy_notional(strategy_id);
    }
    double in_flight_portfolio_notional(const std::string& portfolio_id) const {
        return in_flight_data_.get_portfolio_notional(portfolio_id);
    }
    int64_t in_flight_quantity() const { return in_flight_data_.global_qty; }

    // ========================================================================
    // Order exposure accessors (open + in-flight only, excludes position)
    // ========================================================================
    //
    // For pre-trade risk checking, "order exposure" excludes realized positions.
    // These accessors return the notional from pending orders only.
    //

    double global_notional() const {
        double total = 0.0;
        if constexpr (Config::track_open) total += open_notional();
        if constexpr (Config::track_in_flight) total += in_flight_notional();
        return total;
    }

    double strategy_notional(const std::string& strategy_id) const {
        double total = 0.0;
        if constexpr (Config::track_open) total += open_strategy_notional(strategy_id);
        if constexpr (Config::track_in_flight) total += in_flight_strategy_notional(strategy_id);
        return total;
    }

    double portfolio_notional(const std::string& portfolio_id) const {
        double total = 0.0;
        if constexpr (Config::track_open) total += open_portfolio_notional(portfolio_id);
        if constexpr (Config::track_in_flight) total += in_flight_portfolio_notional(portfolio_id);
        return total;
    }

    int64_t global_quantity() const {
        int64_t total = 0;
        if constexpr (Config::track_open) total += open_quantity();
        if constexpr (Config::track_in_flight) total += in_flight_quantity();
        return total;
    }

    // ========================================================================
    // Total accessors (including position stage)
    // ========================================================================

    double total_global_notional() const {
        double total = 0.0;
        if constexpr (Config::track_position) total += position_notional();
        if constexpr (Config::track_open) total += open_notional();
        if constexpr (Config::track_in_flight) total += in_flight_notional();
        return total;
    }

    double total_strategy_notional(const std::string& strategy_id) const {
        double total = 0.0;
        if constexpr (Config::track_position) total += position_strategy_notional(strategy_id);
        if constexpr (Config::track_open) total += open_strategy_notional(strategy_id);
        if constexpr (Config::track_in_flight) total += in_flight_strategy_notional(strategy_id);
        return total;
    }

    double total_portfolio_notional(const std::string& portfolio_id) const {
        double total = 0.0;
        if constexpr (Config::track_position) total += position_portfolio_notional(portfolio_id);
        if constexpr (Config::track_open) total += open_portfolio_notional(portfolio_id);
        if constexpr (Config::track_in_flight) total += in_flight_portfolio_notional(portfolio_id);
        return total;
    }

    int64_t total_quantity() const {
        int64_t total = 0;
        if constexpr (Config::track_position) total += position_quantity();
        if constexpr (Config::track_open) total += open_quantity();
        if constexpr (Config::track_in_flight) total += in_flight_quantity();
        return total;
    }

    std::vector<aggregation::StrategyKey> strategies() const {
        std::set<std::string> all_strategies;
        auto collect = [&all_strategies](const NotionalData<Provider>& data) {
            for (const auto& [strategy_id, _] : data.strategy_notional) {
                all_strategies.insert(strategy_id);
            }
        };
        if constexpr (Config::track_position) collect(position_data_);
        if constexpr (Config::track_open) collect(open_data_);
        if constexpr (Config::track_in_flight) collect(in_flight_data_);

        std::vector<aggregation::StrategyKey> result;
        for (const auto& s : all_strategies) {
            result.push_back(aggregation::StrategyKey{s});
        }
        return result;
    }

    std::vector<aggregation::PortfolioKey> portfolios() const {
        std::set<std::string> all_portfolios;
        auto collect = [&all_portfolios](const NotionalData<Provider>& data) {
            for (const auto& [portfolio_id, _] : data.portfolio_notional) {
                all_portfolios.insert(portfolio_id);
            }
        };
        if constexpr (Config::track_position) collect(position_data_);
        if constexpr (Config::track_open) collect(open_data_);
        if constexpr (Config::track_in_flight) collect(in_flight_data_);

        std::vector<aggregation::PortfolioKey> result;
        for (const auto& p : all_portfolios) {
            result.push_back(aggregation::PortfolioKey{p});
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
    // Direct interface (for backward compatibility and direct usage)
    // ========================================================================

    void add_order(const std::string& symbol, const std::string& strategy_id,
                   const std::string& portfolio_id, int64_t quantity) {
        // Default: add to in_flight
        in_flight_data_.add(provider_, symbol, strategy_id, portfolio_id, quantity);
    }

    void remove_order(const std::string& symbol, const std::string& strategy_id,
                      const std::string& portfolio_id, int64_t quantity) {
        // Default: remove from in_flight
        in_flight_data_.remove(provider_, symbol, strategy_id, portfolio_id, quantity);
    }

    void update_order(const std::string& symbol, const std::string& strategy_id,
                      const std::string& portfolio_id, int64_t old_qty, int64_t new_qty) {
        remove_order(symbol, strategy_id, portfolio_id, old_qty);
        add_order(symbol, strategy_id, portfolio_id, new_qty);
    }

    // Position management
    void add_to_position(const std::string& symbol, const std::string& strategy_id,
                         const std::string& portfolio_id, int64_t quantity) {
        position_data_.add(provider_, symbol, strategy_id, portfolio_id, quantity);
    }

    void remove_from_position(const std::string& symbol, const std::string& strategy_id,
                              const std::string& portfolio_id, int64_t quantity) {
        position_data_.remove(provider_, symbol, strategy_id, portfolio_id, quantity);
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
void NotionalMetrics<Provider, Stages...>::on_order_added(const engine::TrackedOrder& order) {
    // New orders always start in IN_FLIGHT stage
    in_flight_data_.add(provider_, order.symbol, order.strategy_id, order.portfolio_id, order.leaves_qty);
}

template<typename Provider, typename... Stages>
void NotionalMetrics<Provider, Stages...>::on_order_removed(const engine::TrackedOrder& order) {
    // Remove from appropriate stage based on current state
    auto stage = aggregation::stage_from_order_state(order.state);
    get_stage_data(stage).remove(provider_, order.symbol, order.strategy_id, order.portfolio_id, order.leaves_qty);
}

template<typename Provider, typename... Stages>
void NotionalMetrics<Provider, Stages...>::on_order_updated(const engine::TrackedOrder& order, int64_t old_qty) {
    // Update in appropriate stage based on current state
    auto stage = aggregation::stage_from_order_state(order.state);
    auto& data = get_stage_data(stage);
    data.remove(provider_, order.symbol, order.strategy_id, order.portfolio_id, old_qty);
    data.add(provider_, order.symbol, order.strategy_id, order.portfolio_id, order.leaves_qty);
}

template<typename Provider, typename... Stages>
void NotionalMetrics<Provider, Stages...>::on_partial_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
    // Reduce open stage by filled quantity
    open_data_.remove(provider_, order.symbol, order.strategy_id, order.portfolio_id, filled_qty);
    // Credit position stage with filled quantity
    position_data_.add(provider_, order.symbol, order.strategy_id, order.portfolio_id, filled_qty);
}

template<typename Provider, typename... Stages>
void NotionalMetrics<Provider, Stages...>::on_full_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
    // Credit position stage with final fill quantity
    position_data_.add(provider_, order.symbol, order.strategy_id, order.portfolio_id, filled_qty);
    // Note: The order will be removed from open/in_flight via on_order_removed
}

template<typename Provider, typename... Stages>
void NotionalMetrics<Provider, Stages...>::on_state_change(const engine::TrackedOrder& order,
                                                            engine::OrderState old_state,
                                                            engine::OrderState new_state) {
    auto old_stage = aggregation::stage_from_order_state(old_state);
    auto new_stage = aggregation::stage_from_order_state(new_state);

    if (old_stage != new_stage && aggregation::is_active_order_state(new_state)) {
        // Move from old stage to new stage
        get_stage_data(old_stage).remove(provider_, order.symbol, order.strategy_id, order.portfolio_id, order.leaves_qty);
        get_stage_data(new_stage).add(provider_, order.symbol, order.strategy_id, order.portfolio_id, order.leaves_qty);
    }
}

template<typename Provider, typename... Stages>
void NotionalMetrics<Provider, Stages...>::on_order_updated_with_state_change(
    const engine::TrackedOrder& order,
    int64_t old_qty,
    engine::OrderState old_state,
    engine::OrderState new_state) {

    auto old_stage = aggregation::stage_from_order_state(old_state);
    auto new_stage = aggregation::stage_from_order_state(new_state);

    // Remove old_qty from old stage, add new_qty (leaves_qty) to new stage
    get_stage_data(old_stage).remove(provider_, order.symbol, order.strategy_id, order.portfolio_id, old_qty);
    get_stage_data(new_stage).add(provider_, order.symbol, order.strategy_id, order.portfolio_id, order.leaves_qty);
}

} // namespace metrics

// ============================================================================
// AccessorMixin specialization for NotionalMetrics
// ============================================================================
//
// This specialization provides CRTP-based accessor methods for engines
// that include NotionalMetrics.
//

#include "../engine/accessor_mixin.hpp"

namespace engine {

template<typename Derived, typename Provider, typename... Stages>
class AccessorMixin<Derived, metrics::NotionalMetrics<Provider, Stages...>> {
protected:
    const metrics::NotionalMetrics<Provider, Stages...>& notional_metrics_() const {
        return static_cast<const Derived*>(this)->template get_metric<metrics::NotionalMetrics<Provider, Stages...>>();
    }

public:
    // Combined accessors
    double global_notional() const {
        return notional_metrics_().global_notional();
    }

    double strategy_notional(const std::string& strategy_id) const {
        return notional_metrics_().strategy_notional(strategy_id);
    }

    double portfolio_notional(const std::string& portfolio_id) const {
        return notional_metrics_().portfolio_notional(portfolio_id);
    }

    int64_t global_quantity() const {
        return notional_metrics_().global_quantity();
    }

    // Per-stage accessors
    double position_notional() const {
        return notional_metrics_().position_notional();
    }

    double open_notional() const {
        return notional_metrics_().open_notional();
    }

    double in_flight_notional() const {
        return notional_metrics_().in_flight_notional();
    }
};

} // namespace engine
