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
// Delta Metrics - Tracks gross and net delta at various grouping levels
// ============================================================================

class DeltaMetrics {
private:
    aggregation::GlobalDeltaBucket global_;
    aggregation::UnderlyerDeltaBucket per_underlyer_;

    aggregation::DeltaValue make_delta_value(double delta_exposure, fix::Side side) const {
        double signed_delta = (side == fix::Side::BID) ? delta_exposure : -delta_exposure;
        return aggregation::DeltaValue{std::abs(delta_exposure), signed_delta};
    }

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

    void add_order(const std::string& underlyer, double delta_exposure, fix::Side side) {
        auto dv = make_delta_value(delta_exposure, side);
        global_.add(aggregation::GlobalKey::instance(), dv);
        per_underlyer_.add(aggregation::UnderlyerKey{underlyer}, dv);
    }

    void remove_order(const std::string& underlyer, double delta_exposure, fix::Side side) {
        auto dv = make_delta_value(delta_exposure, side);
        global_.remove(aggregation::GlobalKey::instance(), dv);
        per_underlyer_.remove(aggregation::UnderlyerKey{underlyer}, dv);
    }

    void update_order(const std::string& underlyer,
                      double old_delta_exposure, double new_delta_exposure,
                      fix::Side side) {
        remove_order(underlyer, old_delta_exposure, side);
        add_order(underlyer, new_delta_exposure, side);
    }

    void partial_fill(const std::string& underlyer, double filled_delta, fix::Side side) {
        remove_order(underlyer, filled_delta, side);
    }

    // ========================================================================
    // Accessors
    // ========================================================================

    aggregation::DeltaValue global_delta() const {
        return global_.get(aggregation::GlobalKey::instance());
    }

    aggregation::DeltaValue underlyer_delta(const std::string& underlyer) const {
        return per_underlyer_.get(aggregation::UnderlyerKey{underlyer});
    }

    double global_gross_delta() const {
        return global_delta().gross;
    }

    double global_net_delta() const {
        return global_delta().net;
    }

    double underlyer_gross_delta(const std::string& underlyer) const {
        return underlyer_delta(underlyer).gross;
    }

    double underlyer_net_delta(const std::string& underlyer) const {
        return underlyer_delta(underlyer).net;
    }

    std::vector<aggregation::UnderlyerKey> underlyers() const {
        return per_underlyer_.keys();
    }

    void clear() {
        global_.clear();
        per_underlyer_.clear();
    }
};

} // namespace metrics

// Include TrackedOrder definition and implement generic interface
#include "../engine/order_state.hpp"

namespace metrics {

inline void DeltaMetrics::on_order_added(const engine::TrackedOrder& order) {
    add_order(order.underlyer, order.delta_exposure(), order.side);
}

inline void DeltaMetrics::on_order_removed(const engine::TrackedOrder& order) {
    remove_order(order.underlyer, order.delta_exposure(), order.side);
}

inline void DeltaMetrics::on_order_updated(const engine::TrackedOrder& order,
                                            double old_delta_exposure, double /*old_notional*/) {
    update_order(order.underlyer, old_delta_exposure, order.delta_exposure(), order.side);
}

inline void DeltaMetrics::on_partial_fill(const engine::TrackedOrder& order,
                                           double filled_delta_exposure, double /*filled_notional*/) {
    partial_fill(order.underlyer, filled_delta_exposure, order.side);
}

} // namespace metrics
