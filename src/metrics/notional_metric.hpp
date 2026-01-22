#pragma once

#include "../aggregation/staged_metric.hpp"
#include "../aggregation/aggregation_core.hpp"
#include "../aggregation/key_extractors.hpp"
#include "../fix/fix_types.hpp"
#include "../instrument/instrument.hpp"
#include <cmath>
#include <unordered_map>

// Forward declarations
namespace engine {
    struct TrackedOrder;
    enum class OrderState;
    enum class LimitType;
}

namespace metrics {

// ============================================================================
// NotionalMetric - Single-purpose notional tracking at a specific grouping level
// ============================================================================
//
// A generic notional metric that tracks notional at a single grouping level.
// The key type determines the grouping:
//   - GlobalKey: track global notional across all orders
//   - StrategyKey: track notional per strategy
//   - PortfolioKey: track notional per portfolio
//
// Template parameters:
//   Key: The grouping key type (GlobalKey, StrategyKey, or PortfolioKey)
//   Provider: Must satisfy the notional provider requirements
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//
// Notional is computed as: quantity * contract_size * spot_price * fx_rate
//

template<typename Key, typename Provider, typename... Stages>
class NotionalMetric {
    static_assert(instrument::is_notional_provider_v<Provider>,
                  "Provider must satisfy notional provider requirements (spot, fx, contract_size)");

public:
    using key_type = Key;
    using value_type = double;
    using provider_type = Provider;
    using Config = aggregation::StageConfig<Stages...>;

    // ========================================================================
    // Static methods for pre-trade limit checking
    // ========================================================================

    // Compute the notional contribution for a new order
    static double compute_order_contribution(
        const fix::NewOrderSingle& order,
        const Provider* provider) {
        if (!provider) return 0.0;
        return instrument::compute_notional(*provider, order.symbol, order.quantity);
    }

    // Compute the notional contribution for an order update (new - old)
    static double compute_update_contribution(
        const fix::OrderCancelReplaceRequest& update,
        const engine::TrackedOrder& existing_order,
        const Provider* provider) {
        if (!provider) return 0.0;
        double old_notional = instrument::compute_notional(
            *provider, existing_order.symbol, existing_order.leaves_qty);
        double new_notional = instrument::compute_notional(
            *provider, update.symbol, update.quantity);
        return new_notional - old_notional;
    }

    // Extract the key from a NewOrderSingle
    static Key extract_key(const fix::NewOrderSingle& order) {
        if constexpr (std::is_same_v<Key, aggregation::GlobalKey>) {
            return aggregation::GlobalKey::instance();
        } else if constexpr (std::is_same_v<Key, aggregation::StrategyKey>) {
            return Key{order.strategy_id};
        } else if constexpr (std::is_same_v<Key, aggregation::PortfolioKey>) {
            return Key{order.portfolio_id};
        } else if constexpr (std::is_same_v<Key, aggregation::UnderlyerKey>) {
            return Key{order.underlyer};
        } else if constexpr (std::is_same_v<Key, aggregation::InstrumentKey>) {
            return Key{order.symbol};
        } else {
            static_assert(sizeof(Key) == 0, "Unsupported key type for NotionalMetric");
        }
    }

    // Get the limit type for this metric
    static constexpr engine::LimitType limit_type() {
        if constexpr (std::is_same_v<Key, aggregation::GlobalKey>) {
            return engine::LimitType::GLOBAL_NOTIONAL;
        } else if constexpr (std::is_same_v<Key, aggregation::StrategyKey>) {
            return engine::LimitType::STRATEGY_NOTIONAL;
        } else if constexpr (std::is_same_v<Key, aggregation::PortfolioKey>) {
            return engine::LimitType::PORTFOLIO_NOTIONAL;
        } else {
            return engine::LimitType::GLOBAL_NOTIONAL;  // Default fallback
        }
    }

private:
    // Per-stage data: maps key -> notional value, plus instrument quantities for recomputation
    struct StageData {
        aggregation::AggregationBucket<Key, aggregation::SumCombiner<double>> notional;
        // Track quantities per instrument per key for proper removal
        std::unordered_map<std::string, std::unordered_map<std::string, int64_t>> quantities;

        double get(const Key& key) const {
            return notional.get(key);
        }

        void clear() {
            notional.clear();
            quantities.clear();
        }
    };

    using Storage = aggregation::StagedMetric<StageData, Stages...>;

    const Provider* provider_ = nullptr;
    Storage storage_;

    // Helper to get the key string for quantities map
    static std::string key_to_string(const Key& key) {
        if constexpr (std::is_same_v<Key, aggregation::GlobalKey>) {
            return "__global__";
        } else if constexpr (std::is_same_v<Key, aggregation::StrategyKey>) {
            return key.strategy_id;
        } else if constexpr (std::is_same_v<Key, aggregation::PortfolioKey>) {
            return key.portfolio_id;
        } else {
            return "";
        }
    }

public:
    // ========================================================================
    // Configuration info
    // ========================================================================

    static constexpr bool tracks_position = Config::track_position;
    static constexpr bool tracks_open = Config::track_open;
    static constexpr bool tracks_in_flight = Config::track_in_flight;

    // ========================================================================
    // InstrumentProvider interface
    // ========================================================================

    void set_instrument_provider(const Provider* provider) {
        provider_ = provider;
    }

    const Provider* instrument_provider() const {
        return provider_;
    }

    // ========================================================================
    // Accessors
    // ========================================================================

    // Get notional for a key (combined open + in-flight, excluding position)
    double get(const Key& key) const {
        double total = 0.0;
        if constexpr (Config::track_open) {
            total += storage_.open().get(key);
        }
        if constexpr (Config::track_in_flight) {
            total += storage_.in_flight().get(key);
        }
        return total;
    }

    // Get notional including all tracked stages
    double get_total(const Key& key) const {
        double total = 0.0;
        if constexpr (Config::track_position) {
            total += storage_.position().get(key);
        }
        if constexpr (Config::track_open) {
            total += storage_.open().get(key);
        }
        if constexpr (Config::track_in_flight) {
            total += storage_.in_flight().get(key);
        }
        return total;
    }

    // Per-stage accessors
    template<typename Dummy = void>
    std::enable_if_t<Config::track_position && std::is_void_v<Dummy>, double>
    get_position(const Key& key) const {
        return storage_.position().get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Config::track_open && std::is_void_v<Dummy>, double>
    get_open(const Key& key) const {
        return storage_.open().get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Config::track_in_flight && std::is_void_v<Dummy>, double>
    get_in_flight(const Key& key) const {
        return storage_.in_flight().get(key);
    }

    // ========================================================================
    // Generic metric interface
    // ========================================================================

    void on_order_added(const engine::TrackedOrder& order);
    void on_order_removed(const engine::TrackedOrder& order);
    void on_order_updated(const engine::TrackedOrder& order, int64_t old_qty);
    void on_partial_fill(const engine::TrackedOrder& order, int64_t filled_qty);
    void on_full_fill(const engine::TrackedOrder& order, int64_t filled_qty);
    void on_state_change(const engine::TrackedOrder& order,
                         engine::OrderState old_state,
                         engine::OrderState new_state);
    void on_order_updated_with_state_change(const engine::TrackedOrder& order,
                                             int64_t old_qty,
                                             engine::OrderState old_state,
                                             engine::OrderState new_state);

    void clear() {
        storage_.clear();
    }

private:
    // Extract key from order based on Key type
    Key extract_key(const engine::TrackedOrder& order) const;

    // Check if this key type is applicable for the order
    bool is_applicable(const engine::TrackedOrder& order) const;

    // Compute notional for the order
    double compute_notional(const engine::TrackedOrder& order, int64_t quantity) const {
        if (!provider_) return 0.0;
        return instrument::compute_notional(*provider_, order.symbol, quantity);
    }
};

// ============================================================================
// GlobalNotionalMetric - Convenience alias for global notional tracking
// ============================================================================

template<typename Provider, typename... Stages>
using GlobalNotionalMetric = NotionalMetric<aggregation::GlobalKey, Provider, Stages...>;

// ============================================================================
// StrategyNotionalMetric - Convenience alias for per-strategy notional tracking
// ============================================================================

template<typename Provider, typename... Stages>
using StrategyNotionalMetric = NotionalMetric<aggregation::StrategyKey, Provider, Stages...>;

// ============================================================================
// PortfolioNotionalMetric - Convenience alias for per-portfolio notional tracking
// ============================================================================

template<typename Provider, typename... Stages>
using PortfolioNotionalMetric = NotionalMetric<aggregation::PortfolioKey, Provider, Stages...>;

} // namespace metrics

// Include TrackedOrder definition and implement generic interface
#include "../engine/order_state.hpp"

namespace metrics {

// Key extraction specializations
template<typename Key, typename Provider, typename... Stages>
Key NotionalMetric<Key, Provider, Stages...>::extract_key(const engine::TrackedOrder& order) const {
    if constexpr (std::is_same_v<Key, aggregation::GlobalKey>) {
        (void)order;
        return aggregation::GlobalKey::instance();
    } else if constexpr (std::is_same_v<Key, aggregation::StrategyKey>) {
        return aggregation::StrategyKey{order.strategy_id};
    } else if constexpr (std::is_same_v<Key, aggregation::PortfolioKey>) {
        return aggregation::PortfolioKey{order.portfolio_id};
    } else {
        return aggregation::KeyExtractor<Key>::extract(order);
    }
}

template<typename Key, typename Provider, typename... Stages>
bool NotionalMetric<Key, Provider, Stages...>::is_applicable(const engine::TrackedOrder& order) const {
    if constexpr (std::is_same_v<Key, aggregation::GlobalKey>) {
        (void)order;
        return true;
    } else if constexpr (std::is_same_v<Key, aggregation::StrategyKey>) {
        return !order.strategy_id.empty();
    } else if constexpr (std::is_same_v<Key, aggregation::PortfolioKey>) {
        return !order.portfolio_id.empty();
    } else {
        return aggregation::KeyExtractor<Key>::is_applicable(order);
    }
}

template<typename Key, typename Provider, typename... Stages>
void NotionalMetric<Key, Provider, Stages...>::on_order_added(const engine::TrackedOrder& order) {
    if (!is_applicable(order)) return;

    if constexpr (Config::track_in_flight) {
        Key key = extract_key(order);
        double notional = compute_notional(order, order.leaves_qty);
        storage_.in_flight().notional.add(key, notional);
    }
}

template<typename Key, typename Provider, typename... Stages>
void NotionalMetric<Key, Provider, Stages...>::on_order_removed(const engine::TrackedOrder& order) {
    if (!is_applicable(order)) return;

    Key key = extract_key(order);
    auto stage = aggregation::stage_from_order_state(order.state);
    auto* stage_data = storage_.get_stage(stage);
    if (stage_data) {
        double notional = compute_notional(order, order.leaves_qty);
        stage_data->notional.remove(key, notional);
    }
}

template<typename Key, typename Provider, typename... Stages>
void NotionalMetric<Key, Provider, Stages...>::on_order_updated(const engine::TrackedOrder& order, int64_t old_qty) {
    if (!is_applicable(order)) return;

    Key key = extract_key(order);
    auto stage = aggregation::stage_from_order_state(order.state);
    auto* stage_data = storage_.get_stage(stage);
    if (stage_data) {
        double old_notional = compute_notional(order, old_qty);
        double new_notional = compute_notional(order, order.leaves_qty);
        stage_data->notional.remove(key, old_notional);
        stage_data->notional.add(key, new_notional);
    }
}

template<typename Key, typename Provider, typename... Stages>
void NotionalMetric<Key, Provider, Stages...>::on_partial_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
    if (!is_applicable(order)) return;

    Key key = extract_key(order);
    double fill_notional = compute_notional(order, filled_qty);

    // Remove from open stage
    if constexpr (Config::track_open) {
        storage_.open().notional.remove(key, fill_notional);
    }
    // Add to position stage
    if constexpr (Config::track_position) {
        storage_.position().notional.add(key, fill_notional);
    }
}

template<typename Key, typename Provider, typename... Stages>
void NotionalMetric<Key, Provider, Stages...>::on_full_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
    if (!is_applicable(order)) return;

    Key key = extract_key(order);
    double fill_notional = compute_notional(order, filled_qty);

    // Add to position stage (removal from open/in_flight handled by on_order_removed)
    if constexpr (Config::track_position) {
        storage_.position().notional.add(key, fill_notional);
    }
}

template<typename Key, typename Provider, typename... Stages>
void NotionalMetric<Key, Provider, Stages...>::on_state_change(const engine::TrackedOrder& order,
                                                                engine::OrderState old_state,
                                                                engine::OrderState new_state) {
    if (!is_applicable(order)) return;

    auto old_stage = aggregation::stage_from_order_state(old_state);
    auto new_stage = aggregation::stage_from_order_state(new_state);

    if (old_stage != new_stage && aggregation::is_active_order_state(new_state)) {
        Key key = extract_key(order);
        double notional = compute_notional(order, order.leaves_qty);

        auto* old_stage_data = storage_.get_stage(old_stage);
        auto* new_stage_data = storage_.get_stage(new_stage);

        if (old_stage_data) {
            old_stage_data->notional.remove(key, notional);
        }
        if (new_stage_data) {
            new_stage_data->notional.add(key, notional);
        }
    }
}

template<typename Key, typename Provider, typename... Stages>
void NotionalMetric<Key, Provider, Stages...>::on_order_updated_with_state_change(
    const engine::TrackedOrder& order,
    int64_t old_qty,
    engine::OrderState old_state,
    engine::OrderState new_state) {

    if (!is_applicable(order)) return;

    auto old_stage = aggregation::stage_from_order_state(old_state);
    auto new_stage = aggregation::stage_from_order_state(new_state);

    Key key = extract_key(order);
    double old_notional = compute_notional(order, old_qty);
    double new_notional = compute_notional(order, order.leaves_qty);

    auto* old_stage_data = storage_.get_stage(old_stage);
    auto* new_stage_data = storage_.get_stage(new_stage);

    if (old_stage_data) {
        old_stage_data->notional.remove(key, old_notional);
    }
    if (new_stage_data) {
        new_stage_data->notional.add(key, new_notional);
    }
}

} // namespace metrics

// ============================================================================
// AccessorMixin specializations
// ============================================================================

#include "../engine/accessor_mixin.hpp"

namespace engine {

// Generic NotionalMetric accessor mixin
template<typename Derived, typename Key, typename Provider, typename... Stages>
class AccessorMixin<Derived, metrics::NotionalMetric<Key, Provider, Stages...>> {
protected:
    const metrics::NotionalMetric<Key, Provider, Stages...>& notional_metric_() const {
        return static_cast<const Derived*>(this)->template
            get_metric<metrics::NotionalMetric<Key, Provider, Stages...>>();
    }

public:
    double notional(const Key& key) const {
        return notional_metric_().get(key);
    }

    double notional_total(const Key& key) const {
        return notional_metric_().get_total(key);
    }
};

// Specialization for GlobalKey with convenience methods
template<typename Derived, typename Provider, typename... Stages>
class AccessorMixin<Derived, metrics::NotionalMetric<aggregation::GlobalKey, Provider, Stages...>> {
protected:
    const metrics::NotionalMetric<aggregation::GlobalKey, Provider, Stages...>&
    global_notional_metric_() const {
        return static_cast<const Derived*>(this)->template
            get_metric<metrics::NotionalMetric<aggregation::GlobalKey, Provider, Stages...>>();
    }

public:
    double global_notional() const {
        return global_notional_metric_().get(aggregation::GlobalKey::instance());
    }

    double total_global_notional() const {
        return global_notional_metric_().get_total(aggregation::GlobalKey::instance());
    }
};

// Specialization for StrategyKey with convenience methods
template<typename Derived, typename Provider, typename... Stages>
class AccessorMixin<Derived, metrics::NotionalMetric<aggregation::StrategyKey, Provider, Stages...>> {
protected:
    const metrics::NotionalMetric<aggregation::StrategyKey, Provider, Stages...>&
    strategy_notional_metric_() const {
        return static_cast<const Derived*>(this)->template
            get_metric<metrics::NotionalMetric<aggregation::StrategyKey, Provider, Stages...>>();
    }

public:
    double strategy_notional(const std::string& strategy_id) const {
        return strategy_notional_metric_().get(aggregation::StrategyKey{strategy_id});
    }

    double total_strategy_notional(const std::string& strategy_id) const {
        return strategy_notional_metric_().get_total(aggregation::StrategyKey{strategy_id});
    }
};

// Specialization for PortfolioKey with convenience methods
template<typename Derived, typename Provider, typename... Stages>
class AccessorMixin<Derived, metrics::NotionalMetric<aggregation::PortfolioKey, Provider, Stages...>> {
protected:
    const metrics::NotionalMetric<aggregation::PortfolioKey, Provider, Stages...>&
    portfolio_notional_metric_() const {
        return static_cast<const Derived*>(this)->template
            get_metric<metrics::NotionalMetric<aggregation::PortfolioKey, Provider, Stages...>>();
    }

public:
    double portfolio_notional(const std::string& portfolio_id) const {
        return portfolio_notional_metric_().get(aggregation::PortfolioKey{portfolio_id});
    }

    double total_portfolio_notional(const std::string& portfolio_id) const {
        return portfolio_notional_metric_().get_total(aggregation::PortfolioKey{portfolio_id});
    }
};

} // namespace engine
