#pragma once

#include "../aggregation/multi_group_aggregator.hpp"
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
//
// Uses MultiGroupAggregator to automatically aggregate notional values at:
// - Global level (system-wide totals)
// - Per-strategy level (if strategy_id is non-empty)
// - Per-portfolio level (if portfolio_id is non-empty)
//

class NotionalMetrics {
private:
    aggregation::MultiGroupAggregator<
        aggregation::SumCombiner<double>,
        aggregation::GlobalKey,
        aggregation::StrategyKey,
        aggregation::PortfolioKey
    > aggregator_;

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
        aggregator_.bucket<aggregation::GlobalKey>().add(aggregation::GlobalKey::instance(), notional);

        if (!strategy_id.empty()) {
            aggregator_.bucket<aggregation::StrategyKey>().add(aggregation::StrategyKey{strategy_id}, notional);
        }
        if (!portfolio_id.empty()) {
            aggregator_.bucket<aggregation::PortfolioKey>().add(aggregation::PortfolioKey{portfolio_id}, notional);
        }
    }

    void remove_order(const std::string& strategy_id, const std::string& portfolio_id, double notional) {
        aggregator_.bucket<aggregation::GlobalKey>().remove(aggregation::GlobalKey::instance(), notional);

        if (!strategy_id.empty()) {
            aggregator_.bucket<aggregation::StrategyKey>().remove(aggregation::StrategyKey{strategy_id}, notional);
        }
        if (!portfolio_id.empty()) {
            aggregator_.bucket<aggregation::PortfolioKey>().remove(aggregation::PortfolioKey{portfolio_id}, notional);
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
        return aggregator_.get(aggregation::GlobalKey::instance());
    }

    double strategy_notional(const std::string& strategy_id) const {
        return aggregator_.get(aggregation::StrategyKey{strategy_id});
    }

    double portfolio_notional(const std::string& portfolio_id) const {
        return aggregator_.get(aggregation::PortfolioKey{portfolio_id});
    }

    std::vector<aggregation::StrategyKey> strategies() const {
        return aggregator_.keys<aggregation::StrategyKey>();
    }

    std::vector<aggregation::PortfolioKey> portfolios() const {
        return aggregator_.keys<aggregation::PortfolioKey>();
    }

    void clear() {
        aggregator_.clear();
    }
};

} // namespace metrics

// Include TrackedOrder definition and implement generic interface
#include "../engine/order_state.hpp"

namespace metrics {

inline void NotionalMetrics::on_order_added(const engine::TrackedOrder& order) {
    aggregator_.add(order, order.notional());
}

inline void NotionalMetrics::on_order_removed(const engine::TrackedOrder& order) {
    aggregator_.remove(order, order.notional());
}

inline void NotionalMetrics::on_order_updated(const engine::TrackedOrder& order,
                                               double /*old_delta_exposure*/, double old_notional) {
    aggregator_.update(order, old_notional, order.notional());
}

inline void NotionalMetrics::on_partial_fill(const engine::TrackedOrder& order,
                                              double /*filled_delta_exposure*/, double filled_notional) {
    aggregator_.remove(order, filled_notional);
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
};

} // namespace engine
