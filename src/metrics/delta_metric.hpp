#pragma once

#include "../aggregation/staged_metric.hpp"
#include "../aggregation/aggregation_core.hpp"
#include "../aggregation/key_extractors.hpp"
#include "../instrument/instrument.hpp"
#include <cmath>

// Forward declarations
namespace engine {
    struct TrackedOrder;
    enum class OrderState;
}

namespace metrics {

// ============================================================================
// DeltaMetric - Single-purpose delta tracking metric
// ============================================================================
//
// Tracks gross and net delta exposure for a single key type.
// Uses precomputed delta values at add time.
//
// Template parameters:
//   Key: The grouping key type (GlobalKey, UnderlyerKey, etc.)
//   Provider: Must satisfy the option provider requirements (delta support)
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//
// For GlobalKey: tracks total system-wide delta
// For UnderlyerKey: tracks delta per underlyer
//

template<typename Key, typename Provider, typename... Stages>
class DeltaMetric {
    static_assert(instrument::is_option_provider_v<Provider>,
                  "Provider must satisfy option provider requirements (underlyer, delta support)");

private:
    struct StageData {
        aggregation::AggregationBucket<Key, aggregation::DeltaCombiner> delta;

        void clear() {
            delta.clear();
        }
    };

    using Storage = aggregation::StagedMetric<StageData, Stages...>;

    const Provider* provider_ = nullptr;
    Storage storage_;

    // Compute delta value for an order (bid = positive, ask = negative)
    aggregation::DeltaValue compute_delta_value(const engine::TrackedOrder& order) const {
        if (!provider_) {
            return aggregation::DeltaValue{0.0, 0.0};
        }
        double delta_exp = instrument::compute_delta_exposure(*provider_, order.symbol, order.leaves_qty);
        double gross = std::abs(delta_exp);
        double net = (order.side == fix::Side::BID) ? delta_exp : -delta_exp;
        return aggregation::DeltaValue{gross, net};
    }

    aggregation::DeltaValue compute_delta_value(const std::string& symbol, int64_t quantity, fix::Side side) const {
        if (!provider_) {
            return aggregation::DeltaValue{0.0, 0.0};
        }
        double delta_exp = instrument::compute_delta_exposure(*provider_, symbol, quantity);
        double gross = std::abs(delta_exp);
        double net = (side == fix::Side::BID) ? delta_exp : -delta_exp;
        return aggregation::DeltaValue{gross, net};
    }

    Key extract_key(const engine::TrackedOrder& order) const {
        return aggregation::KeyExtractor<Key>::extract(order);
    }

public:
    DeltaMetric() = default;

    void set_instrument_provider(const Provider* provider) {
        provider_ = provider;
    }

    const Provider* instrument_provider() const {
        return provider_;
    }

    // ========================================================================
    // Accessors
    // ========================================================================

    // Get combined delta (gross, net) across all tracked stages for a key
    aggregation::DeltaValue get(const Key& key) const {
        aggregation::DeltaValue total{0.0, 0.0};
        storage_.for_each_stage([&key, &total](const StageData& data) {
            auto val = data.delta.get(key);
            total.gross += val.gross;
            total.net += val.net;
        });
        return total;
    }

    // Get gross delta (absolute sum) across all tracked stages
    double get_gross(const Key& key) const {
        return get(key).gross;
    }

    // Get net delta (signed sum) across all tracked stages
    double get_net(const Key& key) const {
        return get(key).net;
    }

    // Per-stage accessors (only available if stage is tracked)
    template<typename = std::enable_if_t<Storage::Config::track_open>>
    aggregation::DeltaValue get_open(const Key& key) const {
        return storage_.open().delta.get(key);
    }

    template<typename = std::enable_if_t<Storage::Config::track_in_flight>>
    aggregation::DeltaValue get_in_flight(const Key& key) const {
        return storage_.in_flight().delta.get(key);
    }

    template<typename = std::enable_if_t<Storage::Config::track_position>>
    aggregation::DeltaValue get_position(const Key& key) const {
        return storage_.position().delta.get(key);
    }

    // ========================================================================
    // Generic metric interface (used by GenericRiskAggregationEngine)
    // ========================================================================

    void on_order_added(const engine::TrackedOrder& order) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_key(order);
        auto delta_val = compute_delta_value(order);
        auto* stage_data = storage_.get_stage(aggregation::OrderStage::IN_FLIGHT);
        if (stage_data) {
            stage_data->delta.add(key, delta_val);
        }
    }

    void on_order_removed(const engine::TrackedOrder& order) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_key(order);
        auto delta_val = compute_delta_value(order);
        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (stage_data) {
            stage_data->delta.remove(key, delta_val);
        }
    }

    void on_order_updated(const engine::TrackedOrder& order, int64_t old_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_key(order);
        auto old_delta = compute_delta_value(order.symbol, old_qty, order.side);
        auto new_delta = compute_delta_value(order);
        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (stage_data) {
            stage_data->delta.update(key, old_delta, new_delta);
        }
    }

    void on_partial_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_key(order);
        auto filled_delta = compute_delta_value(order.symbol, filled_qty, order.side);

        // Remove from open stage
        auto* open_data = storage_.get_stage(aggregation::OrderStage::OPEN);
        if (open_data) {
            open_data->delta.remove(key, filled_delta);
        }

        // Add to position stage
        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            pos_data->delta.add(key, filled_delta);
        }
    }

    void on_full_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_key(order);
        auto filled_delta = compute_delta_value(order.symbol, filled_qty, order.side);

        // Credit position stage with filled quantity
        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            pos_data->delta.add(key, filled_delta);
        }
    }

    void on_state_change(const engine::TrackedOrder& order,
                         engine::OrderState old_state,
                         engine::OrderState new_state) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        auto old_stage = aggregation::stage_from_order_state(old_state);
        auto new_stage = aggregation::stage_from_order_state(new_state);

        if (old_stage != new_stage && aggregation::is_active_order_state(new_state)) {
            Key key = extract_key(order);
            auto delta_val = compute_delta_value(order);

            auto* old_data = storage_.get_stage(old_stage);
            auto* new_data = storage_.get_stage(new_stage);

            if (old_data) {
                old_data->delta.remove(key, delta_val);
            }
            if (new_data) {
                new_data->delta.add(key, delta_val);
            }
        }
    }

    void on_order_updated_with_state_change(const engine::TrackedOrder& order,
                                             int64_t old_qty,
                                             engine::OrderState old_state,
                                             engine::OrderState new_state) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_key(order);
        auto old_delta = compute_delta_value(order.symbol, old_qty, order.side);
        auto new_delta = compute_delta_value(order);

        auto old_stage = aggregation::stage_from_order_state(old_state);
        auto new_stage = aggregation::stage_from_order_state(new_state);

        auto* old_data = storage_.get_stage(old_stage);
        auto* new_data = storage_.get_stage(new_stage);

        if (old_data) {
            old_data->delta.remove(key, old_delta);
        }
        if (new_data) {
            new_data->delta.add(key, new_delta);
        }
    }

    void clear() {
        storage_.clear();
    }
};

// ============================================================================
// Type aliases for common delta metric configurations
// ============================================================================

// Global delta metric - tracks total system-wide delta
template<typename Provider, typename... Stages>
using GlobalDeltaMetric = DeltaMetric<aggregation::GlobalKey, Provider, Stages...>;

// Per-underlyer delta metric
template<typename Provider, typename... Stages>
using UnderlyerDeltaMetric = DeltaMetric<aggregation::UnderlyerKey, Provider, Stages...>;

} // namespace metrics

// Include TrackedOrder definition for complete type info
#include "../engine/order_state.hpp"
