#pragma once

#include "../aggregation/aggregation_core.hpp"
#include "../fix/fix_types.hpp"

namespace metrics {

// ============================================================================
// Delta Metrics - Tracks gross and net delta at various grouping levels
// ============================================================================

class DeltaMetrics {
private:
    aggregation::GlobalDeltaBucket global_;
    aggregation::UnderlyerDeltaBucket per_underlyer_;

public:
    // Add delta exposure for a new/pending order
    void add_order(const std::string& underlyer, double delta_exposure, fix::Side side) {
        // Net delta: positive for bids (buying), negative for asks (selling)
        double signed_delta = (side == fix::Side::BID) ? delta_exposure : -delta_exposure;
        aggregation::DeltaValue dv{std::abs(delta_exposure), signed_delta};

        global_.add(aggregation::GlobalKey::instance(), dv);
        per_underlyer_.add(aggregation::UnderlyerKey{underlyer}, dv);
    }

    // Remove delta exposure when order is canceled/filled/rejected
    void remove_order(const std::string& underlyer, double delta_exposure, fix::Side side) {
        double signed_delta = (side == fix::Side::BID) ? delta_exposure : -delta_exposure;
        aggregation::DeltaValue dv{std::abs(delta_exposure), signed_delta};

        global_.remove(aggregation::GlobalKey::instance(), dv);
        per_underlyer_.remove(aggregation::UnderlyerKey{underlyer}, dv);
    }

    // Update delta exposure when order is modified
    void update_order(const std::string& underlyer,
                      double old_delta_exposure, double new_delta_exposure,
                      fix::Side side) {
        remove_order(underlyer, old_delta_exposure, side);
        add_order(underlyer, new_delta_exposure, side);
    }

    // Reduce delta exposure on partial fill
    void partial_fill(const std::string& underlyer, double filled_delta, fix::Side side) {
        remove_order(underlyer, filled_delta, side);
    }

    // Accessors
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

    // Get all underlyers with delta exposure
    std::vector<aggregation::UnderlyerKey> underlyers() const {
        return per_underlyer_.keys();
    }

    void clear() {
        global_.clear();
        per_underlyer_.clear();
    }
};

} // namespace metrics
