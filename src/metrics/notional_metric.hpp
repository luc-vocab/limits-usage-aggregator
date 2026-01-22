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
// StoredNotionalInputs - Computation inputs for drift-free notional tracking
// ============================================================================
//
// When we add a notional contribution for an order, we store the inputs used
// to compute it. When removing, we use these stored inputs to compute exactly
// what was added, preventing drift when spot prices change between operations.
//
struct StoredNotionalInputs {
    int64_t quantity;
    double contract_size;
    double spot_price;
    double fx_rate;

    double compute() const {
        return static_cast<double>(quantity) * contract_size * spot_price * fx_rate;
    }

    // For partial fills: create inputs for a portion of the stored quantity
    StoredNotionalInputs with_quantity(int64_t new_qty) const {
        return StoredNotionalInputs{new_qty, contract_size, spot_price, fx_rate};
    }
};

// StoredNetNotionalInputs - includes side for net (signed) notional computation
struct StoredNetNotionalInputs {
    int64_t quantity;
    double contract_size;
    double spot_price;
    double fx_rate;
    fix::Side side;  // BID = positive, ASK = negative

    double compute() const {
        double notional = static_cast<double>(quantity) * contract_size * spot_price * fx_rate;
        return (side == fix::Side::BID) ? notional : -notional;
    }

    // For partial fills: create inputs for a portion of the stored quantity
    StoredNetNotionalInputs with_quantity(int64_t new_qty) const {
        return StoredNetNotionalInputs{new_qty, contract_size, spot_price, fx_rate, side};
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
    // Per-stage data: maps key -> notional value, plus stored inputs per order
    struct StageData {
        aggregation::AggregationBucket<Key, aggregation::SumCombiner<double>> notional;
        // cl_ord_id -> (key, stored_inputs) for drift-free removal
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

    // Capture current inputs from context for drift-free storage
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
        // cl_ord_id -> (key, stored_inputs) for drift-free removal
        std::unordered_map<std::string, std::pair<Key, StoredNotionalInputs>> order_inputs;

        void clear() {
            gross_notional.clear();
            instrument_quantities.clear();
            order_inputs.clear();
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

    // Capture current inputs from context for drift-free storage
    StoredNotionalInputs capture_inputs(const Context& ctx, const Instrument& inst, int64_t quantity) const {
        return StoredNotionalInputs{
            quantity,
            ctx.contract_size(inst),
            ctx.spot_price(inst),
            ctx.fx_rate(inst)
        };
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
        auto* stage_data = storage_.get_stage(aggregation::OrderStage::IN_FLIGHT);
        if (stage_data) {
            // Capture and store inputs for drift-free removal
            StoredNotionalInputs inputs = capture_inputs(context, instrument, order.leaves_qty);
            double gross = std::abs(inputs.compute());
            stage_data->gross_notional.add(key, gross);
            stage_data->order_inputs[order.key.cl_ord_id] = {key, inputs};
        }
    }

    void on_order_removed(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        (void)instrument;  // Unused - we use stored inputs
        (void)context;     // Unused - we use stored inputs

        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (!stage_data) return;

        // Use stored inputs for drift-free removal
        auto it = stage_data->order_inputs.find(order.key.cl_ord_id);
        if (it != stage_data->order_inputs.end()) {
            Key key = it->second.first;
            double gross = std::abs(it->second.second.compute());
            stage_data->gross_notional.remove(key, gross);
            stage_data->order_inputs.erase(it);
        }
    }

    void on_order_updated(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t old_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;

        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (!stage_data) return;

        Key key = extract_order_key(order);

        // Remove old contribution using stored inputs (or fallback to old_qty if key changed)
        auto it = stage_data->order_inputs.find(order.key.cl_ord_id);
        if (it != stage_data->order_inputs.end()) {
            double old_gross = std::abs(it->second.second.compute());
            stage_data->gross_notional.remove(key, old_gross);
            stage_data->order_inputs.erase(it);
        } else {
            // Fallback for key change during replace: use old_qty with current context
            double old_gross = compute_gross(old_qty, instrument, context);
            stage_data->gross_notional.remove(key, old_gross);
        }

        // Add new contribution with current inputs
        StoredNotionalInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty);
        double new_gross = std::abs(new_inputs.compute());
        stage_data->gross_notional.add(key, new_gross);
        stage_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
    }

    void on_partial_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);

        auto* open_data = storage_.get_stage(aggregation::OrderStage::OPEN);
        if (open_data) {
            // Use stored inputs for drift-free removal (proportional)
            auto it = open_data->order_inputs.find(order.key.cl_ord_id);
            if (it != open_data->order_inputs.end()) {
                StoredNotionalInputs& stored = it->second.second;
                StoredNotionalInputs filled_inputs = stored.with_quantity(filled_qty);
                double filled_gross = std::abs(filled_inputs.compute());
                open_data->gross_notional.remove(key, filled_gross);
                stored.quantity -= filled_qty;  // Update stored quantity
            }
        }

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            // Add to position with CURRENT inputs
            StoredNotionalInputs pos_inputs = capture_inputs(context, instrument, filled_qty);
            double pos_gross = std::abs(pos_inputs.compute());
            pos_data->gross_notional.add(key, pos_gross);
        }
    }

    void on_full_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            // Add to position with CURRENT inputs
            StoredNotionalInputs pos_inputs = capture_inputs(context, instrument, filled_qty);
            double filled_gross = std::abs(pos_inputs.compute());
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

        if (old_stage == new_stage) return;
        if (!aggregation::is_active_order_state(new_state)) return;

        Key key = extract_order_key(order);
        auto* old_data = storage_.get_stage(old_stage);
        auto* new_data = storage_.get_stage(new_stage);

        // Remove from old stage using stored inputs
        if (old_data) {
            auto it = old_data->order_inputs.find(order.key.cl_ord_id);
            if (it != old_data->order_inputs.end()) {
                double old_gross = std::abs(it->second.second.compute());
                old_data->gross_notional.remove(key, old_gross);
                old_data->order_inputs.erase(it);
            }
        }

        // Add to new stage with CURRENT inputs
        if (new_data) {
            StoredNotionalInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty);
            double new_gross = std::abs(new_inputs.compute());
            new_data->gross_notional.add(key, new_gross);
            new_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
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
        auto old_stage = aggregation::stage_from_order_state(old_state);
        auto new_stage = aggregation::stage_from_order_state(new_state);

        auto* old_data = storage_.get_stage(old_stage);
        auto* new_data = storage_.get_stage(new_stage);

        // Remove from old stage using stored inputs (or fallback to old_qty if key changed)
        if (old_data) {
            auto it = old_data->order_inputs.find(order.key.cl_ord_id);
            if (it != old_data->order_inputs.end()) {
                double old_gross = std::abs(it->second.second.compute());
                old_data->gross_notional.remove(key, old_gross);
                old_data->order_inputs.erase(it);
            } else {
                // Fallback for key change during replace: use old_qty with current context
                double old_gross = compute_gross(old_qty, instrument, context);
                old_data->gross_notional.remove(key, old_gross);
            }
        }

        // Add to new stage with CURRENT inputs
        if (new_data) {
            StoredNotionalInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty);
            double new_gross = std::abs(new_inputs.compute());
            new_data->gross_notional.add(key, new_gross);
            new_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
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
        // cl_ord_id -> (key, stored_inputs) for drift-free removal
        std::unordered_map<std::string, std::pair<Key, StoredNetNotionalInputs>> order_inputs;

        void clear() {
            net_notional.clear();
            instrument_quantities.clear();
            order_inputs.clear();
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

    // Capture current inputs from context for drift-free storage
    StoredNetNotionalInputs capture_inputs(const Context& ctx, const Instrument& inst, int64_t quantity, fix::Side side) const {
        return StoredNetNotionalInputs{
            quantity,
            ctx.contract_size(inst),
            ctx.spot_price(inst),
            ctx.fx_rate(inst),
            side
        };
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
        auto* stage_data = storage_.get_stage(aggregation::OrderStage::IN_FLIGHT);
        if (stage_data) {
            // Capture and store inputs for drift-free removal
            StoredNetNotionalInputs inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
            double net = inputs.compute();
            stage_data->net_notional.add(key, net);
            stage_data->order_inputs[order.key.cl_ord_id] = {key, inputs};
        }
    }

    void on_order_removed(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        (void)instrument;  // Unused - we use stored inputs
        (void)context;     // Unused - we use stored inputs

        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (!stage_data) return;

        // Use stored inputs for drift-free removal
        auto it = stage_data->order_inputs.find(order.key.cl_ord_id);
        if (it != stage_data->order_inputs.end()) {
            Key key = it->second.first;
            double net = it->second.second.compute();
            stage_data->net_notional.remove(key, net);
            stage_data->order_inputs.erase(it);
        }
    }

    void on_order_updated(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t old_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;

        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (!stage_data) return;

        Key key = extract_order_key(order);

        // Remove old contribution using stored inputs (or fallback to old_qty if key changed)
        auto it = stage_data->order_inputs.find(order.key.cl_ord_id);
        if (it != stage_data->order_inputs.end()) {
            double old_net = it->second.second.compute();
            stage_data->net_notional.remove(key, old_net);
            stage_data->order_inputs.erase(it);
        } else {
            // Fallback for key change during replace: use old_qty with current context
            double old_net = compute_net(old_qty, order.side, instrument, context);
            stage_data->net_notional.remove(key, old_net);
        }

        // Add new contribution with current inputs
        StoredNetNotionalInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
        double new_net = new_inputs.compute();
        stage_data->net_notional.add(key, new_net);
        stage_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
    }

    void on_partial_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);

        auto* open_data = storage_.get_stage(aggregation::OrderStage::OPEN);
        if (open_data) {
            // Use stored inputs for drift-free removal (proportional)
            auto it = open_data->order_inputs.find(order.key.cl_ord_id);
            if (it != open_data->order_inputs.end()) {
                StoredNetNotionalInputs& stored = it->second.second;
                StoredNetNotionalInputs filled_inputs = stored.with_quantity(filled_qty);
                double filled_net = filled_inputs.compute();
                open_data->net_notional.remove(key, filled_net);
                stored.quantity -= filled_qty;  // Update stored quantity
            }
        }

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            // Add to position with CURRENT inputs
            StoredNetNotionalInputs pos_inputs = capture_inputs(context, instrument, filled_qty, order.side);
            double pos_net = pos_inputs.compute();
            pos_data->net_notional.add(key, pos_net);
        }
    }

    void on_full_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            // Add to position with CURRENT inputs
            StoredNetNotionalInputs pos_inputs = capture_inputs(context, instrument, filled_qty, order.side);
            double filled_net = pos_inputs.compute();
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

        if (old_stage == new_stage) return;
        if (!aggregation::is_active_order_state(new_state)) return;

        Key key = extract_order_key(order);
        auto* old_data = storage_.get_stage(old_stage);
        auto* new_data = storage_.get_stage(new_stage);

        // Remove from old stage using stored inputs
        if (old_data) {
            auto it = old_data->order_inputs.find(order.key.cl_ord_id);
            if (it != old_data->order_inputs.end()) {
                double old_net = it->second.second.compute();
                old_data->net_notional.remove(key, old_net);
                old_data->order_inputs.erase(it);
            }
        }

        // Add to new stage with CURRENT inputs
        if (new_data) {
            StoredNetNotionalInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
            double new_net = new_inputs.compute();
            new_data->net_notional.add(key, new_net);
            new_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
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
        auto old_stage = aggregation::stage_from_order_state(old_state);
        auto new_stage = aggregation::stage_from_order_state(new_state);

        auto* old_data = storage_.get_stage(old_stage);
        auto* new_data = storage_.get_stage(new_stage);

        // Remove from old stage using stored inputs (or fallback to old_qty if key changed)
        if (old_data) {
            auto it = old_data->order_inputs.find(order.key.cl_ord_id);
            if (it != old_data->order_inputs.end()) {
                double old_net = it->second.second.compute();
                old_data->net_notional.remove(key, old_net);
                old_data->order_inputs.erase(it);
            } else {
                // Fallback for key change during replace: use old_qty with current context
                double old_net = compute_net(old_qty, order.side, instrument, context);
                old_data->net_notional.remove(key, old_net);
            }
        }

        // Add to new stage with CURRENT inputs
        if (new_data) {
            StoredNetNotionalInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
            double new_net = new_inputs.compute();
            new_data->net_notional.add(key, new_net);
            new_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
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
        // Capture and store inputs for drift-free removal
        StoredNotionalInputs inputs = capture_inputs(context, instrument, order.leaves_qty);
        double notional = inputs.compute();
        storage_.in_flight().notional.add(key, notional);
        storage_.in_flight().order_inputs[order.key.cl_ord_id] = {key, inputs};
    }
}

template<typename Key, typename Context, typename Instrument, typename... Stages>
void NotionalMetric<Key, Context, Instrument, Stages...>::on_order_removed(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context) {
    if (!is_applicable(order)) return;
    (void)instrument;  // Unused - we use stored inputs
    (void)context;     // Unused - we use stored inputs

    auto stage = aggregation::stage_from_order_state(order.state);
    auto* stage_data = storage_.get_stage(stage);
    if (!stage_data) return;

    // Use stored inputs for drift-free removal
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

    // Remove old contribution using stored inputs (or fallback to old_qty if key changed)
    auto it = stage_data->order_inputs.find(order.key.cl_ord_id);
    if (it != stage_data->order_inputs.end()) {
        double old_notional = it->second.second.compute();
        stage_data->notional.remove(key, old_notional);
        stage_data->order_inputs.erase(it);
    } else {
        // Fallback for key change during replace: use old_qty with current context
        double old_notional = compute_notional(instrument, context, old_qty);
        stage_data->notional.remove(key, old_notional);
    }

    // Add new contribution with current inputs
    StoredNotionalInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty);
    double new_notional = new_inputs.compute();
    stage_data->notional.add(key, new_notional);
    stage_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
}

template<typename Key, typename Context, typename Instrument, typename... Stages>
void NotionalMetric<Key, Context, Instrument, Stages...>::on_partial_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
    if (!is_applicable(order)) return;

    Key key = extract_order_key(order);

    // Remove filled portion from open stage using stored inputs (proportional)
    if constexpr (Config::track_open) {
        auto* open_data = &storage_.open();
        auto it = open_data->order_inputs.find(order.key.cl_ord_id);
        if (it != open_data->order_inputs.end()) {
            StoredNotionalInputs& stored = it->second.second;
            // Compute filled notional using stored inputs (proportional)
            StoredNotionalInputs filled_inputs = stored.with_quantity(filled_qty);
            double filled_notional = filled_inputs.compute();
            open_data->notional.remove(key, filled_notional);
            // Update stored quantity for remaining order
            stored.quantity -= filled_qty;
        }
    }

    // Add to position stage with CURRENT inputs
    if constexpr (Config::track_position) {
        StoredNotionalInputs pos_inputs = capture_inputs(context, instrument, filled_qty);
        double pos_notional = pos_inputs.compute();
        storage_.position().notional.add(key, pos_notional);
        // Note: Position stage typically doesn't need per-order tracking
        // as positions are aggregated and don't get "removed" in the same way
    }
}

template<typename Key, typename Context, typename Instrument, typename... Stages>
void NotionalMetric<Key, Context, Instrument, Stages...>::on_full_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
    if (!is_applicable(order)) return;

    Key key = extract_order_key(order);

    // Add to position stage with CURRENT inputs
    // (removal from open/in_flight handled by on_order_removed which uses stored inputs)
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

    // Remove from old stage using stored inputs
    if (old_stage_data) {
        auto it = old_stage_data->order_inputs.find(order.key.cl_ord_id);
        if (it != old_stage_data->order_inputs.end()) {
            double old_notional = it->second.second.compute();
            old_stage_data->notional.remove(key, old_notional);
            old_stage_data->order_inputs.erase(it);
        }
    }

    // Add to new stage with CURRENT inputs
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

    // Remove from old stage using stored inputs (or fallback to old_qty if key changed)
    if (old_stage_data) {
        auto it = old_stage_data->order_inputs.find(order.key.cl_ord_id);
        if (it != old_stage_data->order_inputs.end()) {
            double old_notional = it->second.second.compute();
            old_stage_data->notional.remove(key, old_notional);
            old_stage_data->order_inputs.erase(it);
        } else {
            // Fallback for key change during replace: use old_qty with current context
            double old_notional = compute_notional(instrument, context, old_qty);
            old_stage_data->notional.remove(key, old_notional);
        }
    }

    // Add to new stage with CURRENT inputs
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
