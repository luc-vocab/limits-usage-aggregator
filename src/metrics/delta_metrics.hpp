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
// Delta Metrics - Tracks gross and net delta at various grouping levels
// ============================================================================
//
// Uses MultiGroupAggregator to automatically aggregate delta values at:
// - Global level (system-wide totals)
// - Per-underlyer level (e.g., AAPL, MSFT)
//

class DeltaMetrics {
private:
    aggregation::MultiGroupAggregator<
        aggregation::DeltaCombiner,
        aggregation::GlobalKey,
        aggregation::UnderlyerKey
    > aggregator_;

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
        aggregator_.bucket<aggregation::GlobalKey>().add(aggregation::GlobalKey::instance(), dv);
        aggregator_.bucket<aggregation::UnderlyerKey>().add(aggregation::UnderlyerKey{underlyer}, dv);
    }

    void remove_order(const std::string& underlyer, double delta_exposure, fix::Side side) {
        auto dv = make_delta_value(delta_exposure, side);
        aggregator_.bucket<aggregation::GlobalKey>().remove(aggregation::GlobalKey::instance(), dv);
        aggregator_.bucket<aggregation::UnderlyerKey>().remove(aggregation::UnderlyerKey{underlyer}, dv);
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
        return aggregator_.get(aggregation::GlobalKey::instance());
    }

    aggregation::DeltaValue underlyer_delta(const std::string& underlyer) const {
        return aggregator_.get(aggregation::UnderlyerKey{underlyer});
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
        return aggregator_.keys<aggregation::UnderlyerKey>();
    }

    void clear() {
        aggregator_.clear();
    }
};

} // namespace metrics

// Include TrackedOrder definition and implement generic interface
#include "../engine/order_state.hpp"

namespace metrics {

inline void DeltaMetrics::on_order_added(const engine::TrackedOrder& order) {
    auto dv = make_delta_value(order.delta_exposure(), order.side);
    aggregator_.add(order, dv);
}

inline void DeltaMetrics::on_order_removed(const engine::TrackedOrder& order) {
    auto dv = make_delta_value(order.delta_exposure(), order.side);
    aggregator_.remove(order, dv);
}

inline void DeltaMetrics::on_order_updated(const engine::TrackedOrder& order,
                                            double old_delta_exposure, double /*old_notional*/) {
    auto old_dv = make_delta_value(old_delta_exposure, order.side);
    auto new_dv = make_delta_value(order.delta_exposure(), order.side);
    aggregator_.update(order, old_dv, new_dv);
}

inline void DeltaMetrics::on_partial_fill(const engine::TrackedOrder& order,
                                           double filled_delta_exposure, double /*filled_notional*/) {
    auto dv = make_delta_value(filled_delta_exposure, order.side);
    aggregator_.remove(order, dv);
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

template<typename Derived>
class AccessorMixin<Derived, metrics::DeltaMetrics> {
protected:
    const metrics::DeltaMetrics& delta_metrics_() const {
        return static_cast<const Derived*>(this)->template get_metric<metrics::DeltaMetrics>();
    }

public:
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
};

} // namespace engine
