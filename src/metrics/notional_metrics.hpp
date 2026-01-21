#pragma once

#include "../aggregation/aggregation_core.hpp"
#include "../fix/fix_types.hpp"

// Forward declarations
namespace engine {
    struct TrackedOrder;
    class OrderBook;
}

namespace metrics {

// ============================================================================
// Notional Metrics - Tracks open order notional by strategy/portfolio
// ============================================================================

class NotionalMetrics {
private:
    aggregation::StrategyNotionalBucket per_strategy_;
    aggregation::PortfolioNotionalBucket per_portfolio_;
    aggregation::AggregationBucket<aggregation::GlobalKey, aggregation::SumCombiner<double>> global_;

public:
    // ========================================================================
    // Generic metric interface (used by template RiskAggregationEngine)
    // ========================================================================

    // Called when order is sent (PENDING_NEW state)
    void on_order_added(const engine::TrackedOrder& order);

    // Called when order is fully removed (nack, cancel, full fill)
    void on_order_removed(const engine::TrackedOrder& order);

    // Called when order is modified (update ack)
    void on_order_updated(const engine::TrackedOrder& order,
                          double old_delta_exposure, double old_notional);

    // Called on partial fill
    void on_partial_fill(const engine::TrackedOrder& order,
                         double filled_delta_exposure, double filled_notional);

    // ========================================================================
    // Legacy interface (for backward compatibility and direct usage)
    // ========================================================================

    void add_order(const std::string& strategy_id, const std::string& portfolio_id, double notional) {
        global_.add(aggregation::GlobalKey::instance(), notional);

        if (!strategy_id.empty()) {
            per_strategy_.add(aggregation::StrategyKey{strategy_id}, notional);
        }
        if (!portfolio_id.empty()) {
            per_portfolio_.add(aggregation::PortfolioKey{portfolio_id}, notional);
        }
    }

    void remove_order(const std::string& strategy_id, const std::string& portfolio_id, double notional) {
        global_.remove(aggregation::GlobalKey::instance(), notional);

        if (!strategy_id.empty()) {
            per_strategy_.remove(aggregation::StrategyKey{strategy_id}, notional);
        }
        if (!portfolio_id.empty()) {
            per_portfolio_.remove(aggregation::PortfolioKey{portfolio_id}, notional);
        }
    }

    void update_order(const std::string& strategy_id, const std::string& portfolio_id,
                      double old_notional, double new_notional) {
        remove_order(strategy_id, portfolio_id, old_notional);
        add_order(strategy_id, portfolio_id, new_notional);
    }

    void partial_fill(const std::string& strategy_id, const std::string& portfolio_id, double filled_notional) {
        remove_order(strategy_id, portfolio_id, filled_notional);
    }

    // ========================================================================
    // Accessors
    // ========================================================================

    double global_notional() const {
        return global_.get(aggregation::GlobalKey::instance());
    }

    double strategy_notional(const std::string& strategy_id) const {
        return per_strategy_.get(aggregation::StrategyKey{strategy_id});
    }

    double portfolio_notional(const std::string& portfolio_id) const {
        return per_portfolio_.get(aggregation::PortfolioKey{portfolio_id});
    }

    std::vector<aggregation::StrategyKey> strategies() const {
        return per_strategy_.keys();
    }

    std::vector<aggregation::PortfolioKey> portfolios() const {
        return per_portfolio_.keys();
    }

    void clear() {
        global_.clear();
        per_strategy_.clear();
        per_portfolio_.clear();
    }
};

} // namespace metrics

// Include TrackedOrder definition and implement generic interface
#include "../engine/order_state.hpp"

namespace metrics {

inline void NotionalMetrics::on_order_added(const engine::TrackedOrder& order) {
    add_order(order.strategy_id, order.portfolio_id, order.notional());
}

inline void NotionalMetrics::on_order_removed(const engine::TrackedOrder& order) {
    remove_order(order.strategy_id, order.portfolio_id, order.notional());
}

inline void NotionalMetrics::on_order_updated(const engine::TrackedOrder& order,
                                               double /*old_delta_exposure*/, double old_notional) {
    update_order(order.strategy_id, order.portfolio_id, old_notional, order.notional());
}

inline void NotionalMetrics::on_partial_fill(const engine::TrackedOrder& order,
                                              double /*filled_delta_exposure*/, double filled_notional) {
    partial_fill(order.strategy_id, order.portfolio_id, filled_notional);
}

} // namespace metrics
