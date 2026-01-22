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

    // Compute the notional contribution for a new order
    template<typename Ctx, typename Inst>
    static double compute_order_contribution(const fix::NewOrderSingle& order, const Inst& instrument, const Ctx& context) {
        return instrument::compute_notional(context, instrument, order.quantity);
    }

    // Compute the notional contribution for an order update (new - old)
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
    // Per-stage data: maps key -> notional value
    struct StageData {
        aggregation::AggregationBucket<Key, aggregation::SumCombiner<double>> notional;

        double get(const Key& key) const {
            return notional.get(key);
        }

        void clear() {
            notional.clear();
        }
    };

    using Storage = aggregation::StagedMetric<StageData, Stages...>;

    Storage storage_;

public:
    // ========================================================================
    // Configuration info
    // ========================================================================

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
    // Extract key from order based on Key type
    Key extract_order_key(const engine::TrackedOrder& order) const;

    // Check if this key type is applicable for the order
    bool is_applicable(const engine::TrackedOrder& order) const;

    // Compute notional for the order
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
class GrossNotionalMetric {
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

    // Compute the gross notional contribution for a new order
    template<typename Ctx, typename Inst>
    static double compute_order_contribution(const fix::NewOrderSingle& order, const Inst& instrument, const Ctx& context) {
        return std::abs(instrument::compute_notional(context, instrument, order.quantity));
    }

    // Compute the gross notional contribution for an order update (new - old)
    template<typename Ctx, typename Inst>
    static double compute_update_contribution(
        const fix::OrderCancelReplaceRequest& update,
        const engine::TrackedOrder& existing_order,
        const Inst& instrument,
        const Ctx& context) {
        double old_notional = std::abs(instrument::compute_notional(context, instrument, existing_order.leaves_qty));
        double new_notional = std::abs(instrument::compute_notional(context, instrument, update.quantity));
        return new_notional - old_notional;
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
            static_assert(sizeof(Key) == 0, "Unsupported key type for GrossNotionalMetric");
        }
    }

    // Get the limit type for this metric
    static constexpr engine::LimitType limit_type() {
        return engine::LimitType::GLOBAL_GROSS_NOTIONAL;
    }

private:
    struct StageData {
        aggregation::AggregationBucket<Key, aggregation::SumCombiner<double>> gross_notional;
        // Track quantities per instrument for position recomputation
        std::unordered_map<std::string, int64_t> instrument_quantities;

        void clear() {
            gross_notional.clear();
            instrument_quantities.clear();
        }
    };

    using Storage = aggregation::StagedMetric<StageData, Stages...>;

    Storage storage_;

    double compute_gross(const engine::TrackedOrder& order, const Instrument& inst, const Context& ctx) const {
        return std::abs(instrument::compute_notional(ctx, inst, order.leaves_qty));
    }

    double compute_gross(int64_t quantity, const Instrument& inst, const Context& ctx) const {
        return std::abs(instrument::compute_notional(ctx, inst, quantity));
    }

    Key extract_order_key(const engine::TrackedOrder& order) const {
        return aggregation::KeyExtractor<Key>::extract(order);
    }

public:
    GrossNotionalMetric() = default;

    // ========================================================================
    // Accessors
    // ========================================================================

    double get(const Key& key) const {
        double total = 0.0;
        storage_.for_each_stage([&key, &total](aggregation::OrderStage /*stage*/, const StageData& data) {
            total += data.gross_notional.get(key);
        });
        return total;
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_open && std::is_void_v<Dummy>, double>
    get_open(const Key& key) const {
        return storage_.open().gross_notional.get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_in_flight && std::is_void_v<Dummy>, double>
    get_in_flight(const Key& key) const {
        return storage_.in_flight().gross_notional.get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_position && std::is_void_v<Dummy>, double>
    get_position(const Key& key) const {
        return storage_.position().gross_notional.get(key);
    }

    // ========================================================================
    // Direct position manipulation (for testing/initialization)
    // ========================================================================

    // Set position for a specific instrument by quantity
    // Computes notional from instrument data: |qty * contract_size * spot_price * fx_rate|
    // Signed quantity is accepted (for engine-level interface compatibility) but absolute value is used
    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_position && std::is_void_v<Dummy>, void>
    set_instrument_position(const std::string& symbol, int64_t signed_quantity, const Instrument& instrument, const Context& context) {
        Key key = aggregation::GlobalKey::instance();
        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (!pos_data) return;

        // Remove old contribution if exists
        auto it = pos_data->instrument_quantities.find(symbol);
        if (it != pos_data->instrument_quantities.end()) {
            double old_gross = compute_gross(it->second, instrument, context);
            pos_data->gross_notional.remove(key, old_gross);
        }

        // Add new contribution (use absolute value for gross)
        int64_t abs_quantity = std::abs(signed_quantity);
        double new_gross = compute_gross(abs_quantity, instrument, context);
        pos_data->gross_notional.add(key, new_gross);
        pos_data->instrument_quantities[symbol] = abs_quantity;
    }

    // ========================================================================
    // Generic metric interface
    // ========================================================================

    void on_order_added(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double gross = compute_gross(order, instrument, context);
        auto* stage_data = storage_.get_stage(aggregation::OrderStage::IN_FLIGHT);
        if (stage_data) {
            stage_data->gross_notional.add(key, gross);
        }
    }

    void on_order_removed(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double gross = compute_gross(order, instrument, context);
        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (stage_data) {
            stage_data->gross_notional.remove(key, gross);
        }
    }

    void on_order_updated(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t old_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double old_gross = compute_gross(old_qty, instrument, context);
        double new_gross = compute_gross(order, instrument, context);
        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (stage_data) {
            stage_data->gross_notional.remove(key, old_gross);
            stage_data->gross_notional.add(key, new_gross);
        }
    }

    void on_partial_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double filled_gross = compute_gross(filled_qty, instrument, context);

        auto* open_data = storage_.get_stage(aggregation::OrderStage::OPEN);
        if (open_data) {
            open_data->gross_notional.remove(key, filled_gross);
        }

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            pos_data->gross_notional.add(key, filled_gross);
        }
    }

    void on_full_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double filled_gross = compute_gross(filled_qty, instrument, context);

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            pos_data->gross_notional.add(key, filled_gross);
        }
    }

    void on_state_change(const engine::TrackedOrder& order,
                         const Instrument& instrument,
                         const Context& context,
                         engine::OrderState old_state,
                         engine::OrderState new_state) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        auto old_stage = aggregation::stage_from_order_state(old_state);
        auto new_stage = aggregation::stage_from_order_state(new_state);

        if (old_stage != new_stage && aggregation::is_active_order_state(new_state)) {
            Key key = extract_order_key(order);
            double gross = compute_gross(order, instrument, context);

            auto* old_data = storage_.get_stage(old_stage);
            auto* new_data = storage_.get_stage(new_stage);

            if (old_data) {
                old_data->gross_notional.remove(key, gross);
            }
            if (new_data) {
                new_data->gross_notional.add(key, gross);
            }
        }
    }

    void on_order_updated_with_state_change(const engine::TrackedOrder& order,
                                             const Instrument& instrument,
                                             const Context& context,
                                             int64_t old_qty,
                                             engine::OrderState old_state,
                                             engine::OrderState new_state) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double old_gross = compute_gross(old_qty, instrument, context);
        double new_gross = compute_gross(order, instrument, context);

        auto old_stage = aggregation::stage_from_order_state(old_state);
        auto new_stage = aggregation::stage_from_order_state(new_state);

        auto* old_data = storage_.get_stage(old_stage);
        auto* new_data = storage_.get_stage(new_stage);

        if (old_data) {
            old_data->gross_notional.remove(key, old_gross);
        }
        if (new_data) {
            new_data->gross_notional.add(key, new_gross);
        }
    }

    void clear() {
        storage_.clear();
    }
};

// ============================================================================
// NetNotionalMetric - Single-value metric tracking signed notional
// ============================================================================
//
// Tracks only net (signed) notional. Designed for use with the
// generic limit checking system where each metric has one value type.
// BID = +notional, ASK = -notional
//

template<typename Key, typename Context, typename Instrument, typename... Stages>
class NetNotionalMetric {
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

    // Compute the net notional contribution for a new order
    template<typename Ctx, typename Inst>
    static double compute_order_contribution(const fix::NewOrderSingle& order, const Inst& instrument, const Ctx& context) {
        double notional = instrument::compute_notional(context, instrument, order.quantity);
        return (order.side == fix::Side::BID) ? notional : -notional;
    }

    // Compute the net notional contribution for an order update (new - old)
    template<typename Ctx, typename Inst>
    static double compute_update_contribution(
        const fix::OrderCancelReplaceRequest& update,
        const engine::TrackedOrder& existing_order,
        const Inst& instrument,
        const Ctx& context) {
        double old_notional = instrument::compute_notional(context, instrument, existing_order.leaves_qty);
        double old_net = (existing_order.side == fix::Side::BID) ? old_notional : -old_notional;

        double new_notional = instrument::compute_notional(context, instrument, update.quantity);
        double new_net = (update.side == fix::Side::BID) ? new_notional : -new_notional;

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
        } else if constexpr (std::is_same_v<Key, aggregation::PortfolioInstrumentKey>) {
            return Key{order.portfolio_id, order.symbol};
        } else {
            static_assert(sizeof(Key) == 0, "Unsupported key type for NetNotionalMetric");
        }
    }

    // Get the limit type for this metric
    static constexpr engine::LimitType limit_type() {
        return engine::LimitType::GLOBAL_NET_NOTIONAL;
    }

private:
    struct StageData {
        aggregation::AggregationBucket<Key, aggregation::SumCombiner<double>> net_notional;
        // Track signed quantities per instrument for position recomputation
        std::unordered_map<std::string, int64_t> instrument_quantities;

        void clear() {
            net_notional.clear();
            instrument_quantities.clear();
        }
    };

    using Storage = aggregation::StagedMetric<StageData, Stages...>;

    Storage storage_;

    double compute_net(const engine::TrackedOrder& order, const Instrument& inst, const Context& ctx) const {
        double notional = instrument::compute_notional(ctx, inst, order.leaves_qty);
        return (order.side == fix::Side::BID) ? notional : -notional;
    }

    double compute_net(int64_t quantity, fix::Side side, const Instrument& inst, const Context& ctx) const {
        double notional = instrument::compute_notional(ctx, inst, quantity);
        return (side == fix::Side::BID) ? notional : -notional;
    }

    // Compute net notional from signed quantity (positive = long, negative = short)
    double compute_net_from_signed_qty(int64_t signed_quantity, const Instrument& inst, const Context& ctx) const {
        double notional = instrument::compute_notional(ctx, inst, std::abs(signed_quantity));
        return (signed_quantity >= 0) ? notional : -notional;
    }

    Key extract_order_key(const engine::TrackedOrder& order) const {
        return aggregation::KeyExtractor<Key>::extract(order);
    }

public:
    NetNotionalMetric() = default;

    // ========================================================================
    // Accessors
    // ========================================================================

    double get(const Key& key) const {
        double total = 0.0;
        storage_.for_each_stage([&key, &total](aggregation::OrderStage /*stage*/, const StageData& data) {
            total += data.net_notional.get(key);
        });
        return total;
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_open && std::is_void_v<Dummy>, double>
    get_open(const Key& key) const {
        return storage_.open().net_notional.get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_in_flight && std::is_void_v<Dummy>, double>
    get_in_flight(const Key& key) const {
        return storage_.in_flight().net_notional.get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_position && std::is_void_v<Dummy>, double>
    get_position(const Key& key) const {
        return storage_.position().net_notional.get(key);
    }

    // ========================================================================
    // Direct position manipulation (for testing/initialization)
    // ========================================================================

    // Set position for a specific instrument by signed quantity
    // Positive quantity = long (BID), negative quantity = short (ASK)
    // Computes notional from instrument data: qty * contract_size * spot_price * fx_rate
    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_position && std::is_void_v<Dummy>, void>
    set_instrument_position(const std::string& symbol, int64_t signed_quantity, const Instrument& instrument, const Context& context) {
        Key key = aggregation::GlobalKey::instance();
        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (!pos_data) return;

        // Remove old contribution if exists
        auto it = pos_data->instrument_quantities.find(symbol);
        if (it != pos_data->instrument_quantities.end()) {
            double old_net = compute_net_from_signed_qty(it->second, instrument, context);
            pos_data->net_notional.remove(key, old_net);
        }

        // Add new contribution
        double new_net = compute_net_from_signed_qty(signed_quantity, instrument, context);
        pos_data->net_notional.add(key, new_net);
        pos_data->instrument_quantities[symbol] = signed_quantity;
    }

    // ========================================================================
    // Generic metric interface
    // ========================================================================

    void on_order_added(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double net = compute_net(order, instrument, context);
        auto* stage_data = storage_.get_stage(aggregation::OrderStage::IN_FLIGHT);
        if (stage_data) {
            stage_data->net_notional.add(key, net);
        }
    }

    void on_order_removed(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double net = compute_net(order, instrument, context);
        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (stage_data) {
            stage_data->net_notional.remove(key, net);
        }
    }

    void on_order_updated(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t old_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double old_net = compute_net(old_qty, order.side, instrument, context);
        double new_net = compute_net(order, instrument, context);
        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (stage_data) {
            stage_data->net_notional.remove(key, old_net);
            stage_data->net_notional.add(key, new_net);
        }
    }

    void on_partial_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double filled_net = compute_net(filled_qty, order.side, instrument, context);

        auto* open_data = storage_.get_stage(aggregation::OrderStage::OPEN);
        if (open_data) {
            open_data->net_notional.remove(key, filled_net);
        }

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            pos_data->net_notional.add(key, filled_net);
        }
    }

    void on_full_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double filled_net = compute_net(filled_qty, order.side, instrument, context);

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            pos_data->net_notional.add(key, filled_net);
        }
    }

    void on_state_change(const engine::TrackedOrder& order,
                         const Instrument& instrument,
                         const Context& context,
                         engine::OrderState old_state,
                         engine::OrderState new_state) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        auto old_stage = aggregation::stage_from_order_state(old_state);
        auto new_stage = aggregation::stage_from_order_state(new_state);

        if (old_stage != new_stage && aggregation::is_active_order_state(new_state)) {
            Key key = extract_order_key(order);
            double net = compute_net(order, instrument, context);

            auto* old_data = storage_.get_stage(old_stage);
            auto* new_data = storage_.get_stage(new_stage);

            if (old_data) {
                old_data->net_notional.remove(key, net);
            }
            if (new_data) {
                new_data->net_notional.add(key, net);
            }
        }
    }

    void on_order_updated_with_state_change(const engine::TrackedOrder& order,
                                             const Instrument& instrument,
                                             const Context& context,
                                             int64_t old_qty,
                                             engine::OrderState old_state,
                                             engine::OrderState new_state) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        double old_net = compute_net(old_qty, order.side, instrument, context);
        double new_net = compute_net(order, instrument, context);

        auto old_stage = aggregation::stage_from_order_state(old_state);
        auto new_stage = aggregation::stage_from_order_state(new_state);

        auto* old_data = storage_.get_stage(old_stage);
        auto* new_data = storage_.get_stage(new_stage);

        if (old_data) {
            old_data->net_notional.remove(key, old_net);
        }
        if (new_data) {
            new_data->net_notional.add(key, new_net);
        }
    }

    void clear() {
        storage_.clear();
    }
};

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
        double notional = compute_notional(instrument, context, order.leaves_qty);
        storage_.in_flight().notional.add(key, notional);
    }
}

template<typename Key, typename Context, typename Instrument, typename... Stages>
void NotionalMetric<Key, Context, Instrument, Stages...>::on_order_removed(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context) {
    if (!is_applicable(order)) return;

    Key key = extract_order_key(order);
    auto stage = aggregation::stage_from_order_state(order.state);
    auto* stage_data = storage_.get_stage(stage);
    if (stage_data) {
        double notional = compute_notional(instrument, context, order.leaves_qty);
        stage_data->notional.remove(key, notional);
    }
}

template<typename Key, typename Context, typename Instrument, typename... Stages>
void NotionalMetric<Key, Context, Instrument, Stages...>::on_order_updated(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t old_qty) {
    if (!is_applicable(order)) return;

    Key key = extract_order_key(order);
    auto stage = aggregation::stage_from_order_state(order.state);
    auto* stage_data = storage_.get_stage(stage);
    if (stage_data) {
        double old_notional = compute_notional(instrument, context, old_qty);
        double new_notional = compute_notional(instrument, context, order.leaves_qty);
        stage_data->notional.remove(key, old_notional);
        stage_data->notional.add(key, new_notional);
    }
}

template<typename Key, typename Context, typename Instrument, typename... Stages>
void NotionalMetric<Key, Context, Instrument, Stages...>::on_partial_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
    if (!is_applicable(order)) return;

    Key key = extract_order_key(order);
    double fill_notional = compute_notional(instrument, context, filled_qty);

    // Remove from open stage
    if constexpr (Config::track_open) {
        storage_.open().notional.remove(key, fill_notional);
    }
    // Add to position stage
    if constexpr (Config::track_position) {
        storage_.position().notional.add(key, fill_notional);
    }
}

template<typename Key, typename Context, typename Instrument, typename... Stages>
void NotionalMetric<Key, Context, Instrument, Stages...>::on_full_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
    if (!is_applicable(order)) return;

    Key key = extract_order_key(order);
    double fill_notional = compute_notional(instrument, context, filled_qty);

    // Add to position stage (removal from open/in_flight handled by on_order_removed)
    if constexpr (Config::track_position) {
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

    if (old_stage != new_stage && aggregation::is_active_order_state(new_state)) {
        Key key = extract_order_key(order);
        double notional = compute_notional(instrument, context, order.leaves_qty);

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

    Key key = extract_order_key(order);
    double old_notional = compute_notional(instrument, context, old_qty);
    double new_notional = compute_notional(instrument, context, order.leaves_qty);

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
