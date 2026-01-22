#pragma once

#include "../aggregation/staged_metric.hpp"
#include "../aggregation/aggregation_core.hpp"
#include "../aggregation/key_extractors.hpp"
#include "../aggregation/container_types.hpp"
#include "../fix/fix_messages.hpp"
#include "metric_policies.hpp"
#include <cmath>

// Forward declarations
namespace engine {
    struct TrackedOrder;
    enum class OrderState;
    enum class LimitType;
}

namespace metrics {

// ============================================================================
// BaseExposureMetric - Unified template for all exposure-based metrics
// ============================================================================
//
// This template provides a single implementation of all event handlers for
// exposure-based metrics (delta, vega, notional in both gross and net forms).
//
// Template parameters:
//   Key: The grouping key type (GlobalKey, UnderlyerKey, etc.)
//   Context: Type providing accessor methods for instrument properties
//   Instrument: The instrument type
//   InputPolicy: Defines StoredInputs and capture/compute methods
//   ValuePolicy: Defines how to derive final value (gross vs net)
//   LimitTypeVal: The engine::LimitType value for this metric
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//

template<typename Key, typename Context, typename Instrument,
         typename InputPolicy, typename ValuePolicy,
         engine::LimitType LimitTypeVal, typename... Stages>
class BaseExposureMetric {
public:
    using key_type = Key;
    using value_type = double;
    using context_type = Context;
    using instrument_type = Instrument;
    using input_policy = InputPolicy;
    using value_policy = ValuePolicy;
    using Config = aggregation::StageConfig<Stages...>;

    // Type alias for stored inputs from the input policy
    using StoredInputs = typename InputPolicy::StoredInputs;

    // ========================================================================
    // Static methods for pre-trade limit checking
    // ========================================================================

    // Compute the contribution for a new order
    template<typename Ctx, typename Inst>
    static double compute_order_contribution(const fix::NewOrderSingle& order,
                                             const Inst& instrument,
                                             const Ctx& context) {
        double exposure = InputPolicy::compute_from_context(context, instrument, order.quantity, order.side);
        return ValuePolicy::compute_from_exposure(exposure, order.side);
    }

    // Compute the contribution for an order update (new - old)
    template<typename Ctx, typename Inst>
    static double compute_update_contribution(
        const fix::OrderCancelReplaceRequest& update,
        const engine::TrackedOrder& existing_order,
        const Inst& instrument,
        const Ctx& context) {
        double old_exposure = InputPolicy::compute_from_context(context, instrument, existing_order.leaves_qty, existing_order.side);
        double old_value = ValuePolicy::compute_from_exposure(old_exposure, existing_order.side);

        double new_exposure = InputPolicy::compute_from_context(context, instrument, update.quantity, update.side);
        double new_value = ValuePolicy::compute_from_exposure(new_exposure, update.side);

        return new_value - old_value;
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
            static_assert(sizeof(Key) == 0, "Unsupported key type for BaseExposureMetric");
        }
    }

    // Get the limit type for this metric
    static constexpr engine::LimitType limit_type() {
        return LimitTypeVal;
    }

private:
    struct StageData {
        aggregation::AggregationBucket<Key, aggregation::SumCombiner<double>> value;
        // Track quantities per instrument for position recomputation (only for notional)
        aggregation::HashMap<std::string, int64_t> instrument_quantities;
        // cl_ord_id -> (key, stored_inputs) for drift-free removal
        aggregation::HashMap<std::string, std::pair<Key, StoredInputs>> order_inputs;

        void clear() {
            value.clear();
            instrument_quantities.clear();
            order_inputs.clear();
        }
    };

    using Storage = aggregation::StagedMetric<StageData, Stages...>;

    Storage storage_;

    Key extract_order_key(const engine::TrackedOrder& order) const {
        return aggregation::KeyExtractor<Key>::extract(order);
    }

    // Compute value from stored inputs using the value policy
    double compute_value(const StoredInputs& inputs) const {
        return ValuePolicy::compute(inputs);
    }

    // Compute value from context (fallback)
    double compute_value_from_context(const Context& ctx, const Instrument& inst,
                                      int64_t quantity, fix::Side side) const {
        double exposure = InputPolicy::compute_from_context(ctx, inst, quantity, side);
        return ValuePolicy::compute_from_exposure(exposure, side);
    }

public:
    BaseExposureMetric() = default;

    // ========================================================================
    // Accessors
    // ========================================================================

    double get(const Key& key) const {
        double total = 0.0;
        storage_.for_each_stage([&key, &total](aggregation::OrderStage /*stage*/, const StageData& data) {
            total += data.value.get(key);
        });
        return total;
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_open && std::is_void_v<Dummy>, double>
    get_open(const Key& key) const {
        return storage_.open().value.get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_in_flight && std::is_void_v<Dummy>, double>
    get_in_flight(const Key& key) const {
        return storage_.in_flight().value.get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_position && std::is_void_v<Dummy>, double>
    get_position(const Key& key) const {
        return storage_.position().value.get(key);
    }

    // ========================================================================
    // Direct position manipulation (only available when InputPolicy supports it)
    // ========================================================================

    // Set position for a specific instrument by quantity
    // Only enabled when InputPolicy::supports_position_set is true
    template<typename Dummy = void>
    std::enable_if_t<Storage::Config::track_position && InputPolicy::supports_position_set && std::is_void_v<Dummy>, void>
    set_instrument_position(const std::string& symbol, int64_t signed_quantity,
                            const Instrument& instrument, const Context& context) {
        Key key = aggregation::GlobalKey::instance();
        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (!pos_data) return;

        // Remove old contribution if exists
        auto it = pos_data->instrument_quantities.find(symbol);
        if (it != pos_data->instrument_quantities.end()) {
            // Compute old value based on ValuePolicy type
            fix::Side old_side = (it->second >= 0) ? fix::Side::BID : fix::Side::ASK;
            double old_val = compute_value_from_context(context, instrument, std::abs(it->second), old_side);
            pos_data->value.remove(key, old_val);
        }

        // Add new contribution
        fix::Side new_side = (signed_quantity >= 0) ? fix::Side::BID : fix::Side::ASK;
        double new_val = compute_value_from_context(context, instrument, std::abs(signed_quantity), new_side);
        pos_data->value.add(key, new_val);
        pos_data->instrument_quantities[symbol] = signed_quantity;
    }

    // ========================================================================
    // Generic metric interface (event handlers)
    // ========================================================================

    void on_order_added(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        auto* stage_data = storage_.get_stage(aggregation::OrderStage::IN_FLIGHT);
        if (stage_data) {
            // Capture and store inputs for drift-free removal
            StoredInputs inputs = InputPolicy::capture(context, instrument, order.leaves_qty, order.side);
            double val = compute_value(inputs);
            stage_data->value.add(key, val);
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
            double val = compute_value(it->second.second);
            stage_data->value.remove(key, val);
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
            double old_val = compute_value(it->second.second);
            stage_data->value.remove(key, old_val);
            stage_data->order_inputs.erase(it);
        } else {
            // Fallback for key change during replace: use old_qty with current context
            double old_val = compute_value_from_context(context, instrument, old_qty, order.side);
            stage_data->value.remove(key, old_val);
        }

        // Add new contribution with current inputs
        StoredInputs new_inputs = InputPolicy::capture(context, instrument, order.leaves_qty, order.side);
        double new_val = compute_value(new_inputs);
        stage_data->value.add(key, new_val);
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
                StoredInputs& stored = it->second.second;
                StoredInputs filled_inputs = stored.with_quantity(filled_qty);
                double filled_val = compute_value(filled_inputs);
                open_data->value.remove(key, filled_val);
                stored.quantity -= filled_qty;  // Update stored quantity
            }
        }

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            // Add to position with CURRENT inputs
            StoredInputs pos_inputs = InputPolicy::capture(context, instrument, filled_qty, order.side);
            double pos_val = compute_value(pos_inputs);
            pos_data->value.add(key, pos_val);
        }
    }

    void on_full_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            // Add to position with CURRENT inputs
            StoredInputs pos_inputs = InputPolicy::capture(context, instrument, filled_qty, order.side);
            double filled_val = compute_value(pos_inputs);
            pos_data->value.add(key, filled_val);
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
                double old_val = compute_value(it->second.second);
                old_data->value.remove(key, old_val);
                old_data->order_inputs.erase(it);
            }
        }

        // Add to new stage with CURRENT inputs
        if (new_data) {
            StoredInputs new_inputs = InputPolicy::capture(context, instrument, order.leaves_qty, order.side);
            double new_val = compute_value(new_inputs);
            new_data->value.add(key, new_val);
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
                double old_val = compute_value(it->second.second);
                old_data->value.remove(key, old_val);
                old_data->order_inputs.erase(it);
            } else {
                // Fallback for key change during replace: use old_qty with current context
                double old_val = compute_value_from_context(context, instrument, old_qty, order.side);
                old_data->value.remove(key, old_val);
            }
        }

        // Add to new stage with CURRENT inputs
        if (new_data) {
            StoredInputs new_inputs = InputPolicy::capture(context, instrument, order.leaves_qty, order.side);
            double new_val = compute_value(new_inputs);
            new_data->value.add(key, new_val);
            new_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
        }
    }

    void clear() {
        storage_.clear();
    }
};

} // namespace metrics

// Include TrackedOrder definition for complete type info
#include "../engine/order_state.hpp"
