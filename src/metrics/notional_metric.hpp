#pragma once

#include "base_exposure_metric.hpp"
#include "metric_policies.hpp"
#include "../aggregation/staged_metric.hpp"
#include "../aggregation/aggregation_core.hpp"
#include "../aggregation/key_extractors.hpp"
#include "../fix/fix_types.hpp"
#include "../instrument/instrument.hpp"
#include "../engine/pre_trade_check.hpp"
#include <cmath>
#include <unordered_map>

// Forward declarations
namespace engine {
    struct TrackedOrder;
    enum class OrderState;
}

namespace metrics {

// ============================================================================
// StoredNotionalInputs - Computation inputs for drift-free notional tracking
// ============================================================================
//
// Used by NotionalMetric for its unique get()/get_total() interface.
// GrossNotionalMetric and NetNotionalMetric use NotionalInputPolicy instead.
//
struct StoredNotionalInputs {
    int64_t quantity;
    double contract_size;
    double spot_price;
    double fx_rate;

    double compute() const {
        return static_cast<double>(quantity) * contract_size * spot_price * fx_rate;
    }

    StoredNotionalInputs with_quantity(int64_t new_qty) const {
        return StoredNotionalInputs{new_qty, contract_size, spot_price, fx_rate};
    }
};

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
// NOTE: This class provides a different interface than GrossNotionalMetric/NetNotionalMetric:
//   - get(): returns open + in-flight (excludes position)
//   - get_total(): returns position + open + in-flight
//
// Template parameters:
//   Key: The grouping key type (GlobalKey, StrategyKey, or PortfolioKey)
//   Context: Type providing accessor methods for instrument properties
//   Instrument: Must satisfy the notional instrument requirements
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//
// Notional is computed as: quantity * contract_size * spot_price * fx_rate
//

template<typename Key, typename Context, typename Instrument, typename... Stages>
class NotionalMetric {
    static_assert(instrument::is_notional_instrument_v<Instrument>,
                  "Instrument must satisfy notional instrument requirements (spot, fx, contract_size)");

public:
    using key_type = Key;
    using value_type = double;
    using context_type = Context;
    using instrument_type = Instrument;
    using Config = aggregation::StageConfig<Stages...>;

    // ========================================================================
    // Static methods for pre-trade limit checking
    // ========================================================================

    template<typename Ctx, typename Inst>
    static double compute_order_contribution(const fix::NewOrderSingle& order, const Inst& instrument, const Ctx& context) {
        return instrument::compute_notional(context, instrument, order.quantity);
    }

    template<typename Ctx, typename Inst>
    static double compute_update_contribution(
        const fix::OrderCancelReplaceRequest& update,
        const engine::TrackedOrder& existing_order,
        const Inst& instrument,
        const Ctx& context) {
        double old_notional = instrument::compute_notional(context, instrument, existing_order.leaves_qty);
        double new_notional = instrument::compute_notional(context, instrument, update.quantity);
        return new_notional - old_notional;
    }

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

    static constexpr engine::LimitType limit_type() {
        if constexpr (std::is_same_v<Key, aggregation::GlobalKey>) {
            return engine::LimitType::GLOBAL_NOTIONAL;
        } else if constexpr (std::is_same_v<Key, aggregation::StrategyKey>) {
            return engine::LimitType::STRATEGY_NOTIONAL;
        } else if constexpr (std::is_same_v<Key, aggregation::PortfolioKey>) {
            return engine::LimitType::PORTFOLIO_NOTIONAL;
        } else {
            return engine::LimitType::GLOBAL_NOTIONAL;
        }
    }

private:
    struct StageData {
        aggregation::AggregationBucket<Key, aggregation::SumCombiner<double>> notional;
        std::unordered_map<std::string, std::pair<Key, StoredNotionalInputs>> order_inputs;

        double get(const Key& key) const {
            return notional.get(key);
        }

        void clear() {
            notional.clear();
            order_inputs.clear();
        }
    };

    using Storage = aggregation::StagedMetric<StageData, Stages...>;

    Storage storage_;

    template<typename Ctx, typename Inst>
    StoredNotionalInputs capture_inputs(const Ctx& context, const Inst& instrument, int64_t quantity) const {
        return StoredNotionalInputs{
            quantity,
            context.contract_size(instrument),
            context.spot_price(instrument),
            context.fx_rate(instrument)
        };
    }

public:
    static constexpr bool tracks_position = Config::track_position;
    static constexpr bool tracks_open = Config::track_open;
    static constexpr bool tracks_in_flight = Config::track_in_flight;

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

    void on_order_added(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context);
    void on_order_removed(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context);
    void on_order_updated(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t old_qty);
    void on_partial_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty);
    void on_full_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty);
    void on_state_change(const engine::TrackedOrder& order,
                         const Instrument& instrument,
                         const Context& context,
                         engine::OrderState old_state,
                         engine::OrderState new_state);
    void on_order_updated_with_state_change(const engine::TrackedOrder& order,
                                             const Instrument& instrument,
                                             const Context& context,
                                             int64_t old_qty,
                                             engine::OrderState old_state,
                                             engine::OrderState new_state);

    void clear() {
        storage_.clear();
    }

private:
    Key extract_order_key(const engine::TrackedOrder& order) const;
    bool is_applicable(const engine::TrackedOrder& order) const;

    double compute_notional(const Instrument& instrument, const Context& context, int64_t quantity) const {
        return instrument::compute_notional(context, instrument, quantity);
    }
};

// ============================================================================
// GlobalNotionalMetric - Convenience alias for global notional tracking
// ============================================================================

template<typename Context, typename Instrument, typename... Stages>
using GlobalNotionalMetric = NotionalMetric<aggregation::GlobalKey, Context, Instrument, Stages...>;

// ============================================================================
// StrategyNotionalMetric - Convenience alias for per-strategy notional tracking
// ============================================================================

template<typename Context, typename Instrument, typename... Stages>
using StrategyNotionalMetric = NotionalMetric<aggregation::StrategyKey, Context, Instrument, Stages...>;

// ============================================================================
// PortfolioNotionalMetric - Convenience alias for per-portfolio notional tracking
// ============================================================================

template<typename Context, typename Instrument, typename... Stages>
using PortfolioNotionalMetric = NotionalMetric<aggregation::PortfolioKey, Context, Instrument, Stages...>;

// ============================================================================
// GrossNotionalMetric - Single-value metric tracking absolute notional
// ============================================================================
//
// Tracks only gross (absolute) notional. Designed for use with the
// generic limit checking system where each metric has one value type.
// BID and ASK both add positive values (sum of |notional|).
//

template<typename Key, typename Context, typename Instrument, typename... Stages>
using GrossNotionalMetric = BaseExposureMetric<
    Key, Context, Instrument,
    NotionalInputPolicy<Context, Instrument>,
    GrossValuePolicy,
    engine::LimitType::GLOBAL_GROSS_NOTIONAL,
    Stages...
>;

// ============================================================================
// NetNotionalMetric - Single-value metric tracking signed notional
// ============================================================================
//
// Tracks only net (signed) notional. Designed for use with the
// generic limit checking system where each metric has one value type.
// BID = +notional, ASK = -notional
//

template<typename Key, typename Context, typename Instrument, typename... Stages>
using NetNotionalMetric = BaseExposureMetric<
    Key, Context, Instrument,
    NotionalInputPolicy<Context, Instrument>,
    NetValuePolicy,
    engine::LimitType::GLOBAL_NET_NOTIONAL,
    Stages...
>;

// ============================================================================
// Type aliases for Gross/Net notional metrics
// ============================================================================

template<typename Context, typename Instrument, typename... Stages>
using GlobalGrossNotionalMetric = GrossNotionalMetric<aggregation::GlobalKey, Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using GlobalNetNotionalMetric = NetNotionalMetric<aggregation::GlobalKey, Context, Instrument, Stages...>;

} // namespace metrics

// Include TrackedOrder definition and implement generic interface
#include "../engine/order_state.hpp"

namespace metrics {

// Key extraction specializations
template<typename Key, typename Context, typename Instrument, typename... Stages>
Key NotionalMetric<Key, Context, Instrument, Stages...>::extract_order_key(const engine::TrackedOrder& order) const {
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

template<typename Key, typename Context, typename Instrument, typename... Stages>
bool NotionalMetric<Key, Context, Instrument, Stages...>::is_applicable(const engine::TrackedOrder& order) const {
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

template<typename Key, typename Context, typename Instrument, typename... Stages>
void NotionalMetric<Key, Context, Instrument, Stages...>::on_order_added(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context) {
    if (!is_applicable(order)) return;

    if constexpr (Config::track_in_flight) {
        Key key = extract_order_key(order);
        StoredNotionalInputs inputs = capture_inputs(context, instrument, order.leaves_qty);
        double notional = inputs.compute();
        storage_.in_flight().notional.add(key, notional);
        storage_.in_flight().order_inputs[order.key.cl_ord_id] = {key, inputs};
    }
}

template<typename Key, typename Context, typename Instrument, typename... Stages>
void NotionalMetric<Key, Context, Instrument, Stages...>::on_order_removed(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context) {
    if (!is_applicable(order)) return;
    (void)instrument;
    (void)context;

    auto stage = aggregation::stage_from_order_state(order.state);
    auto* stage_data = storage_.get_stage(stage);
    if (!stage_data) return;

    auto it = stage_data->order_inputs.find(order.key.cl_ord_id);
    if (it != stage_data->order_inputs.end()) {
        Key key = it->second.first;
        double notional = it->second.second.compute();
        stage_data->notional.remove(key, notional);
        stage_data->order_inputs.erase(it);
    }
}

template<typename Key, typename Context, typename Instrument, typename... Stages>
void NotionalMetric<Key, Context, Instrument, Stages...>::on_order_updated(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t old_qty) {
    if (!is_applicable(order)) return;

    auto stage = aggregation::stage_from_order_state(order.state);
    auto* stage_data = storage_.get_stage(stage);
    if (!stage_data) return;

    Key key = extract_order_key(order);

    auto it = stage_data->order_inputs.find(order.key.cl_ord_id);
    if (it != stage_data->order_inputs.end()) {
        double old_notional = it->second.second.compute();
        stage_data->notional.remove(key, old_notional);
        stage_data->order_inputs.erase(it);
    } else {
        double old_notional = compute_notional(instrument, context, old_qty);
        stage_data->notional.remove(key, old_notional);
    }

    StoredNotionalInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty);
    double new_notional = new_inputs.compute();
    stage_data->notional.add(key, new_notional);
    stage_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
}

template<typename Key, typename Context, typename Instrument, typename... Stages>
void NotionalMetric<Key, Context, Instrument, Stages...>::on_partial_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
    if (!is_applicable(order)) return;

    Key key = extract_order_key(order);

    if constexpr (Config::track_open) {
        auto* open_data = &storage_.open();
        auto it = open_data->order_inputs.find(order.key.cl_ord_id);
        if (it != open_data->order_inputs.end()) {
            StoredNotionalInputs& stored = it->second.second;
            StoredNotionalInputs filled_inputs = stored.with_quantity(filled_qty);
            double filled_notional = filled_inputs.compute();
            open_data->notional.remove(key, filled_notional);
            stored.quantity -= filled_qty;
        }
    }

    if constexpr (Config::track_position) {
        StoredNotionalInputs pos_inputs = capture_inputs(context, instrument, filled_qty);
        double pos_notional = pos_inputs.compute();
        storage_.position().notional.add(key, pos_notional);
    }
}

template<typename Key, typename Context, typename Instrument, typename... Stages>
void NotionalMetric<Key, Context, Instrument, Stages...>::on_full_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
    if (!is_applicable(order)) return;

    Key key = extract_order_key(order);

    if constexpr (Config::track_position) {
        StoredNotionalInputs pos_inputs = capture_inputs(context, instrument, filled_qty);
        double fill_notional = pos_inputs.compute();
        storage_.position().notional.add(key, fill_notional);
    }
}

template<typename Key, typename Context, typename Instrument, typename... Stages>
void NotionalMetric<Key, Context, Instrument, Stages...>::on_state_change(const engine::TrackedOrder& order,
                                                                const Instrument& instrument,
                                                                const Context& context,
                                                                engine::OrderState old_state,
                                                                engine::OrderState new_state) {
    if (!is_applicable(order)) return;

    auto old_stage = aggregation::stage_from_order_state(old_state);
    auto new_stage = aggregation::stage_from_order_state(new_state);

    if (old_stage == new_stage) return;
    if (!aggregation::is_active_order_state(new_state)) return;

    auto* old_stage_data = storage_.get_stage(old_stage);
    auto* new_stage_data = storage_.get_stage(new_stage);

    Key key = extract_order_key(order);

    if (old_stage_data) {
        auto it = old_stage_data->order_inputs.find(order.key.cl_ord_id);
        if (it != old_stage_data->order_inputs.end()) {
            double old_notional = it->second.second.compute();
            old_stage_data->notional.remove(key, old_notional);
            old_stage_data->order_inputs.erase(it);
        }
    }

    if (new_stage_data) {
        StoredNotionalInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty);
        double new_notional = new_inputs.compute();
        new_stage_data->notional.add(key, new_notional);
        new_stage_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
    }
}

template<typename Key, typename Context, typename Instrument, typename... Stages>
void NotionalMetric<Key, Context, Instrument, Stages...>::on_order_updated_with_state_change(
    const engine::TrackedOrder& order,
    const Instrument& instrument,
    const Context& context,
    int64_t old_qty,
    engine::OrderState old_state,
    engine::OrderState new_state) {

    if (!is_applicable(order)) return;

    auto old_stage = aggregation::stage_from_order_state(old_state);
    auto new_stage = aggregation::stage_from_order_state(new_state);

    auto* old_stage_data = storage_.get_stage(old_stage);
    auto* new_stage_data = storage_.get_stage(new_stage);

    Key key = extract_order_key(order);

    if (old_stage_data) {
        auto it = old_stage_data->order_inputs.find(order.key.cl_ord_id);
        if (it != old_stage_data->order_inputs.end()) {
            double old_notional = it->second.second.compute();
            old_stage_data->notional.remove(key, old_notional);
            old_stage_data->order_inputs.erase(it);
        } else {
            double old_notional = compute_notional(instrument, context, old_qty);
            old_stage_data->notional.remove(key, old_notional);
        }
    }

    if (new_stage_data) {
        StoredNotionalInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty);
        double new_notional = new_inputs.compute();
        new_stage_data->notional.add(key, new_notional);
        new_stage_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
    }
}

} // namespace metrics

// ============================================================================
// AccessorMixin specializations
// ============================================================================

#include "../engine/accessor_mixin.hpp"

namespace engine {

// Generic NotionalMetric accessor mixin
template<typename Derived, typename Key, typename Context, typename Instrument, typename... Stages>
class AccessorMixin<Derived, metrics::NotionalMetric<Key, Context, Instrument, Stages...>> {
protected:
    const metrics::NotionalMetric<Key, Context, Instrument, Stages...>& notional_metric_() const {
        return static_cast<const Derived*>(this)->template
            get_metric<metrics::NotionalMetric<Key, Context, Instrument, Stages...>>();
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
template<typename Derived, typename Context, typename Instrument, typename... Stages>
class AccessorMixin<Derived, metrics::NotionalMetric<aggregation::GlobalKey, Context, Instrument, Stages...>> {
protected:
    const metrics::NotionalMetric<aggregation::GlobalKey, Context, Instrument, Stages...>&
    global_notional_metric_() const {
        return static_cast<const Derived*>(this)->template
            get_metric<metrics::NotionalMetric<aggregation::GlobalKey, Context, Instrument, Stages...>>();
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
template<typename Derived, typename Context, typename Instrument, typename... Stages>
class AccessorMixin<Derived, metrics::NotionalMetric<aggregation::StrategyKey, Context, Instrument, Stages...>> {
protected:
    const metrics::NotionalMetric<aggregation::StrategyKey, Context, Instrument, Stages...>&
    strategy_notional_metric_() const {
        return static_cast<const Derived*>(this)->template
            get_metric<metrics::NotionalMetric<aggregation::StrategyKey, Context, Instrument, Stages...>>();
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
template<typename Derived, typename Context, typename Instrument, typename... Stages>
class AccessorMixin<Derived, metrics::NotionalMetric<aggregation::PortfolioKey, Context, Instrument, Stages...>> {
protected:
    const metrics::NotionalMetric<aggregation::PortfolioKey, Context, Instrument, Stages...>&
    portfolio_notional_metric_() const {
        return static_cast<const Derived*>(this)->template
            get_metric<metrics::NotionalMetric<aggregation::PortfolioKey, Context, Instrument, Stages...>>();
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
