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
// StoredVegaInputs - Computation inputs for drift-free vega tracking
// ============================================================================
//
// When we add a vega contribution for an order, we store the inputs used
// to compute it. When removing, we use these stored inputs to compute exactly
// what was added, preventing drift when underlyer spot/vega changes.
//
struct StoredVegaInputs {
    int64_t quantity;
    double vega;
    double contract_size;
    double underlyer_spot;
    double fx_rate;
    fix::Side side;  // For gross/net computation

    double compute_exposure() const {
        return static_cast<double>(quantity) * vega * contract_size * underlyer_spot * fx_rate;
    }

    double compute_gross() const {
        return std::abs(compute_exposure());
    }

    double compute_net() const {
        double exposure = compute_exposure();
        return (side == fix::Side::BID) ? exposure : -exposure;
    }

    // For partial fills: create inputs for a portion of the stored quantity
    StoredVegaInputs with_quantity(int64_t new_qty) const {
        return StoredVegaInputs{new_qty, vega, contract_size, underlyer_spot, fx_rate, side};
    }
};

// ============================================================================
// GrossVegaMetric - Single-value metric tracking absolute vega exposure
// ============================================================================
//
// Tracks only gross (absolute) vega exposure. Designed for use with the
// generic limit checking system where each metric has one value type.
//
// Template parameters:
//   Key: The grouping key type (GlobalKey, UnderlyerKey, etc.)
//   Context: Provides instrument accessor methods (vega, contract_size, etc.)
//   Instrument: Must satisfy the vega instrument requirements (vega support)
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//

template<typename Key, typename Context, typename Instrument, typename... Stages>
class GrossVegaMetric {
    static_assert(instrument::is_vega_instrument_v<Instrument>,
                  "Instrument must satisfy vega instrument requirements (vega support)");

public:
    using key_type = Key;
    using value_type = double;
    using instrument_type = Instrument;
    using context_type = Context;
    using Config = aggregation::StageConfig<Stages...>;

    // ========================================================================
    // Static methods for pre-trade limit checking
    // ========================================================================

    // Compute the gross vega contribution for a new order
    template<typename Ctx, typename Inst>
    static double compute_order_contribution(
        const fix::NewOrderSingle& order,
        const Inst& instrument,
        const Ctx& context) {
        double vega_exp = instrument::compute_vega_exposure(context, instrument, order.quantity);
        return std::abs(vega_exp);
    }

    // Compute the gross vega contribution for an order update (new - old)
    template<typename Ctx, typename Inst>
    static double compute_update_contribution(
        const fix::OrderCancelReplaceRequest& update,
        const engine::TrackedOrder& existing_order,
        const Inst& instrument,
        const Ctx& context) {
        double old_vega = std::abs(instrument::compute_vega_exposure(
            context, instrument, existing_order.leaves_qty));
        double new_vega = std::abs(instrument::compute_vega_exposure(
            context, instrument, update.quantity));
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
        // cl_ord_id -> (key, stored_inputs) for drift-free removal
        std::unordered_map<std::string, std::pair<Key, StoredVegaInputs>> order_inputs;

        void clear() {
            gross_vega.clear();
            order_inputs.clear();
        }
    };

    using Storage = aggregation::StagedMetric<StageData, Stages...>;

    Storage storage_;

    static double compute_gross(const Context& context, const Instrument& instrument, int64_t quantity) {
        double vega_exp = instrument::compute_vega_exposure(context, instrument, quantity);
        return std::abs(vega_exp);
    }

    Key extract_order_key(const engine::TrackedOrder& order) const {
        return aggregation::KeyExtractor<Key>::extract(order);
    }

    // Capture current inputs from context for drift-free storage
    StoredVegaInputs capture_inputs(const Context& ctx, const Instrument& inst, int64_t quantity, fix::Side side) const {
        return StoredVegaInputs{
            quantity,
            ctx.vega(inst),
            ctx.contract_size(inst),
            ctx.underlyer_spot(inst),
            ctx.fx_rate(inst),
            side
        };
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

    void on_order_added(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        auto* stage_data = storage_.get_stage(aggregation::OrderStage::IN_FLIGHT);
        if (stage_data) {
            // Capture and store inputs for drift-free removal
            StoredVegaInputs inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
            double gross = inputs.compute_gross();
            stage_data->gross_vega.add(key, gross);
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
            stage_data->gross_vega.remove(key, gross);
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
            stage_data->gross_vega.remove(key, old_gross);
            stage_data->order_inputs.erase(it);
        } else {
            // Fallback for key change during replace: use old_qty with current context
            double old_gross = compute_gross(context, instrument, old_qty);
            stage_data->gross_vega.remove(key, old_gross);
        }

        // Add new contribution with current inputs
        StoredVegaInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
        double new_gross = new_inputs.compute_gross();
        stage_data->gross_vega.add(key, new_gross);
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
                StoredVegaInputs& stored = it->second.second;
                StoredVegaInputs filled_inputs = stored.with_quantity(filled_qty);
                double filled_gross = filled_inputs.compute_gross();
                open_data->gross_vega.remove(key, filled_gross);
                stored.quantity -= filled_qty;  // Update stored quantity
            }
        }

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            // Add to position with CURRENT inputs
            StoredVegaInputs pos_inputs = capture_inputs(context, instrument, filled_qty, order.side);
            double pos_gross = pos_inputs.compute_gross();
            pos_data->gross_vega.add(key, pos_gross);
        }
    }

    void on_full_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            // Add to position with CURRENT inputs
            StoredVegaInputs pos_inputs = capture_inputs(context, instrument, filled_qty, order.side);
            double filled_gross = pos_inputs.compute_gross();
            pos_data->gross_vega.add(key, filled_gross);
        }
    }

    void on_state_change(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context,
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
                old_data->gross_vega.remove(key, old_gross);
                old_data->order_inputs.erase(it);
            }
        }

        // Add to new stage with CURRENT inputs
        if (new_data) {
            StoredVegaInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
            double new_gross = new_inputs.compute_gross();
            new_data->gross_vega.add(key, new_gross);
            new_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
        }
    }

    void on_order_updated_with_state_change(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context,
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
                old_data->gross_vega.remove(key, old_gross);
                old_data->order_inputs.erase(it);
            } else {
                // Fallback for key change during replace: use old_qty with current context
                double old_gross = compute_gross(context, instrument, old_qty);
                old_data->gross_vega.remove(key, old_gross);
            }
        }

        // Add to new stage with CURRENT inputs
        if (new_data) {
            StoredVegaInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
            double new_gross = new_inputs.compute_gross();
            new_data->gross_vega.add(key, new_gross);
            new_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
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
// Template parameters:
//   Key: The grouping key type (GlobalKey, UnderlyerKey, etc.)
//   Context: Provides instrument accessor methods (vega, contract_size, etc.)
//   Instrument: Must satisfy the vega instrument requirements (vega support)
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//

template<typename Key, typename Context, typename Instrument, typename... Stages>
class NetVegaMetric {
    static_assert(instrument::is_vega_instrument_v<Instrument>,
                  "Instrument must satisfy vega instrument requirements (vega support)");

public:
    using key_type = Key;
    using value_type = double;
    using instrument_type = Instrument;
    using context_type = Context;
    using Config = aggregation::StageConfig<Stages...>;

    // ========================================================================
    // Static methods for pre-trade limit checking
    // ========================================================================

    // Compute the net vega contribution for a new order
    template<typename Ctx, typename Inst>
    static double compute_order_contribution(
        const fix::NewOrderSingle& order,
        const Inst& instrument,
        const Ctx& context) {
        double vega_exp = instrument::compute_vega_exposure(context, instrument, order.quantity);
        return (order.side == fix::Side::BID) ? vega_exp : -vega_exp;
    }

    // Compute the net vega contribution for an order update (new - old)
    template<typename Ctx, typename Inst>
    static double compute_update_contribution(
        const fix::OrderCancelReplaceRequest& update,
        const engine::TrackedOrder& existing_order,
        const Inst& instrument,
        const Ctx& context) {
        double old_vega = instrument::compute_vega_exposure(
            context, instrument, existing_order.leaves_qty);
        double old_net = (existing_order.side == fix::Side::BID) ? old_vega : -old_vega;

        double new_vega = instrument::compute_vega_exposure(
            context, instrument, update.quantity);
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
        // cl_ord_id -> (key, stored_inputs) for drift-free removal
        std::unordered_map<std::string, std::pair<Key, StoredVegaInputs>> order_inputs;

        void clear() {
            net_vega.clear();
            order_inputs.clear();
        }
    };

    using Storage = aggregation::StagedMetric<StageData, Stages...>;

    Storage storage_;

    static double compute_net(const Context& context, const Instrument& instrument, int64_t quantity, fix::Side side) {
        double vega_exp = instrument::compute_vega_exposure(context, instrument, quantity);
        return (side == fix::Side::BID) ? vega_exp : -vega_exp;
    }

    Key extract_order_key(const engine::TrackedOrder& order) const {
        return aggregation::KeyExtractor<Key>::extract(order);
    }

    // Capture current inputs from context for drift-free storage
    StoredVegaInputs capture_inputs(const Context& ctx, const Instrument& inst, int64_t quantity, fix::Side side) const {
        return StoredVegaInputs{
            quantity,
            ctx.vega(inst),
            ctx.contract_size(inst),
            ctx.underlyer_spot(inst),
            ctx.fx_rate(inst),
            side
        };
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

    void on_order_added(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);
        auto* stage_data = storage_.get_stage(aggregation::OrderStage::IN_FLIGHT);
        if (stage_data) {
            // Capture and store inputs for drift-free removal
            StoredVegaInputs inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
            double net = inputs.compute_net();
            stage_data->net_vega.add(key, net);
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
            stage_data->net_vega.remove(key, net);
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
            stage_data->net_vega.remove(key, old_net);
            stage_data->order_inputs.erase(it);
        } else {
            // Fallback for key change during replace: use old_qty with current context
            double old_net = compute_net(context, instrument, old_qty, order.side);
            stage_data->net_vega.remove(key, old_net);
        }

        // Add new contribution with current inputs
        StoredVegaInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
        double new_net = new_inputs.compute_net();
        stage_data->net_vega.add(key, new_net);
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
                StoredVegaInputs& stored = it->second.second;
                StoredVegaInputs filled_inputs = stored.with_quantity(filled_qty);
                double filled_net = filled_inputs.compute_net();
                open_data->net_vega.remove(key, filled_net);
                stored.quantity -= filled_qty;  // Update stored quantity
            }
        }

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            // Add to position with CURRENT inputs
            StoredVegaInputs pos_inputs = capture_inputs(context, instrument, filled_qty, order.side);
            double pos_net = pos_inputs.compute_net();
            pos_data->net_vega.add(key, pos_net);
        }
    }

    void on_full_fill(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty) {
        if (!aggregation::KeyExtractor<Key>::is_applicable(order)) return;
        Key key = extract_order_key(order);

        auto* pos_data = storage_.get_stage(aggregation::OrderStage::POSITION);
        if (pos_data) {
            // Add to position with CURRENT inputs
            StoredVegaInputs pos_inputs = capture_inputs(context, instrument, filled_qty, order.side);
            double filled_net = pos_inputs.compute_net();
            pos_data->net_vega.add(key, filled_net);
        }
    }

    void on_state_change(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context,
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
                old_data->net_vega.remove(key, old_net);
                old_data->order_inputs.erase(it);
            }
        }

        // Add to new stage with CURRENT inputs
        if (new_data) {
            StoredVegaInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
            double new_net = new_inputs.compute_net();
            new_data->net_vega.add(key, new_net);
            new_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
        }
    }

    void on_order_updated_with_state_change(const engine::TrackedOrder& order, const Instrument& instrument, const Context& context,
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
                old_data->net_vega.remove(key, old_net);
                old_data->order_inputs.erase(it);
            } else {
                // Fallback for key change during replace: use old_qty with current context
                double old_net = compute_net(context, instrument, old_qty, order.side);
                old_data->net_vega.remove(key, old_net);
            }
        }

        // Add to new stage with CURRENT inputs
        if (new_data) {
            StoredVegaInputs new_inputs = capture_inputs(context, instrument, order.leaves_qty, order.side);
            double new_net = new_inputs.compute_net();
            new_data->net_vega.add(key, new_net);
            new_data->order_inputs[order.key.cl_ord_id] = {key, new_inputs};
        }
    }

    void clear() {
        storage_.clear();
    }
};

// ============================================================================
// Type aliases for Gross/Net vega metrics
// ============================================================================

template<typename Context, typename Instrument, typename... Stages>
using GlobalGrossVegaMetric = GrossVegaMetric<aggregation::GlobalKey, Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using UnderlyerGrossVegaMetric = GrossVegaMetric<aggregation::UnderlyerKey, Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using GlobalNetVegaMetric = NetVegaMetric<aggregation::GlobalKey, Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using UnderlyerNetVegaMetric = NetVegaMetric<aggregation::UnderlyerKey, Context, Instrument, Stages...>;

} // namespace metrics

// Include TrackedOrder definition for complete type info
#include "../engine/order_state.hpp"
