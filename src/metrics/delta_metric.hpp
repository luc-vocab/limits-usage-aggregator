#pragma once

#include "../aggregation/staged_metric.hpp"
#include "../aggregation/aggregation_core.hpp"
#include "../aggregation/key_extractors.hpp"
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
// StoredDeltaInputs - Computation inputs for drift-free delta tracking
// ============================================================================
//
// When we add a delta contribution for an order, we store the inputs used
// to compute it. When removing, we use these stored inputs to compute exactly
// what was added, preventing drift when underlyer spot/delta changes.
//
struct StoredDeltaInputs {
    int64_t quantity;
    double delta;
    double contract_size;
    double underlyer_spot;
    double fx_rate;
    fix::Side side;  // For gross/net computation

    double compute_exposure() const {
        return static_cast<double>(quantity) * delta * contract_size * underlyer_spot * fx_rate;
    }

    double compute_gross() const {
        return std::abs(compute_exposure());
    }

    double compute_net() const {
        double exposure = compute_exposure();
        return (side == fix::Side::BID) ? exposure : -exposure;
    }

    // For partial fills: create inputs for a portion of the stored quantity
    StoredDeltaInputs with_quantity(int64_t new_qty) const {
        return StoredDeltaInputs{new_qty, delta, contract_size, underlyer_spot, fx_rate, side};
    }
};

// ============================================================================
// DeltaMetric - Single-purpose delta tracking metric
// ============================================================================
//
// Tracks gross and net delta exposure for a single key type.
// Uses precomputed delta values at add time.
//
// Template parameters:
//   Key: The grouping key type (GlobalKey, UnderlyerKey, etc.)
//   Context: Type providing accessor methods for instrument properties
//   Instrument: Must satisfy the option instrument requirements (delta support)
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//
// For GlobalKey: tracks total system-wide delta
// For UnderlyerKey: tracks delta per underlyer
//

template<typename Key, typename Context, typename Instrument, typename... Stages>
class DeltaMetric {
    static_assert(instrument::is_option_instrument_v<Instrument>,
                  "Instrument must satisfy option instrument requirements (underlyer, delta support)");

private:
    struct StageData {
        aggregation::AggregationBucket<Key, aggregation::DeltaCombiner> delta;
        // cl_ord_id -> (key, stored_inputs) for drift-free removal
        std::unordered_map<std::string, std::pair<Key, StoredDeltaInputs>> order_inputs;

        void clear() {
            delta.clear();
            order_inputs.clear();
        }
    };

    using Storage = aggregation::StagedMetric<StageData, Stages...>;

    Storage storage_;

    // Compute delta value for an order (bid = positive, ask = negative)
    aggregation::DeltaValue compute_delta_value(const engine::TrackedOrder& order, const Instrument& inst, const Context& ctx) const {
        double delta_exp = instrument::compute_delta_exposure(ctx, inst, order.leaves_qty);
        double gross = std::abs(delta_exp);
        double net = (order.side == fix::Side::BID) ? delta_exp : -delta_exp;
        return aggregation::DeltaValue{gross, net};
    }

    aggregation::DeltaValue compute_delta_value(int64_t quantity, fix::Side side, const Instrument& inst, const Context& ctx) const {
        double delta_exp = instrument::compute_delta_exposure(ctx, inst, quantity);
        double gross = std::abs(delta_exp);
        double net = (side == fix::Side::BID) ? delta_exp : -delta_exp;
        return aggregation::DeltaValue{gross, net};
    }

    Key extract_key(const engine::TrackedOrder& order) const {
        return aggregation::KeyExtractor<Key>::extract(order);
    }

    // Capture current inputs from context for drift-free storage
    StoredDeltaInputs capture_inputs(const Context& ctx, const Instrument& inst, int64_t quantity, fix::Side side) const {
        return StoredDeltaInputs{
            quantity,
            ctx.delta(inst),
            ctx.contract_size(inst),
            ctx.underlyer_spot(inst),
            ctx.fx_rate(inst),
            side
        };
    }

    // Compute DeltaValue from stored inputs
    aggregation::DeltaValue compute_delta_value_from_stored(const StoredDeltaInputs& inputs) const {
        return aggregation::DeltaValue{inputs.compute_gross(), inputs.compute_net()};
    }

public:
    using key_type = Key;
    using context_type = Context;
    using instrument_type = Instrument;

    DeltaMetric() = default;

    // ========================================================================
    // Accessors
    // ========================================================================

    // Get combined delta (gross, net) across all tracked stages for a key
    aggregation::DeltaValue get(const Key& key) const {
        aggregation::DeltaValue total{0.0, 0.0};
        storage_.for_each_stage([&key, &total](aggregation::OrderStage /*stage*/, const StageData& data) {
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

    void on_order_added(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_key(order);
        auto* stage_data = storage_.get_stage(aggregation::OrderStage::IN_FLIGHT);
        if (stage_data) {
            // Capture and store inputs for drift-free removal
            StoredDeltaInputs inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
            auto delta_val = compute_delta_value_from_stored(inputs);
            stage_data->delta.add(key, delta_val);
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
            auto delta_val = compute_delta_value_from_stored(it->second.second);
            stage_data->delta.remove(key, delta_val);
            stage_data->order_inputs.erase(it);
        }
    }

    void on_order_updated(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t old_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;

        auto stage = aggregation::stage_from_order_state(order.state);
        auto* stage_data = storage_.get_stage(stage);
        if (!stage_data) return;

        Key key = extract_key(order);

        // Remove old contribution using stored inputs (or fallback to old_qty if key changed)
        auto it = stage_data->order_inputs.find(order.key.cl_ord_id);
        if (it != stage_data->order_inputs.end()) {
            auto old_delta = compute_delta_value_from_stored(it->second.second);
            stage_data->delta.remove(key, old_delta);
            stage_data->order_inputs.erase(it);
        } else {
            // Fallback for key change during replace: use old_qty with current context
            auto old_delta = compute_delta_value(old_qty, order.side, instrument, context);
            stage_data->delta.remove(key, old_delta);
        }

        // Add new contribution with current inputs
        StoredDeltaInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
        auto new_delta = compute_delta_value_from_stored(new_inputs);
        stage_data->delta.add(key, new_delta);
        stage_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
    }

    void on_partial_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_key(order);

        // Remove filled portion from open stage using stored inputs (proportional)
        auto* open_data = storage_.get_stage(aggregation::OrderStage::OPEN);
        if (open_data) {
            auto it = open_data->order_inputs.find(order.key.cl_ord_id);
            if (it != open_data->order_inputs.end()) {
                StoredDeltaInputs& stored = it->second.second;
                StoredDeltaInputs filled_inputs = stored.with_quantity(filled_qty);
                auto filled_delta = compute_delta_value_from_stored(filled_inputs);
                open_data->delta.remove(key, filled_delta);
                stored.quantity -= filled_qty;  // Update stored quantity
            }
        }

        // Add to position stage with CURRENT inputs
        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            StoredDeltaInputs pos_inputs = capture_inputs(context, instrument, filled_qty, order.side);
            auto pos_delta = compute_delta_value_from_stored(pos_inputs);
            pos_data->delta.add(key, pos_delta);
        }
    }

    void on_full_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_key(order);

        // Credit position stage with filled quantity using CURRENT inputs
        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            StoredDeltaInputs pos_inputs = capture_inputs(context, instrument, filled_qty, order.side);
            auto filled_delta = compute_delta_value_from_stored(pos_inputs);
            pos_data->delta.add(key, filled_delta);
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

        Key key = extract_key(order);
        auto* old_data = storage_.get_stage(old_stage);
        auto* new_data = storage_.get_stage(new_stage);

        // Remove from old stage using stored inputs
        if (old_data) {
            auto it = old_data->order_inputs.find(order.key.cl_ord_id);
            if (it != old_data->order_inputs.end()) {
                auto old_delta = compute_delta_value_from_stored(it->second.second);
                old_data->delta.remove(key, old_delta);
                old_data->order_inputs.erase(it);
            }
        }

        // Add to new stage with CURRENT inputs
        if (new_data) {
            StoredDeltaInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
            auto new_delta = compute_delta_value_from_stored(new_inputs);
            new_data->delta.add(key, new_delta);
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

        Key key = extract_key(order);
        auto old_stage = aggregation::stage_from_order_state(old_state);
        auto new_stage = aggregation::stage_from_order_state(new_state);

        auto* old_data = storage_.get_stage(old_stage);
        auto* new_data = storage_.get_stage(new_stage);

        // Remove from old stage using stored inputs (or fallback to old_qty if key changed)
        if (old_data) {
            auto it = old_data->order_inputs.find(order.key.cl_ord_id);
            if (it != old_data->order_inputs.end()) {
                auto old_delta = compute_delta_value_from_stored(it->second.second);
                old_data->delta.remove(key, old_delta);
                old_data->order_inputs.erase(it);
            } else {
                // Fallback for key change during replace: use old_qty with current context
                auto old_delta = compute_delta_value(old_qty, order.side, instrument, context);
                old_data->delta.remove(key, old_delta);
            }
        }

        // Add to new stage with CURRENT inputs
        if (new_data) {
            StoredDeltaInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
            auto new_delta = compute_delta_value_from_stored(new_inputs);
            new_data->delta.add(key, new_delta);
            new_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
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
template<typename Context, typename Instrument, typename... Stages>
using GlobalDeltaMetric = DeltaMetric<aggregation::GlobalKey, Context, Instrument, Stages...>;

// Per-underlyer delta metric
template<typename Context, typename Instrument, typename... Stages>
using UnderlyerDeltaMetric = DeltaMetric<aggregation::UnderlyerKey, Context, Instrument, Stages...>;

// ============================================================================
// GrossDeltaMetric - Single-value metric tracking absolute delta exposure
// ============================================================================
//
// Tracks only gross (absolute) delta exposure. Designed for use with the
// generic limit checking system where each metric has one value type.
//

template<typename Key, typename Context, typename Instrument, typename... Stages>
class GrossDeltaMetric {
    static_assert(instrument::is_option_instrument_v<Instrument>,
                  "Instrument must satisfy option instrument requirements (underlyer, delta support)");

public:
    using key_type = Key;
    using value_type = double;
    using context_type = Context;
    using instrument_type = Instrument;
    using Config = aggregation::StageConfig<Stages...>;

    // ========================================================================
    // Static methods for pre-trade limit checking
    // ========================================================================

    // Compute the gross delta contribution for a new order
    template<typename Ctx, typename Inst>
    static double compute_order_contribution(const fix::NewOrderSingle& order, const Inst& instrument, const Ctx& context) {
        double delta_exp = instrument::compute_delta_exposure(context, instrument, order.quantity);
        return std::abs(delta_exp);
    }

    // Compute the gross delta contribution for an order update (new - old)
    template<typename Ctx, typename Inst>
    static double compute_update_contribution(
        const fix::OrderCancelReplaceRequest& update,
        const engine::TrackedOrder& existing_order,
        const Inst& instrument,
        const Ctx& context) {
        double old_delta = std::abs(instrument::compute_delta_exposure(context, instrument, existing_order.leaves_qty));
        double new_delta = std::abs(instrument::compute_delta_exposure(context, instrument, update.quantity));
        return new_delta - old_delta;
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
            static_assert(sizeof(Key) == 0, "Unsupported key type for GrossDeltaMetric");
        }
    }

    // Get the limit type for this metric
    static constexpr engine::LimitType limit_type() {
        return engine::LimitType::GROSS_DELTA;
    }

private:
    struct StageData {
        aggregation::AggregationBucket<Key, aggregation::SumCombiner<double>> gross_delta;
        // cl_ord_id -> (key, stored_inputs) for drift-free removal
        std::unordered_map<std::string, std::pair<Key, StoredDeltaInputs>> order_inputs;

        void clear() {
            gross_delta.clear();
            order_inputs.clear();
        }
    };

    using Storage = aggregation::StagedMetric<StageData, Stages...>;

    Storage storage_;

    double compute_gross(const engine::TrackedOrder& order, const Instrument& inst, const Context& ctx) const {
        double delta_exp = instrument::compute_delta_exposure(ctx, inst, order.leaves_qty);
        return std::abs(delta_exp);
    }

    double compute_gross(int64_t quantity, const Instrument& inst, const Context& ctx) const {
        double delta_exp = instrument::compute_delta_exposure(ctx, inst, quantity);
        return std::abs(delta_exp);
    }

    Key extract_order_key(const engine::TrackedOrder& order) const {
        return aggregation::KeyExtractor<Key>::extract(order);
    }

    // Capture current inputs from context for drift-free storage
    StoredDeltaInputs capture_inputs(const Context& ctx, const Instrument& inst, int64_t quantity, fix::Side side) const {
        return StoredDeltaInputs{
            quantity,
            ctx.delta(inst),
            ctx.contract_size(inst),
            ctx.underlyer_spot(inst),
            ctx.fx_rate(inst),
            side
        };
    }

public:
    GrossDeltaMetric() = default;

    // ========================================================================
    // Accessors
    // ========================================================================

    double get(const Key& key) const {
        double total = 0.0;
        storage_.for_each_stage([&key, &total](aggregation::OrderStage /*stage*/, const StageData& data) {
            total += data.gross_delta.get(key);
        });
        return total;
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_open && std::is_void_v<Dummy>, double>
    get_open(const Key& key) const {
        return storage_.open().gross_delta.get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_in_flight && std::is_void_v<Dummy>, double>
    get_in_flight(const Key& key) const {
        return storage_.in_flight().gross_delta.get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_position && std::is_void_v<Dummy>, double>
    get_position(const Key& key) const {
        return storage_.position().gross_delta.get(key);
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
            StoredDeltaInputs inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
            double gross = inputs.compute_gross();
            stage_data->gross_delta.add(key, gross);
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
            double gross = it->second.second.compute_gross();
            stage_data->gross_delta.remove(key, gross);
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
            double old_gross = it->second.second.compute_gross();
            stage_data->gross_delta.remove(key, old_gross);
            stage_data->order_inputs.erase(it);
        } else {
            // Fallback for key change during replace: use old_qty with current context
            double old_gross = compute_gross(old_qty, instrument, context);
            stage_data->gross_delta.remove(key, old_gross);
        }

        // Add new contribution with current inputs
        StoredDeltaInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
        double new_gross = new_inputs.compute_gross();
        stage_data->gross_delta.add(key, new_gross);
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
                StoredDeltaInputs& stored = it->second.second;
                StoredDeltaInputs filled_inputs = stored.with_quantity(filled_qty);
                double filled_gross = filled_inputs.compute_gross();
                open_data->gross_delta.remove(key, filled_gross);
                stored.quantity -= filled_qty;  // Update stored quantity
            }
        }

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            // Add to position with CURRENT inputs
            StoredDeltaInputs pos_inputs = capture_inputs(context, instrument, filled_qty, order.side);
            double pos_gross = pos_inputs.compute_gross();
            pos_data->gross_delta.add(key, pos_gross);
        }
    }

    void on_full_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            // Add to position with CURRENT inputs
            StoredDeltaInputs pos_inputs = capture_inputs(context, instrument, filled_qty, order.side);
            double filled_gross = pos_inputs.compute_gross();
            pos_data->gross_delta.add(key, filled_gross);
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
                double old_gross = it->second.second.compute_gross();
                old_data->gross_delta.remove(key, old_gross);
                old_data->order_inputs.erase(it);
            }
        }

        // Add to new stage with CURRENT inputs
        if (new_data) {
            StoredDeltaInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
            double new_gross = new_inputs.compute_gross();
            new_data->gross_delta.add(key, new_gross);
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
                double old_gross = it->second.second.compute_gross();
                old_data->gross_delta.remove(key, old_gross);
                old_data->order_inputs.erase(it);
            } else {
                // Fallback for key change during replace: use old_qty with current context
                double old_gross = compute_gross(old_qty, instrument, context);
                old_data->gross_delta.remove(key, old_gross);
            }
        }

        // Add to new stage with CURRENT inputs
        if (new_data) {
            StoredDeltaInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
            double new_gross = new_inputs.compute_gross();
            new_data->gross_delta.add(key, new_gross);
            new_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
        }
    }

    void clear() {
        storage_.clear();
    }
};

// ============================================================================
// NetDeltaMetric - Single-value metric tracking signed delta exposure
// ============================================================================
//
// Tracks only net (signed) delta exposure. Designed for use with the
// generic limit checking system where each metric has one value type.
//

template<typename Key, typename Context, typename Instrument, typename... Stages>
class NetDeltaMetric {
    static_assert(instrument::is_option_instrument_v<Instrument>,
                  "Instrument must satisfy option instrument requirements (underlyer, delta support)");

public:
    using key_type = Key;
    using value_type = double;
    using context_type = Context;
    using instrument_type = Instrument;
    using Config = aggregation::StageConfig<Stages...>;

    // ========================================================================
    // Static methods for pre-trade limit checking
    // ========================================================================

    // Compute the net delta contribution for a new order
    template<typename Ctx, typename Inst>
    static double compute_order_contribution(const fix::NewOrderSingle& order, const Inst& instrument, const Ctx& context) {
        double delta_exp = instrument::compute_delta_exposure(context, instrument, order.quantity);
        return (order.side == fix::Side::BID) ? delta_exp : -delta_exp;
    }

    // Compute the net delta contribution for an order update (new - old)
    template<typename Ctx, typename Inst>
    static double compute_update_contribution(
        const fix::OrderCancelReplaceRequest& update,
        const engine::TrackedOrder& existing_order,
        const Inst& instrument,
        const Ctx& context) {
        double old_delta = instrument::compute_delta_exposure(context, instrument, existing_order.leaves_qty);
        double old_net = (existing_order.side == fix::Side::BID) ? old_delta : -old_delta;

        double new_delta = instrument::compute_delta_exposure(context, instrument, update.quantity);
        double new_net = (update.side == fix::Side::BID) ? new_delta : -new_delta;

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
            static_assert(sizeof(Key) == 0, "Unsupported key type for NetDeltaMetric");
        }
    }

    // Get the limit type for this metric
    static constexpr engine::LimitType limit_type() {
        return engine::LimitType::NET_DELTA;
    }

private:
    struct StageData {
        aggregation::AggregationBucket<Key, aggregation::SumCombiner<double>> net_delta;
        // cl_ord_id -> (key, stored_inputs) for drift-free removal
        std::unordered_map<std::string, std::pair<Key, StoredDeltaInputs>> order_inputs;

        void clear() {
            net_delta.clear();
            order_inputs.clear();
        }
    };

    using Storage = aggregation::StagedMetric<StageData, Stages...>;

    Storage storage_;

    double compute_net(const engine::TrackedOrder& order, const Instrument& inst, const Context& ctx) const {
        double delta_exp = instrument::compute_delta_exposure(ctx, inst, order.leaves_qty);
        return (order.side == fix::Side::BID) ? delta_exp : -delta_exp;
    }

    double compute_net(int64_t quantity, fix::Side side, const Instrument& inst, const Context& ctx) const {
        double delta_exp = instrument::compute_delta_exposure(ctx, inst, quantity);
        return (side == fix::Side::BID) ? delta_exp : -delta_exp;
    }

    Key extract_order_key(const engine::TrackedOrder& order) const {
        return aggregation::KeyExtractor<Key>::extract(order);
    }

    // Capture current inputs from context for drift-free storage
    StoredDeltaInputs capture_inputs(const Context& ctx, const Instrument& inst, int64_t quantity, fix::Side side) const {
        return StoredDeltaInputs{
            quantity,
            ctx.delta(inst),
            ctx.contract_size(inst),
            ctx.underlyer_spot(inst),
            ctx.fx_rate(inst),
            side
        };
    }

public:
    NetDeltaMetric() = default;

    // ========================================================================
    // Accessors
    // ========================================================================

    double get(const Key& key) const {
        double total = 0.0;
        storage_.for_each_stage([&key, &total](aggregation::OrderStage /*stage*/, const StageData& data) {
            total += data.net_delta.get(key);
        });
        return total;
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_open && std::is_void_v<Dummy>, double>
    get_open(const Key& key) const {
        return storage_.open().net_delta.get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_in_flight && std::is_void_v<Dummy>, double>
    get_in_flight(const Key& key) const {
        return storage_.in_flight().net_delta.get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_position && std::is_void_v<Dummy>, double>
    get_position(const Key& key) const {
        return storage_.position().net_delta.get(key);
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
            StoredDeltaInputs inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
            double net = inputs.compute_net();
            stage_data->net_delta.add(key, net);
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
            double net = it->second.second.compute_net();
            stage_data->net_delta.remove(key, net);
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
            double old_net = it->second.second.compute_net();
            stage_data->net_delta.remove(key, old_net);
            stage_data->order_inputs.erase(it);
        } else {
            // Fallback for key change during replace: use old_qty with current context
            double old_net = compute_net(old_qty, order.side, instrument, context);
            stage_data->net_delta.remove(key, old_net);
        }

        // Add new contribution with current inputs
        StoredDeltaInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
        double new_net = new_inputs.compute_net();
        stage_data->net_delta.add(key, new_net);
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
                StoredDeltaInputs& stored = it->second.second;
                StoredDeltaInputs filled_inputs = stored.with_quantity(filled_qty);
                double filled_net = filled_inputs.compute_net();
                open_data->net_delta.remove(key, filled_net);
                stored.quantity -= filled_qty;  // Update stored quantity
            }
        }

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            // Add to position with CURRENT inputs
            StoredDeltaInputs pos_inputs = capture_inputs(context, instrument, filled_qty, order.side);
            double pos_net = pos_inputs.compute_net();
            pos_data->net_delta.add(key, pos_net);
        }
    }

    void on_full_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            // Add to position with CURRENT inputs
            StoredDeltaInputs pos_inputs = capture_inputs(context, instrument, filled_qty, order.side);
            double filled_net = pos_inputs.compute_net();
            pos_data->net_delta.add(key, filled_net);
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
                double old_net = it->second.second.compute_net();
                old_data->net_delta.remove(key, old_net);
                old_data->order_inputs.erase(it);
            }
        }

        // Add to new stage with CURRENT inputs
        if (new_data) {
            StoredDeltaInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
            double new_net = new_inputs.compute_net();
            new_data->net_delta.add(key, new_net);
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
                double old_net = it->second.second.compute_net();
                old_data->net_delta.remove(key, old_net);
                old_data->order_inputs.erase(it);
            } else {
                // Fallback for key change during replace: use old_qty with current context
                double old_net = compute_net(old_qty, order.side, instrument, context);
                old_data->net_delta.remove(key, old_net);
            }
        }

        // Add to new stage with CURRENT inputs
        if (new_data) {
            StoredDeltaInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
            double new_net = new_inputs.compute_net();
            new_data->net_delta.add(key, new_net);
            new_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
        }
    }

    void clear() {
        storage_.clear();
    }
};

// ============================================================================
// Type aliases for Gross/Net delta metrics
// ============================================================================

template<typename Context, typename Instrument, typename... Stages>
using GlobalGrossDeltaMetric = GrossDeltaMetric<aggregation::GlobalKey, Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using UnderlyerGrossDeltaMetric = GrossDeltaMetric<aggregation::UnderlyerKey, Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using GlobalNetDeltaMetric = NetDeltaMetric<aggregation::GlobalKey, Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using UnderlyerNetDeltaMetric = NetDeltaMetric<aggregation::UnderlyerKey, Context, Instrument, Stages...>;

} // namespace metrics

// Include TrackedOrder definition for complete type info
#include "../engine/order_state.hpp"
