#pragma once

#include "../aggregation/multi_group_aggregator.hpp"
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
// Notional Metrics - Tracks open order notional by strategy/portfolio
// ============================================================================
//
// Uses quantity-based tracking with lazy notional computation via InstrumentProvider.
// Notional is computed as: quantity * contract_size * spot_price * fx_rate
//
// Template parameter Provider must satisfy the InstrumentProvider concept.
//
// Tracks quantities at:
// - Global level (system-wide totals)
// - Per-strategy level (if strategy_id is non-empty)
// - Per-portfolio level (if portfolio_id is non-empty)
//

template<typename Provider>
class NotionalMetrics {
    static_assert(instrument::is_notional_provider_v<Provider>,
                  "Provider must satisfy notional provider requirements (spot, fx, contract_size)");

private:
    const Provider* provider_ = nullptr;

    // Track quantities per instrument
    // Key: symbol, Value: quantity
    std::unordered_map<std::string, int64_t> instrument_quantities_;

    // Track notional per strategy/portfolio
    std::unordered_map<std::string, double> strategy_notional_;
    std::unordered_map<std::string, double> portfolio_notional_;

    // Global totals
    int64_t global_qty_ = 0;
    double global_notional_ = 0.0;

    // Compute notional for a single instrument
    double compute_instrument_notional(const std::string& symbol, int64_t quantity) const {
        if (!provider_) {
            return 0.0;
        }
        return instrument::compute_notional(*provider_, symbol, quantity);
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
    // Generic metric interface (used by template RiskAggregationEngine)
    // ========================================================================

    // Called when order is sent (PENDING_NEW state)
    void on_order_added(const engine::TrackedOrder& order);

    // Called when order is fully removed (nack, cancel, full fill)
    void on_order_removed(const engine::TrackedOrder& order);

    // Called when order is modified (update ack)
    void on_order_updated(const engine::TrackedOrder& order, int64_t old_qty);

    // Called on partial fill
    void on_partial_fill(const engine::TrackedOrder& order, int64_t filled_qty);

    // ========================================================================
    // Direct interface (for backward compatibility and direct usage)
    // ========================================================================

    void add_order(const std::string& symbol, const std::string& strategy_id,
                   const std::string& portfolio_id, int64_t quantity) {
        instrument_quantities_[symbol] += quantity;
        global_qty_ += quantity;

        // Compute notional for this quantity and update totals
        double notional = compute_instrument_notional(symbol, quantity);
        global_notional_ += notional;

        if (!strategy_id.empty()) {
            strategy_notional_[strategy_id] += notional;
        }
        if (!portfolio_id.empty()) {
            portfolio_notional_[portfolio_id] += notional;
        }
    }

    void remove_order(const std::string& symbol, const std::string& strategy_id,
                      const std::string& portfolio_id, int64_t quantity) {
        auto it = instrument_quantities_.find(symbol);
        if (it != instrument_quantities_.end()) {
            it->second -= quantity;
            global_qty_ -= quantity;

            // Compute notional for this quantity and update totals
            double notional = compute_instrument_notional(symbol, quantity);
            global_notional_ -= notional;

            if (!strategy_id.empty()) {
                strategy_notional_[strategy_id] -= notional;
            }
            if (!portfolio_id.empty()) {
                portfolio_notional_[portfolio_id] -= notional;
            }

            if (it->second <= 0) {
                instrument_quantities_.erase(it);
                cleanup_mappings(symbol, strategy_id, portfolio_id);
            }
        }
    }

    void update_order(const std::string& symbol, const std::string& strategy_id,
                      const std::string& portfolio_id, int64_t old_qty, int64_t new_qty) {
        remove_order(symbol, strategy_id, portfolio_id, old_qty);
        add_order(symbol, strategy_id, portfolio_id, new_qty);
    }

    void partial_fill(const std::string& symbol, const std::string& strategy_id,
                      const std::string& portfolio_id, int64_t filled_qty) {
        remove_order(symbol, strategy_id, portfolio_id, filled_qty);
    }

    // ========================================================================
    // Accessors - O(1) lookups of precomputed notional values
    // ========================================================================

    // Quantity accessors
    int64_t global_quantity() const { return global_qty_; }

    int64_t instrument_quantity(const std::string& symbol) const {
        auto it = instrument_quantities_.find(symbol);
        return it != instrument_quantities_.end() ? it->second : 0;
    }

    // Notional accessors (O(1) lookups)
    double global_notional() const {
        return global_notional_;
    }

    double strategy_notional(const std::string& strategy_id) const {
        auto it = strategy_notional_.find(strategy_id);
        return it != strategy_notional_.end() ? it->second : 0.0;
    }

    double portfolio_notional(const std::string& portfolio_id) const {
        auto it = portfolio_notional_.find(portfolio_id);
        return it != portfolio_notional_.end() ? it->second : 0.0;
    }

    std::vector<aggregation::StrategyKey> strategies() const {
        std::vector<aggregation::StrategyKey> result;
        for (const auto& [strategy_id, _] : strategy_notional_) {
            result.push_back(aggregation::StrategyKey{strategy_id});
        }
        return result;
    }

    std::vector<aggregation::PortfolioKey> portfolios() const {
        std::vector<aggregation::PortfolioKey> result;
        for (const auto& [portfolio_id, _] : portfolio_notional_) {
            result.push_back(aggregation::PortfolioKey{portfolio_id});
        }
        return result;
    }

    void clear() {
        instrument_quantities_.clear();
        strategy_notional_.clear();
        portfolio_notional_.clear();
        global_qty_ = 0;
        global_notional_ = 0.0;
    }

private:
    void cleanup_mappings([[maybe_unused]] const std::string& symbol,
                          const std::string& strategy_id,
                          const std::string& portfolio_id) {
        // Clean up strategy entry if notional is zero (within floating point tolerance)
        if (!strategy_id.empty()) {
            auto it = strategy_notional_.find(strategy_id);
            if (it != strategy_notional_.end() && std::abs(it->second) < 1e-9) {
                strategy_notional_.erase(it);
            }
        }

        // Clean up portfolio entry if notional is zero (within floating point tolerance)
        if (!portfolio_id.empty()) {
            auto it = portfolio_notional_.find(portfolio_id);
            if (it != portfolio_notional_.end() && std::abs(it->second) < 1e-9) {
                portfolio_notional_.erase(it);
            }
        }
    }
};

} // namespace metrics

// Include TrackedOrder definition and implement generic interface
#include "../engine/order_state.hpp"

namespace metrics {

template<typename Provider>
void NotionalMetrics<Provider>::on_order_added(const engine::TrackedOrder& order) {
    add_order(order.symbol, order.strategy_id, order.portfolio_id, order.leaves_qty);
}

template<typename Provider>
void NotionalMetrics<Provider>::on_order_removed(const engine::TrackedOrder& order) {
    remove_order(order.symbol, order.strategy_id, order.portfolio_id, order.leaves_qty);
}

template<typename Provider>
void NotionalMetrics<Provider>::on_order_updated(const engine::TrackedOrder& order, int64_t old_qty) {
    update_order(order.symbol, order.strategy_id, order.portfolio_id, old_qty, order.leaves_qty);
}

template<typename Provider>
void NotionalMetrics<Provider>::on_partial_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
    partial_fill(order.symbol, order.strategy_id, order.portfolio_id, filled_qty);
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

template<typename Derived, typename Provider>
class AccessorMixin<Derived, metrics::NotionalMetrics<Provider>> {
protected:
    const metrics::NotionalMetrics<Provider>& notional_metrics_() const {
        return static_cast<const Derived*>(this)->template get_metric<metrics::NotionalMetrics<Provider>>();
    }

public:
    double global_notional() const {
        return notional_metrics_().global_notional();
    }

    double strategy_notional(const std::string& strategy_id) const {
        return notional_metrics_().strategy_notional(strategy_id);
    }

    double portfolio_notional(const std::string& portfolio_id) const {
        return notional_metrics_().portfolio_notional(portfolio_id);
    }

    // Quantity accessor
    int64_t global_quantity() const {
        return notional_metrics_().global_quantity();
    }
};

} // namespace engine
