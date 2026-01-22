#pragma once

#include "../aggregation/multi_group_aggregator.hpp"
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
// Notional Metrics - Tracks open order notional by strategy/portfolio
// ============================================================================
//
// Uses quantity-based tracking with lazy notional computation via InstrumentProvider.
// Notional is computed as: quantity * contract_size * spot_price * fx_rate
//
// Tracks quantities at:
// - Global level (system-wide totals)
// - Per-strategy level (if strategy_id is non-empty)
// - Per-portfolio level (if portfolio_id is non-empty)
//

class NotionalMetrics {
private:
    instrument::InstrumentProvider* provider_ = nullptr;

    // Track quantities per instrument
    // Key: symbol, Value: quantity
    std::unordered_map<std::string, int64_t> instrument_quantities_;

    // Track which instruments belong to which strategy/portfolio
    std::unordered_map<std::string, std::set<std::string>> strategy_instruments_;
    std::unordered_map<std::string, std::set<std::string>> portfolio_instruments_;

    // Global quantity total
    int64_t global_qty_ = 0;

    // Compute notional for a single instrument
    double compute_instrument_notional(const std::string& symbol, int64_t quantity) const {
        if (!provider_) {
            return 0.0;
        }
        return provider_->compute_notional(symbol, quantity);
    }

public:
    NotionalMetrics() = default;

    void set_instrument_provider(instrument::InstrumentProvider* provider) {
        provider_ = provider;
    }

    instrument::InstrumentProvider* instrument_provider() const {
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
    // Legacy interface (for backward compatibility and direct usage)
    // ========================================================================

    void add_order(const std::string& symbol, const std::string& strategy_id,
                   const std::string& portfolio_id, int64_t quantity) {
        instrument_quantities_[symbol] += quantity;
        global_qty_ += quantity;

        // Track strategy/portfolio mappings
        if (!strategy_id.empty()) {
            strategy_instruments_[strategy_id].insert(symbol);
        }
        if (!portfolio_id.empty()) {
            portfolio_instruments_[portfolio_id].insert(symbol);
        }
    }

    void remove_order(const std::string& symbol, const std::string& strategy_id,
                      const std::string& portfolio_id, int64_t quantity) {
        auto it = instrument_quantities_.find(symbol);
        if (it != instrument_quantities_.end()) {
            it->second -= quantity;
            global_qty_ -= quantity;
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
    // Accessors - compute notional values lazily using InstrumentProvider
    // ========================================================================

    // Quantity accessors
    int64_t global_quantity() const { return global_qty_; }

    int64_t instrument_quantity(const std::string& symbol) const {
        auto it = instrument_quantities_.find(symbol);
        return it != instrument_quantities_.end() ? it->second : 0;
    }

    // Notional accessors (computed lazily)
    double global_notional() const {
        double total = 0.0;
        for (const auto& [symbol, qty] : instrument_quantities_) {
            total += compute_instrument_notional(symbol, qty);
        }
        return total;
    }

    double strategy_notional(const std::string& strategy_id) const {
        auto it = strategy_instruments_.find(strategy_id);
        if (it == strategy_instruments_.end()) {
            return 0.0;
        }

        double total = 0.0;
        for (const auto& symbol : it->second) {
            auto qty_it = instrument_quantities_.find(symbol);
            if (qty_it != instrument_quantities_.end()) {
                total += compute_instrument_notional(symbol, qty_it->second);
            }
        }
        return total;
    }

    double portfolio_notional(const std::string& portfolio_id) const {
        auto it = portfolio_instruments_.find(portfolio_id);
        if (it == portfolio_instruments_.end()) {
            return 0.0;
        }

        double total = 0.0;
        for (const auto& symbol : it->second) {
            auto qty_it = instrument_quantities_.find(symbol);
            if (qty_it != instrument_quantities_.end()) {
                total += compute_instrument_notional(symbol, qty_it->second);
            }
        }
        return total;
    }

    std::vector<aggregation::StrategyKey> strategies() const {
        std::vector<aggregation::StrategyKey> result;
        for (const auto& [strategy_id, _] : strategy_instruments_) {
            result.push_back(aggregation::StrategyKey{strategy_id});
        }
        return result;
    }

    std::vector<aggregation::PortfolioKey> portfolios() const {
        std::vector<aggregation::PortfolioKey> result;
        for (const auto& [portfolio_id, _] : portfolio_instruments_) {
            result.push_back(aggregation::PortfolioKey{portfolio_id});
        }
        return result;
    }

    void clear() {
        instrument_quantities_.clear();
        strategy_instruments_.clear();
        portfolio_instruments_.clear();
        global_qty_ = 0;
    }

private:
    void cleanup_mappings(const std::string& symbol, const std::string& strategy_id,
                          const std::string& portfolio_id) {
        // Clean up strategy mapping
        if (!strategy_id.empty()) {
            auto it = strategy_instruments_.find(strategy_id);
            if (it != strategy_instruments_.end()) {
                it->second.erase(symbol);
                if (it->second.empty()) {
                    strategy_instruments_.erase(it);
                }
            }
        }

        // Clean up portfolio mapping
        if (!portfolio_id.empty()) {
            auto it = portfolio_instruments_.find(portfolio_id);
            if (it != portfolio_instruments_.end()) {
                it->second.erase(symbol);
                if (it->second.empty()) {
                    portfolio_instruments_.erase(it);
                }
            }
        }
    }
};

} // namespace metrics

// Include TrackedOrder definition and implement generic interface
#include "../engine/order_state.hpp"

namespace metrics {

inline void NotionalMetrics::on_order_added(const engine::TrackedOrder& order) {
    add_order(order.symbol, order.strategy_id, order.portfolio_id, order.leaves_qty);
}

inline void NotionalMetrics::on_order_removed(const engine::TrackedOrder& order) {
    remove_order(order.symbol, order.strategy_id, order.portfolio_id, order.leaves_qty);
}

inline void NotionalMetrics::on_order_updated(const engine::TrackedOrder& order, int64_t old_qty) {
    update_order(order.symbol, order.strategy_id, order.portfolio_id, old_qty, order.leaves_qty);
}

inline void NotionalMetrics::on_partial_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
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

template<typename Derived>
class AccessorMixin<Derived, metrics::NotionalMetrics> {
protected:
    const metrics::NotionalMetrics& notional_metrics_() const {
        return static_cast<const Derived*>(this)->template get_metric<metrics::NotionalMetrics>();
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
