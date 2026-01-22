#pragma once

#include "../aggregation/staged_metric.hpp"
#include "../aggregation/aggregation_core.hpp"
#include "../aggregation/key_extractors.hpp"
#include "../fix/fix_types.hpp"
#include <unordered_map>
#include <unordered_set>

// Forward declarations
namespace engine {
    struct TrackedOrder;
    enum class OrderState;
    enum class LimitType;
}

namespace metrics {

// ============================================================================
// OrderCountMetric - Simple order count per key
// ============================================================================
//
// A generic order counting metric that tracks the number of orders for each
// key value. The key type determines the grouping level:
//   - InstrumentSideKey: count orders per instrument-side combination
//   - InstrumentKey: count orders per instrument
//   - GlobalKey: count total orders
//
// Template parameters:
//   Key: The grouping key type (must have a KeyExtractor specialization)
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//

template<typename Key, typename... Stages>
class OrderCountMetric {
public:
    using key_type = Key;
    using value_type = int64_t;
    using provider_type = void;  // No provider needed for order counts
    using Config = aggregation::StageConfig<Stages...>;

    // ========================================================================
    // Static methods for pre-trade limit checking
    // ========================================================================

    // Returns 1 for each new order (order count contribution is always 1)
    template<typename Provider>
    static int64_t compute_order_contribution(
        const fix::NewOrderSingle& /*order*/,
        const Provider* /*provider*/) {
        return 1;
    }

    // Order count doesn't change on update, so contribution is 0
    template<typename Provider>
    static int64_t compute_update_contribution(
        const fix::OrderCancelReplaceRequest& /*update*/,
        const engine::TrackedOrder& /*existing_order*/,
        const Provider* /*provider*/) {
        return 0;
    }

    // Extract the key from a NewOrderSingle
    static Key extract_key(const fix::NewOrderSingle& order) {
        if constexpr (std::is_same_v<Key, aggregation::InstrumentSideKey>) {
            return Key{order.symbol, static_cast<int>(order.side)};
        } else if constexpr (std::is_same_v<Key, aggregation::InstrumentKey>) {
            return Key{order.symbol};
        } else if constexpr (std::is_same_v<Key, aggregation::GlobalKey>) {
            return aggregation::GlobalKey::instance();
        } else if constexpr (std::is_same_v<Key, aggregation::UnderlyerKey>) {
            return Key{order.underlyer};
        } else if constexpr (std::is_same_v<Key, aggregation::StrategyKey>) {
            return Key{order.strategy_id};
        } else if constexpr (std::is_same_v<Key, aggregation::PortfolioKey>) {
            return Key{order.portfolio_id};
        } else {
            static_assert(sizeof(Key) == 0, "Unsupported key type for OrderCountMetric");
        }
    }

    // Get the limit type for this metric
    static constexpr engine::LimitType limit_type() {
        return engine::LimitType::ORDER_COUNT;
    }

private:
    using Bucket = aggregation::AggregationBucket<Key, aggregation::CountCombiner>;
    using Storage = aggregation::StagedMetric<Bucket, Stages...>;

    Storage storage_;

public:
    // ========================================================================
    // Configuration info
    // ========================================================================

    static constexpr bool tracks_position = Config::track_position;
    static constexpr bool tracks_open = Config::track_open;
    static constexpr bool tracks_in_flight = Config::track_in_flight;

    // ========================================================================
    // InstrumentProvider interface (no-op for order counts)
    // ========================================================================

    template<typename Provider>
    void set_instrument_provider(const Provider* /*provider*/) {
        // OrderCountMetric doesn't need InstrumentProvider
    }

    // ========================================================================
    // Accessors
    // ========================================================================

    // Get count for a key (combined open + in-flight, excluding position)
    int64_t get(const Key& key) const {
        int64_t total = 0;
        if constexpr (Config::track_open) {
            total += storage_.open().get(key);
        }
        if constexpr (Config::track_in_flight) {
            total += storage_.in_flight().get(key);
        }
        return total;
    }

    // Get count including all tracked stages
    int64_t get_total(const Key& key) const {
        int64_t total = 0;
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
    std::enable_if_t<Config::track_position && std::is_void_v<Dummy>, int64_t>
    get_position(const Key& key) const {
        return storage_.position().get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Config::track_open && std::is_void_v<Dummy>, int64_t>
    get_open(const Key& key) const {
        return storage_.open().get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Config::track_in_flight && std::is_void_v<Dummy>, int64_t>
    get_in_flight(const Key& key) const {
        return storage_.in_flight().get(key);
    }

    // Access the underlying bucket for a stage
    template<typename Dummy = void>
    std::enable_if_t<Config::track_position && std::is_void_v<Dummy>, const Bucket&>
    position_bucket() const {
        return storage_.position();
    }

    template<typename Dummy = void>
    std::enable_if_t<Config::track_open && std::is_void_v<Dummy>, const Bucket&>
    open_bucket() const {
        return storage_.open();
    }

    template<typename Dummy = void>
    std::enable_if_t<Config::track_in_flight && std::is_void_v<Dummy>, const Bucket&>
    in_flight_bucket() const {
        return storage_.in_flight();
    }

    // ========================================================================
    // Generic metric interface
    // ========================================================================

    void on_order_added(const engine::TrackedOrder& order);
    void on_order_removed(const engine::TrackedOrder& order);

    void on_order_updated(const engine::TrackedOrder& /*order*/, int64_t /*old_qty*/) {
        // Order count doesn't change on quantity update
    }

    void on_partial_fill(const engine::TrackedOrder& /*order*/, int64_t /*filled_qty*/) {
        // Order count doesn't change on partial fill
    }

    void on_full_fill(const engine::TrackedOrder& /*order*/, int64_t /*filled_qty*/) {
        // Order count change handled by on_order_removed
    }

    void on_state_change(const engine::TrackedOrder& order,
                         engine::OrderState old_state,
                         engine::OrderState new_state);

    void on_order_updated_with_state_change(const engine::TrackedOrder& order,
                                             int64_t /*old_qty*/,
                                             engine::OrderState old_state,
                                             engine::OrderState new_state) {
        on_state_change(order, old_state, new_state);
    }

    void clear() {
        storage_.clear();
    }
};

// ============================================================================
// QuotedInstrumentCountMetric - Count unique quoted instruments per underlyer
// ============================================================================
//
// A specialized metric that tracks the number of unique instruments with active
// orders per underlyer. Unlike OrderCountMetric, this metric counts instruments,
// not individual orders.
//
// Example: If you have 3 orders on AAPL_OPT1 and 2 orders on AAPL_OPT2 (both
// with underlyer AAPL), the quoted instrument count for AAPL is 2, not 5.
//

template<typename... Stages>
class QuotedInstrumentCountMetric {
public:
    using key_type = aggregation::UnderlyerKey;
    using value_type = int64_t;
    using provider_type = void;  // No provider needed
    using Config = aggregation::StageConfig<Stages...>;

    // ========================================================================
    // Static methods for pre-trade limit checking
    // ========================================================================

    // Returns 1 for a new instrument on this underlyer (assumes it's new)
    // The caller must check if the instrument is already quoted
    template<typename Provider>
    static int64_t compute_order_contribution(
        const fix::NewOrderSingle& /*order*/,
        const Provider* /*provider*/) {
        // Returns 1, assuming this might be a new instrument
        // The engine must check if instrument is already quoted
        return 1;
    }

    // Quoted instrument count doesn't change on update, so contribution is 0
    template<typename Provider>
    static int64_t compute_update_contribution(
        const fix::OrderCancelReplaceRequest& /*update*/,
        const engine::TrackedOrder& /*existing_order*/,
        const Provider* /*provider*/) {
        return 0;
    }

    // Extract the key from a NewOrderSingle
    static key_type extract_key(const fix::NewOrderSingle& order) {
        return aggregation::UnderlyerKey{order.underlyer};
    }

    // Get the limit type for this metric
    static constexpr engine::LimitType limit_type() {
        return engine::LimitType::QUOTED_INSTRUMENTS;
    }

private:
    // Per-stage data structure
    struct StageData {
        // Map from underlyer to set of instruments that have orders
        std::unordered_map<std::string, std::unordered_set<std::string>> instruments_per_underlyer;
        // Count bucket
        aggregation::AggregationBucket<aggregation::UnderlyerKey, aggregation::CountCombiner> count;

        void add(const std::string& symbol, const std::string& underlyer) {
            auto& instruments = instruments_per_underlyer[underlyer];
            if (instruments.find(symbol) == instruments.end()) {
                instruments.insert(symbol);
                count.add(aggregation::UnderlyerKey{underlyer}, 1);
            }
        }

        void remove(const std::string& symbol, const std::string& underlyer) {
            auto it = instruments_per_underlyer.find(underlyer);
            if (it != instruments_per_underlyer.end()) {
                auto& instruments = it->second;
                if (instruments.erase(symbol) > 0) {
                    count.remove(aggregation::UnderlyerKey{underlyer}, 1);
                }
                if (instruments.empty()) {
                    instruments_per_underlyer.erase(it);
                }
            }
        }

        bool has_instrument(const std::string& symbol, const std::string& underlyer) const {
            auto it = instruments_per_underlyer.find(underlyer);
            if (it == instruments_per_underlyer.end()) return false;
            return it->second.find(symbol) != it->second.end();
        }

        int64_t get(const aggregation::UnderlyerKey& key) const {
            return count.get(key);
        }

        void clear() {
            instruments_per_underlyer.clear();
            count.clear();
        }
    };

    using Storage = aggregation::StagedMetric<StageData, Stages...>;
    Storage storage_;

    // Track orders per instrument-side to know when to remove from quoted count
    std::unordered_map<std::string, int> order_count_per_instrument_;

public:
    // ========================================================================
    // Configuration info
    // ========================================================================

    static constexpr bool tracks_position = Config::track_position;
    static constexpr bool tracks_open = Config::track_open;
    static constexpr bool tracks_in_flight = Config::track_in_flight;

    // ========================================================================
    // InstrumentProvider interface (no-op)
    // ========================================================================

    template<typename Provider>
    void set_instrument_provider(const Provider* /*provider*/) {}

    // ========================================================================
    // Accessors
    // ========================================================================

    int64_t get(const aggregation::UnderlyerKey& key) const {
        int64_t total = 0;
        if constexpr (Config::track_open) {
            total += storage_.open().get(key);
        }
        if constexpr (Config::track_in_flight) {
            total += storage_.in_flight().get(key);
        }
        return total;
    }

    int64_t get_total(const aggregation::UnderlyerKey& key) const {
        int64_t total = 0;
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
    std::enable_if_t<Config::track_position && std::is_void_v<Dummy>, int64_t>
    get_position(const aggregation::UnderlyerKey& key) const {
        return storage_.position().get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Config::track_open && std::is_void_v<Dummy>, int64_t>
    get_open(const aggregation::UnderlyerKey& key) const {
        return storage_.open().get(key);
    }

    template<typename Dummy = void>
    std::enable_if_t<Config::track_in_flight && std::is_void_v<Dummy>, int64_t>
    get_in_flight(const aggregation::UnderlyerKey& key) const {
        return storage_.in_flight().get(key);
    }

    // Check if instrument is quoted (has any orders) in any tracked stage
    bool is_instrument_quoted(const std::string& symbol, const std::string& underlyer) const {
        bool quoted = false;
        if constexpr (Config::track_position) {
            quoted = quoted || storage_.position().has_instrument(symbol, underlyer);
        }
        if constexpr (Config::track_open) {
            quoted = quoted || storage_.open().has_instrument(symbol, underlyer);
        }
        if constexpr (Config::track_in_flight) {
            quoted = quoted || storage_.in_flight().has_instrument(symbol, underlyer);
        }
        return quoted;
    }

    // ========================================================================
    // Generic metric interface
    // ========================================================================

    void on_order_added(const engine::TrackedOrder& order);
    void on_order_removed(const engine::TrackedOrder& order);

    void on_order_updated(const engine::TrackedOrder& /*order*/, int64_t /*old_qty*/) {
        // Quoted instrument count doesn't change on quantity update
    }

    void on_partial_fill(const engine::TrackedOrder& /*order*/, int64_t /*filled_qty*/) {
        // Quoted instrument count doesn't change on partial fill
    }

    void on_full_fill(const engine::TrackedOrder& /*order*/, int64_t /*filled_qty*/) {
        // Handled by on_order_removed
    }

    void on_state_change(const engine::TrackedOrder& order,
                         engine::OrderState old_state,
                         engine::OrderState new_state);

    void on_order_updated_with_state_change(const engine::TrackedOrder& order,
                                             int64_t /*old_qty*/,
                                             engine::OrderState old_state,
                                             engine::OrderState new_state) {
        on_state_change(order, old_state, new_state);
    }

    void clear() {
        storage_.clear();
        order_count_per_instrument_.clear();
    }
};

// ============================================================================
// Type aliases for common configurations
// ============================================================================

template<typename... Stages>
using InstrumentSideOrderCount = OrderCountMetric<aggregation::InstrumentSideKey, Stages...>;

template<typename... Stages>
using InstrumentOrderCount = OrderCountMetric<aggregation::InstrumentKey, Stages...>;

template<typename... Stages>
using GlobalOrderCount = OrderCountMetric<aggregation::GlobalKey, Stages...>;

} // namespace metrics

// Include TrackedOrder definition and implement generic interface
#include "../engine/order_state.hpp"

namespace metrics {

// ============================================================================
// OrderCountMetric implementation
// ============================================================================

template<typename Key, typename... Stages>
void OrderCountMetric<Key, Stages...>::on_order_added(const engine::TrackedOrder& order) {
    // New orders always start in IN_FLIGHT stage
    if constexpr (Config::track_in_flight) {
        if (aggregation::KeyExtractor<Key>::is_applicable(order)) {
            Key key = aggregation::KeyExtractor<Key>::extract(order);
            storage_.in_flight().add(key, 1);
        }
    }
}

template<typename Key, typename... Stages>
void OrderCountMetric<Key, Stages...>::on_order_removed(const engine::TrackedOrder& order) {
    if (!aggregation::KeyExtractor<Key>::is_applicable(order)) {
        return;
    }

    Key key = aggregation::KeyExtractor<Key>::extract(order);
    auto stage = aggregation::stage_from_order_state(order.state);
    auto* bucket = storage_.get_stage(stage);
    if (bucket) {
        bucket->remove(key, 1);
    }
}

template<typename Key, typename... Stages>
void OrderCountMetric<Key, Stages...>::on_state_change(const engine::TrackedOrder& order,
                                                        engine::OrderState old_state,
                                                        engine::OrderState new_state) {
    if (!aggregation::KeyExtractor<Key>::is_applicable(order)) {
        return;
    }

    auto old_stage = aggregation::stage_from_order_state(old_state);
    auto new_stage = aggregation::stage_from_order_state(new_state);

    if (old_stage != new_stage && aggregation::is_active_order_state(new_state)) {
        Key key = aggregation::KeyExtractor<Key>::extract(order);

        // Remove from old stage
        auto* old_bucket = storage_.get_stage(old_stage);
        if (old_bucket) {
            old_bucket->remove(key, 1);
        }

        // Add to new stage
        auto* new_bucket = storage_.get_stage(new_stage);
        if (new_bucket) {
            new_bucket->add(key, 1);
        }
    }
}

// ============================================================================
// QuotedInstrumentCountMetric implementation
// ============================================================================

template<typename... Stages>
void QuotedInstrumentCountMetric<Stages...>::on_order_added(const engine::TrackedOrder& order) {
    // New orders always start in IN_FLIGHT stage
    if constexpr (Config::track_in_flight) {
        storage_.in_flight().add(order.symbol, order.underlyer);
    }
    order_count_per_instrument_[order.symbol]++;
}

template<typename... Stages>
void QuotedInstrumentCountMetric<Stages...>::on_order_removed(const engine::TrackedOrder& order) {
    auto it = order_count_per_instrument_.find(order.symbol);
    if (it != order_count_per_instrument_.end()) {
        it->second--;
        if (it->second <= 0) {
            order_count_per_instrument_.erase(it);
            // Only remove from stage when last order on this instrument is removed
            auto stage = aggregation::stage_from_order_state(order.state);
            auto* stage_data = storage_.get_stage(stage);
            if (stage_data) {
                stage_data->remove(order.symbol, order.underlyer);
            }
        }
    }
}

template<typename... Stages>
void QuotedInstrumentCountMetric<Stages...>::on_state_change(const engine::TrackedOrder& order,
                                                              engine::OrderState old_state,
                                                              engine::OrderState new_state) {
    auto old_stage = aggregation::stage_from_order_state(old_state);
    auto new_stage = aggregation::stage_from_order_state(new_state);

    if (old_stage != new_stage && aggregation::is_active_order_state(new_state)) {
        auto* old_stage_data = storage_.get_stage(old_stage);
        auto* new_stage_data = storage_.get_stage(new_stage);

        if (old_stage_data) {
            old_stage_data->remove(order.symbol, order.underlyer);
        }
        if (new_stage_data) {
            new_stage_data->add(order.symbol, order.underlyer);
        }
    }
}

} // namespace metrics

// ============================================================================
// AccessorMixin specializations
// ============================================================================

#include "../engine/accessor_mixin.hpp"

namespace engine {

// Generic OrderCountMetric accessor mixin
template<typename Derived, typename Key, typename... Stages>
class AccessorMixin<Derived, metrics::OrderCountMetric<Key, Stages...>> {
protected:
    const metrics::OrderCountMetric<Key, Stages...>& order_count_metric_() const {
        return static_cast<const Derived*>(this)->template get_metric<metrics::OrderCountMetric<Key, Stages...>>();
    }

public:
    int64_t order_count(const Key& key) const {
        return order_count_metric_().get(key);
    }

    int64_t order_count_total(const Key& key) const {
        return order_count_metric_().get_total(key);
    }
};

// Specialization for InstrumentSideKey with convenience methods
template<typename Derived, typename... Stages>
class AccessorMixin<Derived, metrics::OrderCountMetric<aggregation::InstrumentSideKey, Stages...>> {
protected:
    const metrics::OrderCountMetric<aggregation::InstrumentSideKey, Stages...>&
    instrument_side_order_count_metric_() const {
        return static_cast<const Derived*>(this)->template
            get_metric<metrics::OrderCountMetric<aggregation::InstrumentSideKey, Stages...>>();
    }

public:
    int64_t order_count(const aggregation::InstrumentSideKey& key) const {
        return instrument_side_order_count_metric_().get(key);
    }

    int64_t bid_order_count(const std::string& symbol) const {
        return instrument_side_order_count_metric_().get(
            aggregation::InstrumentSideKey{symbol, static_cast<int>(fix::Side::BID)});
    }

    int64_t ask_order_count(const std::string& symbol) const {
        return instrument_side_order_count_metric_().get(
            aggregation::InstrumentSideKey{symbol, static_cast<int>(fix::Side::ASK)});
    }
};

// QuotedInstrumentCountMetric accessor mixin
template<typename Derived, typename... Stages>
class AccessorMixin<Derived, metrics::QuotedInstrumentCountMetric<Stages...>> {
protected:
    const metrics::QuotedInstrumentCountMetric<Stages...>& quoted_instrument_count_metric_() const {
        return static_cast<const Derived*>(this)->template
            get_metric<metrics::QuotedInstrumentCountMetric<Stages...>>();
    }

public:
    int64_t quoted_instruments_count(const std::string& underlyer) const {
        return quoted_instrument_count_metric_().get(aggregation::UnderlyerKey{underlyer});
    }

    bool is_instrument_quoted(const std::string& symbol, const std::string& underlyer) const {
        return quoted_instrument_count_metric_().is_instrument_quoted(symbol, underlyer);
    }
};

} // namespace engine
