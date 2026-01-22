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
//   Provider: Must satisfy the vega provider requirements (vega support)
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//

template<typename Key, typename Provider, typename... Stages>
class GrossVegaMetric {
    static_assert(instrument::is_vega_provider_v<Provider>,
                  "Provider must satisfy vega provider requirements (vega support)");

public:
    using key_type = Key;
    using value_type = double;
    using provider_type = Provider;
    using Config = aggregation::StageConfig<Stages...>;

    // ========================================================================
    // Static methods for pre-trade limit checking
    // ========================================================================

    // Compute the gross vega contribution for a new order
    static double compute_order_contribution(
        const fix::NewOrderSingle& order,
        const Provider* provider) {
        if (!provider) return 0.0;
        double vega_exp = instrument::compute_vega_exposure(*provider, order.symbol, order.quantity);
        return std::abs(vega_exp);
    }

    // Compute the gross vega contribution for an order update (new - old)
    static double compute_update_contribution(
        const fix::OrderCancelReplaceRequest& update,
        const engine::TrackedOrder& existing_order,
        const Provider* provider) {
        if (!provider) return 0.0;
        double old_vega = std::abs(instrument::compute_vega_exposure(
            *provider, existing_order.symbol, existing_order.leaves_qty));
        double new_vega = std::abs(instrument::compute_vega_exposure(
            *provider, update.symbol, update.quantity));
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

    const Provider* provider_ = nullptr;
    Storage storage_;

    double compute_gross(const engine::TrackedOrder& order) const {
        if (!provider_) return 0.0;
        double vega_exp = instrument::compute_vega_exposure(*provider_, order.symbol, order.leaves_qty);
        return std::abs(vega_exp);
    }

    double compute_gross(const std::string& symbol, int64_t quantity) const {
        if (!provider_) return 0.0;
        double vega_exp = instrument::compute_vega_exposure(*provider_, symbol, quantity);
        return std::abs(vega_exp);
    }

    Key extract_order_key(const engine::TrackedOrder& order) const {
        return aggregation::KeyExtractor<Key>::extract(order);
    }

public:
    GrossVegaMetric() = default;

    void set_instrument_provider(const Provider* provider) {
        provider_ = provider;
    }

    const Provider* instrument_provider() const {
        return provider_;
    }

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

    void on_order_added(const engine::TrackedOrder& order) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double gross = compute_gross(order);
        auto* stage_data = storage_.get_stage(aggregation::OrderStage::IN_FLIGHT);
        if (stage_data) {
            stage_data->gross_vega.add(key, gross);
        }
    }

    void on_order_removed(const engine::TrackedOrder& order) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double gross = compute_gross(order);
        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (stage_data) {
            stage_data->gross_vega.remove(key, gross);
        }
    }

    void on_order_updated(const engine::TrackedOrder& order, int64_t old_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double old_gross = compute_gross(order.symbol, old_qty);
        double new_gross = compute_gross(order);
        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (stage_data) {
            stage_data->gross_vega.remove(key, old_gross);
            stage_data->gross_vega.add(key, new_gross);
        }
    }

    void on_partial_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double filled_gross = compute_gross(order.symbol, filled_qty);

        auto* open_data = storage_.get_stage(aggregation::OrderStage::OPEN);
        if (open_data) {
            open_data->gross_vega.remove(key, filled_gross);
        }

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            pos_data->gross_vega.add(key, filled_gross);
        }
    }

    void on_full_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double filled_gross = compute_gross(order.symbol, filled_qty);

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            pos_data->gross_vega.add(key, filled_gross);
        }
    }

    void on_state_change(const engine::TrackedOrder& order,
                         engine::OrderState old_state,
                         engine::OrderState new_state) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        auto old_stage = aggregation::stage_from_order_state(old_state);
        auto new_stage = aggregation::stage_from_order_state(new_state);

        if (old_stage != new_stage && aggregation::is_active_order_state(new_state)) {
            Key key = extract_order_key(order);
            double gross = compute_gross(order);

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

    void on_order_updated_with_state_change(const engine::TrackedOrder& order,
                                             int64_t old_qty,
                                             engine::OrderState old_state,
                                             engine::OrderState new_state) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double old_gross = compute_gross(order.symbol, old_qty);
        double new_gross = compute_gross(order);

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

template<typename Key, typename Provider, typename... Stages>
class NetVegaMetric {
    static_assert(instrument::is_vega_provider_v<Provider>,
                  "Provider must satisfy vega provider requirements (vega support)");

public:
    using key_type = Key;
    using value_type = double;
    using provider_type = Provider;
    using Config = aggregation::StageConfig<Stages...>;

    // ========================================================================
    // Static methods for pre-trade limit checking
    // ========================================================================

    // Compute the net vega contribution for a new order
    static double compute_order_contribution(
        const fix::NewOrderSingle& order,
        const Provider* provider) {
        if (!provider) return 0.0;
        double vega_exp = instrument::compute_vega_exposure(*provider, order.symbol, order.quantity);
        return (order.side == fix::Side::BID) ? vega_exp : -vega_exp;
    }

    // Compute the net vega contribution for an order update (new - old)
    static double compute_update_contribution(
        const fix::OrderCancelReplaceRequest& update,
        const engine::TrackedOrder& existing_order,
        const Provider* provider) {
        if (!provider) return 0.0;
        double old_vega = instrument::compute_vega_exposure(
            *provider, existing_order.symbol, existing_order.leaves_qty);
        double old_net = (existing_order.side == fix::Side::BID) ? old_vega : -old_vega;

        double new_vega = instrument::compute_vega_exposure(
            *provider, update.symbol, update.quantity);
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

    const Provider* provider_ = nullptr;
    Storage storage_;

    double compute_net(const engine::TrackedOrder& order) const {
        if (!provider_) return 0.0;
        double vega_exp = instrument::compute_vega_exposure(*provider_, order.symbol, order.leaves_qty);
        return (order.side == fix::Side::BID) ? vega_exp : -vega_exp;
    }

    double compute_net(const std::string& symbol, int64_t quantity, fix::Side side) const {
        if (!provider_) return 0.0;
        double vega_exp = instrument::compute_vega_exposure(*provider_, symbol, quantity);
        return (side == fix::Side::BID) ? vega_exp : -vega_exp;
    }

    Key extract_order_key(const engine::TrackedOrder& order) const {
        return aggregation::KeyExtractor<Key>::extract(order);
    }

public:
    NetVegaMetric() = default;

    void set_instrument_provider(const Provider* provider) {
        provider_ = provider;
    }

    const Provider* instrument_provider() const {
        return provider_;
    }

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

    void on_order_added(const engine::TrackedOrder& order) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double net = compute_net(order);
        auto* stage_data = storage_.get_stage(aggregation::OrderStage::IN_FLIGHT);
        if (stage_data) {
            stage_data->net_vega.add(key, net);
        }
    }

    void on_order_removed(const engine::TrackedOrder& order) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double net = compute_net(order);
        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (stage_data) {
            stage_data->net_vega.remove(key, net);
        }
    }

    void on_order_updated(const engine::TrackedOrder& order, int64_t old_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double old_net = compute_net(order.symbol, old_qty, order.side);
        double new_net = compute_net(order);
        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (stage_data) {
            stage_data->net_vega.remove(key, old_net);
            stage_data->net_vega.add(key, new_net);
        }
    }

    void on_partial_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double filled_net = compute_net(order.symbol, filled_qty, order.side);

        auto* open_data = storage_.get_stage(aggregation::OrderStage::OPEN);
        if (open_data) {
            open_data->net_vega.remove(key, filled_net);
        }

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            pos_data->net_vega.add(key, filled_net);
        }
    }

    void on_full_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double filled_net = compute_net(order.symbol, filled_qty, order.side);

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            pos_data->net_vega.add(key, filled_net);
        }
    }

    void on_state_change(const engine::TrackedOrder& order,
                         engine::OrderState old_state,
                         engine::OrderState new_state) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        auto old_stage = aggregation::stage_from_order_state(old_state);
        auto new_stage = aggregation::stage_from_order_state(new_state);

        if (old_stage != new_stage && aggregation::is_active_order_state(new_state)) {
            Key key = extract_order_key(order);
            double net = compute_net(order);

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

    void on_order_updated_with_state_change(const engine::TrackedOrder& order,
                                             int64_t old_qty,
                                             engine::OrderState old_state,
                                             engine::OrderState new_state) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double old_net = compute_net(order.symbol, old_qty, order.side);
        double new_net = compute_net(order);

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

template<typename Provider, typename... Stages>
using GlobalGrossVegaMetric = GrossVegaMetric<aggregation::GlobalKey, Provider, Stages...>;

template<typename Provider, typename... Stages>
using UnderlyerGrossVegaMetric = GrossVegaMetric<aggregation::UnderlyerKey, Provider, Stages...>;

template<typename Provider, typename... Stages>
using GlobalNetVegaMetric = NetVegaMetric<aggregation::GlobalKey, Provider, Stages...>;

template<typename Provider, typename... Stages>
using UnderlyerNetVegaMetric = NetVegaMetric<aggregation::UnderlyerKey, Provider, Stages...>;

} // namespace metrics

// Include TrackedOrder definition for complete type info
#include "../engine/order_state.hpp"
