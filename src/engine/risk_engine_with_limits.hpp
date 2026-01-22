#pragma once

#include "generic_aggregation_engine.hpp"
#include "limits_config.hpp"
#include "metric_limit_store.hpp"
#include "pre_trade_check.hpp"
#include "../metrics/delta_metric.hpp"
#include "../metrics/order_count_metric.hpp"
#include "../metrics/notional_metric.hpp"

namespace engine {

// ============================================================================
// RiskAggregationEngineWithLimits - Engine wrapper with limit checking
// ============================================================================
//
// Wraps a GenericRiskAggregationEngine and adds runtime limit configuration
// and breach checking capabilities.
//
// Template parameters:
//   - ContextType: Provides accessor methods for instrument data (spot_price, fx_rate, etc.)
//   - Instrument: The Instrument type (use void for metrics that don't need instrument data)
//   - Metrics...: Zero or more metric types
//
// Each metric type must have:
//   - key_type: The key type for the limit store
//   - value_type: The value type tracked by the metric
//   - static compute_order_contribution(order, instrument, context): contribution for limit check
//   - static extract_key(order): extract key from order
//   - static limit_type(): the LimitType enum value
//
// Generic API:
//   engine.set_limit<MetricType>(key, limit);
//   engine.set_default_limit<MetricType>(limit);
//   engine.get_limit<MetricType>(key);
//   engine.get_limit_store<MetricType>();
//
// Example usage:
//   using Context = MyContext;
//   using Instrument = instrument::InstrumentData;
//   using GrossDelta = metrics::GrossDeltaMetric<aggregation::UnderlyerKey, Context, Instrument, aggregation::AllStages>;
//   using OrderCount = metrics::OrderCountMetric<aggregation::InstrumentSideKey, aggregation::AllStages>;
//
//   using EngineWithLimits = RiskAggregationEngineWithLimits<Context, Instrument, GrossDelta, OrderCount>;
//
//   Context ctx;
//   EngineWithLimits engine(ctx);
//   engine.set_limit<GrossDelta>(aggregation::UnderlyerKey{"AAPL"}, 10000.0);
//   engine.set_default_limit<OrderCount>(50.0);
//   auto instrument = provider.get_instrument(order.symbol);
//   auto result = engine.pre_trade_check(order, instrument);
//

template<typename ContextType, typename Instrument, typename... Metrics>
class RiskAggregationEngineWithLimits {
private:
    GenericRiskAggregationEngine<ContextType, Instrument, Metrics...> engine_;
    MetricLimitStores<Metrics...> limits_;

public:
    using instrument_type = Instrument;
    using context_type = ContextType;

    explicit RiskAggregationEngineWithLimits(const ContextType& context) : engine_(context) {}

    // ========================================================================
    // Context access
    // ========================================================================

    const ContextType& context() const { return engine_.context(); }

    // ========================================================================
    // Forwarding to underlying engine
    // ========================================================================

    GenericRiskAggregationEngine<ContextType, Instrument, Metrics...>& engine() { return engine_; }
    const GenericRiskAggregationEngine<ContextType, Instrument, Metrics...>& engine() const { return engine_; }

    // Forward all message handlers - caller provides instrument
    void on_new_order_single(const fix::NewOrderSingle& msg, const Instrument& instrument) {
        engine_.on_new_order_single(msg, instrument);
    }

    void on_order_cancel_replace(const fix::OrderCancelReplaceRequest& msg, const Instrument& instrument) {
        engine_.on_order_cancel_replace(msg, instrument);
    }

    void on_order_cancel_request(const fix::OrderCancelRequest& msg, const Instrument& instrument) {
        engine_.on_order_cancel_request(msg, instrument);
    }

    void on_execution_report(const fix::ExecutionReport& msg, const Instrument& instrument) {
        engine_.on_execution_report(msg, instrument);
    }

    void on_order_cancel_reject(const fix::OrderCancelReject& msg, const Instrument& instrument) {
        engine_.on_order_cancel_reject(msg, instrument);
    }

    // Forward order book access
    const OrderBook& order_book() const { return engine_.order_book(); }
    size_t active_order_count() const { return engine_.active_order_count(); }

    void clear() {
        engine_.clear();
        clear_all_limits();
    }

    void clear_all_limits() {
        limits_.reset();
    }

    // ========================================================================
    // Position management
    // ========================================================================

    // Set position for a specific instrument across all metrics that support it
    // Signed quantity: positive = long, negative = short
    void set_instrument_position(const std::string& symbol, int64_t signed_quantity, const Instrument& instrument) {
        engine_.set_instrument_position(symbol, signed_quantity, instrument);
    }

    // ========================================================================
    // Metric access (forwarded from underlying engine via CRTP mixins)
    // ========================================================================

    template<typename Metric>
    Metric& get_metric() {
        return engine_.template get_metric<Metric>();
    }

    template<typename Metric>
    const Metric& get_metric() const {
        return engine_.template get_metric<Metric>();
    }

    template<typename Metric>
    static constexpr bool has_metric() {
        return GenericRiskAggregationEngine<ContextType, Instrument, Metrics...>::template has_metric<Metric>();
    }

    // ========================================================================
    // Generic Limit API
    // ========================================================================

    // Set limit for a specific metric and key
    template<typename Metric>
    void set_limit(const typename Metric::key_type& key, double limit) {
        limits_.template get<Metric>().set_limit(key, limit);
    }

    // Set default limit for a specific metric
    template<typename Metric>
    void set_default_limit(double limit) {
        limits_.template get<Metric>().set_default_limit(limit);
    }

    // Get limit for a specific metric and key
    template<typename Metric>
    double get_limit(const typename Metric::key_type& key) const {
        return limits_.template get<Metric>().get_limit(key);
    }

    // Get the limit store for a specific metric
    template<typename Metric>
    LimitStore<typename Metric::key_type>& get_limit_store() {
        return limits_.template get<Metric>();
    }

    template<typename Metric>
    const LimitStore<typename Metric::key_type>& get_limit_store() const {
        return limits_.template get<Metric>();
    }

    // ========================================================================
    // Pre-Trade Check
    // ========================================================================

    // Check if a new order would breach any configured limits
    // Returns a structured result with all breaches
    PreTradeCheckResult pre_trade_check(const fix::NewOrderSingle& order, const Instrument& instrument) const {
        PreTradeCheckResult result;
        check_all_limits<Metrics...>(order, instrument, result);
        return result;
    }

    // Check if an order update would breach any configured limits
    // Returns a structured result with all breaches
    PreTradeCheckResult pre_trade_check(const fix::OrderCancelReplaceRequest& update, const Instrument& instrument) const {
        PreTradeCheckResult result;

        // Look up the existing order
        const TrackedOrder* existing = engine_.order_book().get_order(update.orig_key);
        if (!existing) {
            // Order not found - can't check limits, but this isn't a breach
            return result;
        }

        check_all_update_limits<Metrics...>(update, *existing, instrument, result);
        return result;
    }

    // Check if a new order would breach a specific metric's limit
    template<typename Metric>
    PreTradeCheckResult pre_trade_check_single(const fix::NewOrderSingle& order, const Instrument& instrument) const {
        PreTradeCheckResult result;
        if constexpr (is_quoted_instrument_metric<Metric>::value) {
            check_quoted_instrument_limit<Metric>(order, instrument, result);
        } else {
            check_standard_limit<Metric>(order, instrument, result);
        }
        return result;
    }

    // Check if an order update would breach a specific metric's limit
    template<typename Metric>
    PreTradeCheckResult pre_trade_check_single(const fix::OrderCancelReplaceRequest& update, const Instrument& instrument) const {
        PreTradeCheckResult result;

        const TrackedOrder* existing = engine_.order_book().get_order(update.orig_key);
        if (!existing) {
            return result;
        }

        check_standard_update_limit<Metric>(update, *existing, instrument, result);
        return result;
    }

private:
    // ========================================================================
    // Metric type detection traits
    // ========================================================================

    template<typename T>
    struct is_quoted_instrument_metric : std::false_type {};

    template<typename... Stages>
    struct is_quoted_instrument_metric<metrics::QuotedInstrumentCountMetric<Stages...>> : std::true_type {};

    // ========================================================================
    // Pre-Trade Check Implementation
    // ========================================================================

    template<typename First, typename... Rest>
    void check_all_limits(const fix::NewOrderSingle& order, const Instrument& instrument, PreTradeCheckResult& result) const {
        check_metric_limit<First>(order, instrument, result);
        if constexpr (sizeof...(Rest) > 0) {
            check_all_limits<Rest...>(order, instrument, result);
        }
    }

    // Base case for empty pack
    void check_all_limits(const fix::NewOrderSingle&, const Instrument&, PreTradeCheckResult&) const {}

    // Recursive helper for order update limit checking
    template<typename First, typename... Rest>
    void check_all_update_limits(const fix::OrderCancelReplaceRequest& update,
                                 const TrackedOrder& existing,
                                 const Instrument& instrument,
                                 PreTradeCheckResult& result) const {
        check_standard_update_limit<First>(update, existing, instrument, result);
        if constexpr (sizeof...(Rest) > 0) {
            check_all_update_limits<Rest...>(update, existing, instrument, result);
        }
    }

    // Base case for empty pack
    void check_all_update_limits(const fix::OrderCancelReplaceRequest&,
                                 const TrackedOrder&,
                                 const Instrument&,
                                 PreTradeCheckResult&) const {}

    // Check limit for a single metric
    template<typename Metric>
    void check_metric_limit(const fix::NewOrderSingle& order, const Instrument& instrument, PreTradeCheckResult& result) const {
        // Special handling for QuotedInstrumentCountMetric
        if constexpr (is_quoted_instrument_metric<Metric>::value) {
            check_quoted_instrument_limit<Metric>(order, instrument, result);
        } else {
            check_standard_limit<Metric>(order, instrument, result);
        }
    }

    // Standard limit check for metrics with compute_order_contribution
    template<typename Metric>
    void check_standard_limit(const fix::NewOrderSingle& order, const Instrument& instrument, PreTradeCheckResult& result) const {
        auto key = Metric::extract_key(order);
        auto contribution = Metric::compute_order_contribution(order, instrument, engine_.context());
        auto current = static_cast<double>(engine_.template get_metric<Metric>().get(key));

        const auto& store = limits_.template get<Metric>();
        double limit = store.get_limit(key);
        double hypothetical = current + static_cast<double>(contribution);

        if (store.would_breach(key, current, static_cast<double>(contribution))) {
            result.add_breach({
                Metric::limit_type(),
                detail::key_to_string(key),
                limit,
                current,
                hypothetical
            });
        }
    }

    // Standard limit check for order updates with compute_update_contribution
    template<typename Metric>
    void check_standard_update_limit(const fix::OrderCancelReplaceRequest& update,
                                     const TrackedOrder& existing,
                                     const Instrument& instrument,
                                     PreTradeCheckResult& result) const {
        // Extract key from the existing order (not the update request)
        auto key = extract_key_from_tracked_order<Metric>(existing);
        auto contribution = Metric::compute_update_contribution(update, existing, instrument, engine_.context());

        // Skip if contribution is zero (e.g., order count doesn't change on update)
        if (contribution == 0) {
            return;
        }

        auto current = static_cast<double>(engine_.template get_metric<Metric>().get(key));

        const auto& store = limits_.template get<Metric>();
        double limit = store.get_limit(key);
        double hypothetical = current + static_cast<double>(contribution);

        if (store.would_breach(key, current, static_cast<double>(contribution))) {
            result.add_breach({
                Metric::limit_type(),
                detail::key_to_string(key),
                limit,
                current,
                hypothetical
            });
        }
    }

    // Helper to extract metric key from a TrackedOrder
    template<typename Metric>
    typename Metric::key_type extract_key_from_tracked_order(const TrackedOrder& order) const {
        using Key = typename Metric::key_type;
        if constexpr (std::is_same_v<Key, aggregation::GlobalKey>) {
            return aggregation::GlobalKey::instance();
        } else if constexpr (std::is_same_v<Key, aggregation::UnderlyerKey>) {
            return Key{order.underlyer};
        } else if constexpr (std::is_same_v<Key, aggregation::InstrumentKey>) {
            return Key{order.symbol};
        } else if constexpr (std::is_same_v<Key, aggregation::InstrumentSideKey>) {
            return Key{order.symbol, static_cast<int>(order.side)};
        } else if constexpr (std::is_same_v<Key, aggregation::StrategyKey>) {
            return Key{order.strategy_id};
        } else if constexpr (std::is_same_v<Key, aggregation::PortfolioKey>) {
            return Key{order.portfolio_id};
        } else {
            static_assert(sizeof(Key) == 0, "Unsupported key type");
        }
    }

    // Special limit check for QuotedInstrumentCountMetric
    template<typename Metric>
    void check_quoted_instrument_limit(const fix::NewOrderSingle& order, const Instrument& instrument, PreTradeCheckResult& result) const {
        // Check if instrument already has orders - if so, won't increase quoted count
        if (is_instrument_already_quoted(order.symbol)) {
            return;
        }

        auto key = Metric::extract_key(order);
        auto contribution = Metric::compute_order_contribution(order, instrument);
        auto current = static_cast<double>(engine_.template get_metric<Metric>().get(key));

        const auto& store = limits_.template get<Metric>();
        double limit = store.get_limit(key);
        double hypothetical = current + static_cast<double>(contribution);

        if (store.would_breach(key, current, static_cast<double>(contribution))) {
            result.add_breach({
                Metric::limit_type(),
                detail::key_to_string(key),
                limit,
                current,
                hypothetical
            });
        }
    }

    // Check if instrument is already quoted by checking order counts
    bool is_instrument_already_quoted(const std::string& symbol) const {
        return is_instrument_quoted_impl<Metrics...>(symbol);
    }

    template<typename First, typename... Rest>
    bool is_instrument_quoted_impl(const std::string& symbol) const {
        if constexpr (std::is_same_v<typename First::key_type, aggregation::InstrumentSideKey>) {
            aggregation::InstrumentSideKey bid_key{symbol, static_cast<int>(fix::Side::BID)};
            aggregation::InstrumentSideKey ask_key{symbol, static_cast<int>(fix::Side::ASK)};
            auto count = engine_.template get_metric<First>().get(bid_key) +
                        engine_.template get_metric<First>().get(ask_key);
            if (count > 0) return true;
        }
        if constexpr (sizeof...(Rest) > 0) {
            return is_instrument_quoted_impl<Rest...>(symbol);
        }
        return false;
    }

    bool is_instrument_quoted_impl(const std::string&) const {
        return false;
    }
};

// ============================================================================
// Specialization for engine with void Instrument (metrics that don't need instrument data)
// ============================================================================

template<typename ContextType, typename... Metrics>
class RiskAggregationEngineWithLimits<ContextType, void, Metrics...> {
private:
    GenericRiskAggregationEngine<ContextType, void, Metrics...> engine_;
    MetricLimitStores<Metrics...> limits_;

public:
    using instrument_type = void;
    using context_type = ContextType;

    RiskAggregationEngineWithLimits() = default;

    // ========================================================================
    // Forwarding to underlying engine
    // ========================================================================

    GenericRiskAggregationEngine<ContextType, void, Metrics...>& engine() { return engine_; }
    const GenericRiskAggregationEngine<ContextType, void, Metrics...>& engine() const { return engine_; }

    // Forward all message handlers - no instrument needed
    void on_new_order_single(const fix::NewOrderSingle& msg) {
        engine_.on_new_order_single(msg);
    }

    void on_order_cancel_replace(const fix::OrderCancelReplaceRequest& msg) {
        engine_.on_order_cancel_replace(msg);
    }

    void on_order_cancel_request(const fix::OrderCancelRequest& msg) {
        engine_.on_order_cancel_request(msg);
    }

    void on_execution_report(const fix::ExecutionReport& msg) {
        engine_.on_execution_report(msg);
    }

    void on_order_cancel_reject(const fix::OrderCancelReject& msg) {
        engine_.on_order_cancel_reject(msg);
    }

    // Forward order book access
    const OrderBook& order_book() const { return engine_.order_book(); }
    size_t active_order_count() const { return engine_.active_order_count(); }

    void clear() {
        engine_.clear();
        clear_all_limits();
    }

    void clear_all_limits() {
        limits_.reset();
    }

    // ========================================================================
    // Position management
    // ========================================================================

    void set_instrument_position(const std::string& symbol, int64_t signed_quantity) {
        engine_.set_instrument_position(symbol, signed_quantity);
    }

    // ========================================================================
    // Metric access
    // ========================================================================

    template<typename Metric>
    Metric& get_metric() {
        return engine_.template get_metric<Metric>();
    }

    template<typename Metric>
    const Metric& get_metric() const {
        return engine_.template get_metric<Metric>();
    }

    template<typename Metric>
    static constexpr bool has_metric() {
        return GenericRiskAggregationEngine<ContextType, void, Metrics...>::template has_metric<Metric>();
    }

    // ========================================================================
    // Generic Limit API
    // ========================================================================

    template<typename Metric>
    void set_limit(const typename Metric::key_type& key, double limit) {
        limits_.template get<Metric>().set_limit(key, limit);
    }

    template<typename Metric>
    void set_default_limit(double limit) {
        limits_.template get<Metric>().set_default_limit(limit);
    }

    template<typename Metric>
    double get_limit(const typename Metric::key_type& key) const {
        return limits_.template get<Metric>().get_limit(key);
    }

    template<typename Metric>
    LimitStore<typename Metric::key_type>& get_limit_store() {
        return limits_.template get<Metric>();
    }

    template<typename Metric>
    const LimitStore<typename Metric::key_type>& get_limit_store() const {
        return limits_.template get<Metric>();
    }

    // ========================================================================
    // Pre-Trade Check (for metrics that don't need instrument)
    // ========================================================================

    PreTradeCheckResult pre_trade_check(const fix::NewOrderSingle& order) const {
        PreTradeCheckResult result;
        check_all_limits<Metrics...>(order, result);
        return result;
    }

    // Pre-trade check for order updates
    PreTradeCheckResult pre_trade_check(const fix::OrderCancelReplaceRequest& update) const {
        PreTradeCheckResult result;
        auto* existing = engine_.order_book().get_order(update.orig_key);
        if (!existing) {
            return result;
        }
        check_update_limits<Metrics...>(update, *existing, result);
        return result;
    }

private:
    template<typename First, typename... Rest>
    void check_update_limits(const fix::OrderCancelReplaceRequest& update,
                             const TrackedOrder& existing,
                             PreTradeCheckResult& result) const {
        check_update_metric_limit<First>(update, existing, result);
        if constexpr (sizeof...(Rest) > 0) {
            check_update_limits<Rest...>(update, existing, result);
        }
    }

    void check_update_limits(const fix::OrderCancelReplaceRequest&,
                             const TrackedOrder&,
                             PreTradeCheckResult&) const {}

    template<typename Metric>
    void check_update_metric_limit(const fix::OrderCancelReplaceRequest& update,
                                   const TrackedOrder& existing,
                                   PreTradeCheckResult& result) const {
        // Extract key from existing order, not update request
        auto key = aggregation::KeyExtractor<typename Metric::key_type>::extract(existing);
        auto contribution = Metric::compute_update_contribution(update, existing);
        auto current = static_cast<double>(engine_.template get_metric<Metric>().get(key));

        const auto& store = limits_.template get<Metric>();
        double limit = store.get_limit(key);
        double hypothetical = current + static_cast<double>(contribution);

        if (store.would_breach(key, current, static_cast<double>(contribution))) {
            LimitBreachInfo breach;
            breach.type = Metric::limit_type();
            breach.key = detail::key_to_string(key);
            breach.limit_value = limit;
            breach.current_usage = current;
            breach.hypothetical_usage = hypothetical;
            result.add_breach(breach);
        }
    }


    template<typename T>
    struct is_quoted_instrument_metric : std::false_type {};

    template<typename... Stages>
    struct is_quoted_instrument_metric<metrics::QuotedInstrumentCountMetric<Stages...>> : std::true_type {};

    template<typename First, typename... Rest>
    void check_all_limits(const fix::NewOrderSingle& order, PreTradeCheckResult& result) const {
        check_metric_limit<First>(order, result);
        if constexpr (sizeof...(Rest) > 0) {
            check_all_limits<Rest...>(order, result);
        }
    }

    void check_all_limits(const fix::NewOrderSingle&, PreTradeCheckResult&) const {}

    template<typename Metric>
    void check_metric_limit(const fix::NewOrderSingle& order, PreTradeCheckResult& result) const {
        if constexpr (is_quoted_instrument_metric<Metric>::value) {
            check_quoted_instrument_limit<Metric>(order, result);
        } else {
            check_standard_limit<Metric>(order, result);
        }
    }

    template<typename Metric>
    void check_standard_limit(const fix::NewOrderSingle& order, PreTradeCheckResult& result) const {
        auto key = Metric::extract_key(order);
        auto contribution = Metric::compute_order_contribution(order);
        auto current = static_cast<double>(engine_.template get_metric<Metric>().get(key));

        const auto& store = limits_.template get<Metric>();
        double limit = store.get_limit(key);
        double hypothetical = current + static_cast<double>(contribution);

        if (store.would_breach(key, current, static_cast<double>(contribution))) {
            result.add_breach({
                Metric::limit_type(),
                detail::key_to_string(key),
                limit,
                current,
                hypothetical
            });
        }
    }

    template<typename Metric>
    void check_quoted_instrument_limit(const fix::NewOrderSingle& order, PreTradeCheckResult& result) const {
        if (is_instrument_already_quoted(order.symbol)) {
            return;
        }

        auto key = Metric::extract_key(order);
        auto contribution = Metric::compute_order_contribution(order);
        auto current = static_cast<double>(engine_.template get_metric<Metric>().get(key));

        const auto& store = limits_.template get<Metric>();
        double limit = store.get_limit(key);
        double hypothetical = current + static_cast<double>(contribution);

        if (store.would_breach(key, current, static_cast<double>(contribution))) {
            result.add_breach({
                Metric::limit_type(),
                detail::key_to_string(key),
                limit,
                current,
                hypothetical
            });
        }
    }

    bool is_instrument_already_quoted(const std::string& symbol) const {
        return is_instrument_quoted_impl<Metrics...>(symbol);
    }

    template<typename First, typename... Rest>
    bool is_instrument_quoted_impl(const std::string& symbol) const {
        if constexpr (std::is_same_v<typename First::key_type, aggregation::InstrumentSideKey>) {
            aggregation::InstrumentSideKey bid_key{symbol, static_cast<int>(fix::Side::BID)};
            aggregation::InstrumentSideKey ask_key{symbol, static_cast<int>(fix::Side::ASK)};
            auto count = engine_.template get_metric<First>().get(bid_key) +
                        engine_.template get_metric<First>().get(ask_key);
            if (count > 0) return true;
        }
        if constexpr (sizeof...(Rest) > 0) {
            return is_instrument_quoted_impl<Rest...>(symbol);
        }
        return false;
    }

    bool is_instrument_quoted_impl(const std::string&) const {
        return false;
    }
};


} // namespace engine
