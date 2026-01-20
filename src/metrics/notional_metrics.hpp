#pragma once

#include "../aggregation/aggregation_core.hpp"
#include "../fix/fix_types.hpp"

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
    // Add notional for a new order
    void add_order(const std::string& strategy_id, const std::string& portfolio_id, double notional) {
        global_.add(aggregation::GlobalKey::instance(), notional);

        if (!strategy_id.empty()) {
            per_strategy_.add(aggregation::StrategyKey{strategy_id}, notional);
        }
        if (!portfolio_id.empty()) {
            per_portfolio_.add(aggregation::PortfolioKey{portfolio_id}, notional);
        }
    }

    // Remove notional when order is canceled/filled/rejected
    void remove_order(const std::string& strategy_id, const std::string& portfolio_id, double notional) {
        global_.remove(aggregation::GlobalKey::instance(), notional);

        if (!strategy_id.empty()) {
            per_strategy_.remove(aggregation::StrategyKey{strategy_id}, notional);
        }
        if (!portfolio_id.empty()) {
            per_portfolio_.remove(aggregation::PortfolioKey{portfolio_id}, notional);
        }
    }

    // Update notional when order is modified
    void update_order(const std::string& strategy_id, const std::string& portfolio_id,
                      double old_notional, double new_notional) {
        remove_order(strategy_id, portfolio_id, old_notional);
        add_order(strategy_id, portfolio_id, new_notional);
    }

    // Reduce notional on partial fill
    void partial_fill(const std::string& strategy_id, const std::string& portfolio_id, double filled_notional) {
        remove_order(strategy_id, portfolio_id, filled_notional);
    }

    // Accessors
    double global_notional() const {
        return global_.get(aggregation::GlobalKey::instance());
    }

    double strategy_notional(const std::string& strategy_id) const {
        return per_strategy_.get(aggregation::StrategyKey{strategy_id});
    }

    double portfolio_notional(const std::string& portfolio_id) const {
        return per_portfolio_.get(aggregation::PortfolioKey{portfolio_id});
    }

    // Get all strategies with open notional
    std::vector<aggregation::StrategyKey> strategies() const {
        return per_strategy_.keys();
    }

    // Get all portfolios with open notional
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
