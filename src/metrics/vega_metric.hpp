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
    enum class LimitType;
}

namespace metrics {

// ============================================================================
// GrossVegaMetric - Single-value metric tracking absolute vega exposure
// ============================================================================
//
// Tracks only gross (absolute) vega exposure. Designed for use with the
// generic limit checking system where each metric has one value type.
//
// Template parameters:
//   Key: The grouping key type (GlobalKey, UnderlyerKey, etc.)
//   Instrument: Must satisfy the vega instrument requirements (vega support)
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//

template<typename Key, typename Instrument, typename... Stages>
class GrossVegaMetric {
    static_assert(instrument::is_vega_instrument_v<Instrument>,
                  "Instrument must satisfy vega instrument requirements (vega support)");

public:
    using key_type = Key;
    using value_type = double;
    using instrument_type = Instrument;
    using Config = aggregation::StageConfig<Stages...>;

    // ========================================================================
    // Static methods for pre-trade limit checking
    // ========================================================================

    // Compute the gross vega contribution for a new order
    template<typename Inst>
    static double compute_order_contribution(
        const fix::NewOrderSingle& order,
        const Inst& instrument) {
        double vega_exp = instrument::compute_vega_exposure(instrument, order.quantity);
        return std::abs(vega_exp);
    }

    // Compute the gross vega contribution for an order update (new - old)
    template<typename Inst>
    static double compute_update_contribution(
        const fix::OrderCancelReplaceRequest& update,
        const engine::TrackedOrder& existing_order,
        const Inst& instrument) {
        double old_vega = std::abs(instrument::compute_vega_exposure(
            instrument, existing_order.leaves_qty));
        double new_vega = std::abs(instrument::compute_vega_exposure(
            instrument, update.quantity));
        return new_vega - old_vega;
    }

    // Extract the key from a NewOrderSingle
    static Key extract_key(const fix::NewOrderSingle& order) {
        if constexpr (std::is_same_v<Key, aggregation::GlobalKey>) {
            return aggregation::GlobalKey::instance();
        } else if constexpr (std::is_same_v<Key, aggregation::UnderlyerKey>) {
            return Key{order.underlyer};
        } else if constexpr (std::is_same_v<Key, aggregation::InstrumentKey>) {
            return Key{order.symbol};
        } else if constexpr (std::is_same_v<Key, aggregation::StrategyKey>) {
            return Key{order.strategy_id};
        } else if constexpr (std::is_same_v<Key, aggregation::PortfolioKey>) {
            return Key{order.portfolio_id};
        } else {
            static_assert(sizeof(Key) == 0, "Unsupported key type for GrossVegaMetric");
        }
    }

    // Get the limit type for this metric
    static constexpr engine::LimitType limit_type() {
        return engine::LimitType::GROSS_VEGA;
    }

private:
    struct StageData {
        aggregation::AggregationBucket<Key, aggregation::SumCombiner<double>> gross_vega;

        void clear() {
            gross_vega.clear();
        }
    };

    using Storage = aggregation::StagedMetric<StageData, Stages...>;

    Storage storage_;

    static double compute_gross(const Instrument& instrument, int64_t quantity) {
        double vega_exp = instrument::compute_vega_exposure(instrument, quantity);
        return std::abs(vega_exp);
    }

    Key extract_order_key(const engine::TrackedOrder& order) const {
        return aggregation::KeyExtractor<Key>::extract(order);
    }

public:
    GrossVegaMetric() = default;

    // ========================================================================
    // Accessors
    // ========================================================================

    double get(const Key& key) const {
        double total = 0.0;
        storage_.for_each_stage([&key, &total](aggregation::OrderStage /*stage*/, const StageData& data) {
            total += data.gross_vega.get(key);
        });
        return total;
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_open && std::is_void_v<Dummy>, double>
    get_open(const Key& key) const {
        return storage_.open().gross_vega.get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_in_flight && std::is_void_v<Dummy>, double>
    get_in_flight(const Key& key) const {
        return storage_.in_flight().gross_vega.get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_position && std::is_void_v<Dummy>, double>
    get_position(const Key& key) const {
        return storage_.position().gross_vega.get(key);
    }

    // ========================================================================
    // Generic metric interface
    // ========================================================================

    void on_order_added(const engine::TrackedOrder& order, const Instrument& instrument) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double gross = compute_gross(instrument, order.leaves_qty);
        auto* stage_data = storage_.get_stage(aggregation::OrderStage::IN_FLIGHT);
        if (stage_data) {
            stage_data->gross_vega.add(key, gross);
        }
    }

    void on_order_removed(const engine::TrackedOrder& order, const Instrument& instrument) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double gross = compute_gross(instrument, order.leaves_qty);
        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (stage_data) {
            stage_data->gross_vega.remove(key, gross);
        }
    }

    void on_order_updated(const engine::TrackedOrder& order, const Instrument& instrument, int64_t old_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double old_gross = compute_gross(instrument, old_qty);
        double new_gross = compute_gross(instrument, order.leaves_qty);
        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (stage_data) {
            stage_data->gross_vega.remove(key, old_gross);
            stage_data->gross_vega.add(key, new_gross);
        }
    }

    void on_partial_fill(const engine::TrackedOrder& order, const Instrument& instrument, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double filled_gross = compute_gross(instrument, filled_qty);

        auto* open_data = storage_.get_stage(aggregation::OrderStage::OPEN);
        if (open_data) {
            open_data->gross_vega.remove(key, filled_gross);
        }

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            pos_data->gross_vega.add(key, filled_gross);
        }
    }

    void on_full_fill(const engine::TrackedOrder& order, const Instrument& instrument, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double filled_gross = compute_gross(instrument, filled_qty);

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            pos_data->gross_vega.add(key, filled_gross);
        }
    }

    void on_state_change(const engine::TrackedOrder& order, const Instrument& instrument,
                         engine::OrderState old_state,
                         engine::OrderState new_state) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        auto old_stage = aggregation::stage_from_order_state(old_state);
        auto new_stage = aggregation::stage_from_order_state(new_state);

        if (old_stage != new_stage && aggregation::is_active_order_state(new_state)) {
            Key key = extract_order_key(order);
            double gross = compute_gross(instrument, order.leaves_qty);

            auto* old_data = storage_.get_stage(old_stage);
            auto* new_data = storage_.get_stage(new_stage);

            if (old_data) {
                old_data->gross_vega.remove(key, gross);
            }
            if (new_data) {
                new_data->gross_vega.add(key, gross);
            }
        }
    }

    void on_order_updated_with_state_change(const engine::TrackedOrder& order, const Instrument& instrument,
                                             int64_t old_qty,
                                             engine::OrderState old_state,
                                             engine::OrderState new_state) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double old_gross = compute_gross(instrument, old_qty);
        double new_gross = compute_gross(instrument, order.leaves_qty);

        auto old_stage = aggregation::stage_from_order_state(old_state);
        auto new_stage = aggregation::stage_from_order_state(new_state);

        auto* old_data = storage_.get_stage(old_stage);
        auto* new_data = storage_.get_stage(new_stage);

        if (old_data) {
            old_data->gross_vega.remove(key, old_gross);
        }
        if (new_data) {
            new_data->gross_vega.add(key, new_gross);
        }
    }

    void clear() {
        storage_.clear();
    }
};

// ============================================================================
// NetVegaMetric - Single-value metric tracking signed vega exposure
// ============================================================================
//
// Tracks only net (signed) vega exposure. Designed for use with the
// generic limit checking system where each metric has one value type.
//

template<typename Key, typename Instrument, typename... Stages>
class NetVegaMetric {
    static_assert(instrument::is_vega_instrument_v<Instrument>,
                  "Instrument must satisfy vega instrument requirements (vega support)");

public:
    using key_type = Key;
    using value_type = double;
    using instrument_type = Instrument;
    using Config = aggregation::StageConfig<Stages...>;

    // ========================================================================
    // Static methods for pre-trade limit checking
    // ========================================================================

    // Compute the net vega contribution for a new order
    template<typename Inst>
    static double compute_order_contribution(
        const fix::NewOrderSingle& order,
        const Inst& instrument) {
        double vega_exp = instrument::compute_vega_exposure(instrument, order.quantity);
        return (order.side == fix::Side::BID) ? vega_exp : -vega_exp;
    }

    // Compute the net vega contribution for an order update (new - old)
    template<typename Inst>
    static double compute_update_contribution(
        const fix::OrderCancelReplaceRequest& update,
        const engine::TrackedOrder& existing_order,
        const Inst& instrument) {
        double old_vega = instrument::compute_vega_exposure(
            instrument, existing_order.leaves_qty);
        double old_net = (existing_order.side == fix::Side::BID) ? old_vega : -old_vega;

        double new_vega = instrument::compute_vega_exposure(
            instrument, update.quantity);
        double new_net = (update.side == fix::Side::BID) ? new_vega : -new_vega;

        return new_net - old_net;
    }

    // Extract the key from a NewOrderSingle
    static Key extract_key(const fix::NewOrderSingle& order) {
        if constexpr (std::is_same_v<Key, aggregation::GlobalKey>) {
            return aggregation::GlobalKey::instance();
        } else if constexpr (std::is_same_v<Key, aggregation::UnderlyerKey>) {
            return Key{order.underlyer};
        } else if constexpr (std::is_same_v<Key, aggregation::InstrumentKey>) {
            return Key{order.symbol};
        } else if constexpr (std::is_same_v<Key, aggregation::StrategyKey>) {
            return Key{order.strategy_id};
        } else if constexpr (std::is_same_v<Key, aggregation::PortfolioKey>) {
            return Key{order.portfolio_id};
        } else {
            static_assert(sizeof(Key) == 0, "Unsupported key type for NetVegaMetric");
        }
    }

    // Get the limit type for this metric
    static constexpr engine::LimitType limit_type() {
        return engine::LimitType::NET_VEGA;
    }

private:
    struct StageData {
        aggregation::AggregationBucket<Key, aggregation::SumCombiner<double>> net_vega;

        void clear() {
            net_vega.clear();
        }
    };

    using Storage = aggregation::StagedMetric<StageData, Stages...>;

    Storage storage_;

    static double compute_net(const Instrument& instrument, int64_t quantity, fix::Side side) {
        double vega_exp = instrument::compute_vega_exposure(instrument, quantity);
        return (side == fix::Side::BID) ? vega_exp : -vega_exp;
    }

    Key extract_order_key(const engine::TrackedOrder& order) const {
        return aggregation::KeyExtractor<Key>::extract(order);
    }

public:
    NetVegaMetric() = default;

    // ========================================================================
    // Accessors
    // ========================================================================

    double get(const Key& key) const {
        double total = 0.0;
        storage_.for_each_stage([&key, &total](aggregation::OrderStage /*stage*/, const StageData& data) {
            total += data.net_vega.get(key);
        });
        return total;
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_open && std::is_void_v<Dummy>, double>
    get_open(const Key& key) const {
        return storage_.open().net_vega.get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_in_flight && std::is_void_v<Dummy>, double>
    get_in_flight(const Key& key) const {
        return storage_.in_flight().net_vega.get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_position && std::is_void_v<Dummy>, double>
    get_position(const Key& key) const {
        return storage_.position().net_vega.get(key);
    }

    // ========================================================================
    // Generic metric interface
    // ========================================================================

    void on_order_added(const engine::TrackedOrder& order, const Instrument& instrument) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double net = compute_net(instrument, order.leaves_qty, order.side);
        auto* stage_data = storage_.get_stage(aggregation::OrderStage::IN_FLIGHT);
        if (stage_data) {
            stage_data->net_vega.add(key, net);
        }
    }

    void on_order_removed(const engine::TrackedOrder& order, const Instrument& instrument) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double net = compute_net(instrument, order.leaves_qty, order.side);
        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (stage_data) {
            stage_data->net_vega.remove(key, net);
        }
    }

    void on_order_updated(const engine::TrackedOrder& order, const Instrument& instrument, int64_t old_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double old_net = compute_net(instrument, old_qty, order.side);
        double new_net = compute_net(instrument, order.leaves_qty, order.side);
        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (stage_data) {
            stage_data->net_vega.remove(key, old_net);
            stage_data->net_vega.add(key, new_net);
        }
    }

    void on_partial_fill(const engine::TrackedOrder& order, const Instrument& instrument, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double filled_net = compute_net(instrument, filled_qty, order.side);

        auto* open_data = storage_.get_stage(aggregation::OrderStage::OPEN);
        if (open_data) {
            open_data->net_vega.remove(key, filled_net);
        }

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            pos_data->net_vega.add(key, filled_net);
        }
    }

    void on_full_fill(const engine::TrackedOrder& order, const Instrument& instrument, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double filled_net = compute_net(instrument, filled_qty, order.side);

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            pos_data->net_vega.add(key, filled_net);
        }
    }

    void on_state_change(const engine::TrackedOrder& order, const Instrument& instrument,
                         engine::OrderState old_state,
                         engine::OrderState new_state) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        auto old_stage = aggregation::stage_from_order_state(old_state);
        auto new_stage = aggregation::stage_from_order_state(new_state);

        if (old_stage != new_stage && aggregation::is_active_order_state(new_state)) {
            Key key = extract_order_key(order);
            double net = compute_net(instrument, order.leaves_qty, order.side);

            auto* old_data = storage_.get_stage(old_stage);
            auto* new_data = storage_.get_stage(new_stage);

            if (old_data) {
                old_data->net_vega.remove(key, net);
            }
            if (new_data) {
                new_data->net_vega.add(key, net);
            }
        }
    }

    void on_order_updated_with_state_change(const engine::TrackedOrder& order, const Instrument& instrument,
                                             int64_t old_qty,
                                             engine::OrderState old_state,
                                             engine::OrderState new_state) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double old_net = compute_net(instrument, old_qty, order.side);
        double new_net = compute_net(instrument, order.leaves_qty, order.side);

        auto old_stage = aggregation::stage_from_order_state(old_state);
        auto new_stage = aggregation::stage_from_order_state(new_state);

        auto* old_data = storage_.get_stage(old_stage);
        auto* new_data = storage_.get_stage(new_stage);

        if (old_data) {
            old_data->net_vega.remove(key, old_net);
        }
        if (new_data) {
            new_data->net_vega.add(key, new_net);
        }
    }

    void clear() {
        storage_.clear();
    }
};

// ============================================================================
// Type aliases for Gross/Net vega metrics
// ============================================================================

template<typename Instrument, typename... Stages>
using GlobalGrossVegaMetric = GrossVegaMetric<aggregation::GlobalKey, Instrument, Stages...>;

template<typename Instrument, typename... Stages>
using UnderlyerGrossVegaMetric = GrossVegaMetric<aggregation::UnderlyerKey, Instrument, Stages...>;

template<typename Instrument, typename... Stages>
using GlobalNetVegaMetric = NetVegaMetric<aggregation::GlobalKey, Instrument, Stages...>;

template<typename Instrument, typename... Stages>
using UnderlyerNetVegaMetric = NetVegaMetric<aggregation::UnderlyerKey, Instrument, Stages...>;

} // namespace metrics

// Include TrackedOrder definition for complete type info
#include "../engine/order_state.hpp"
