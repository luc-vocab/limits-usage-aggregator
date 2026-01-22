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
//   - Provider: The InstrumentProvider type (use void for metrics that don't need a provider)
//   - Metrics...: Zero or more metric types
//
// Each metric type must have:
//   - key_type: The key type for the limit store
//   - value_type: The value type tracked by the metric
//   - static compute_order_contribution(order, provider): contribution for limit check
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
//   using Provider = instrument::StaticInstrumentProvider;
//   using GrossDelta = metrics::GrossDeltaMetric<aggregation::UnderlyerKey, Provider, aggregation::AllStages>;
//   using OrderCount = metrics::OrderCountMetric<aggregation::InstrumentSideKey, aggregation::AllStages>;
//
//   using EngineWithLimits = RiskAggregationEngineWithLimits<Provider, GrossDelta, OrderCount>;
//
//   EngineWithLimits engine(&provider);
//   engine.set_limit<GrossDelta>(aggregation::UnderlyerKey{"AAPL"}, 10000.0);
//   engine.set_default_limit<OrderCount>(50.0);
//   auto result = engine.pre_trade_check(order);
//

template<typename Provider, typename... Metrics>
class RiskAggregationEngineWithLimits {
private:
    GenericRiskAggregationEngine<Provider, Metrics...> engine_;
    MetricLimitStores<Metrics...> limits_;

public:
    using provider_type = Provider;

    RiskAggregationEngineWithLimits() = default;

    template<typename P = Provider, typename = std::enable_if_t<!std::is_void_v<P>>>
    explicit RiskAggregationEngineWithLimits(const P* provider)
        : engine_(provider) {}

    // ========================================================================
    // Forwarding to underlying engine
    // ========================================================================

    GenericRiskAggregationEngine<Provider, Metrics...>& engine() { return engine_; }
    const GenericRiskAggregationEngine<Provider, Metrics...>& engine() const { return engine_; }

    template<typename P = Provider>
    std::enable_if_t<!std::is_void_v<P>, void>
    set_instrument_provider(const P* provider) {
        engine_.set_instrument_provider(provider);
    }

    template<typename P = Provider>
    std::enable_if_t<!std::is_void_v<P>, const P*>
    instrument_provider() const {
        return engine_.instrument_provider();
    }

    // For void provider - return nullptr
    template<typename P = Provider>
    std::enable_if_t<std::is_void_v<P>, std::nullptr_t>
    instrument_provider() const {
        return nullptr;
    }

    // Forward all message handlers
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

    // Set position for a specific instrument across all metrics that support it
    // Signed quantity: positive = long, negative = short
    void set_instrument_position(const std::string& symbol, int64_t signed_quantity) {
        engine_.set_instrument_position(symbol, signed_quantity);
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
        return GenericRiskAggregationEngine<Provider, Metrics...>::template has_metric<Metric>();
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
    PreTradeCheckResult pre_trade_check(const fix::NewOrderSingle& order) const {
        PreTradeCheckResult result;
        check_all_limits<Metrics...>(order, result);
        return result;
    }

    // Check if an order update would breach any configured limits
    // Returns a structured result with all breaches
    PreTradeCheckResult pre_trade_check(const fix::OrderCancelReplaceRequest& update) const {
        PreTradeCheckResult result;

        // Look up the existing order
        const TrackedOrder* existing = engine_.order_book().get_order(update.orig_key);
        if (!existing) {
            // Order not found - can't check limits, but this isn't a breach
            return result;
        }

        check_all_update_limits<Metrics...>(update, *existing, result);
        return result;
    }

    // Check if a new order would breach a specific metric's limit
    template<typename Metric>
    PreTradeCheckResult pre_trade_check_single(const fix::NewOrderSingle& order) const {
        PreTradeCheckResult result;
        if constexpr (is_quoted_instrument_metric<Metric>::value) {
            check_quoted_instrument_limit<Metric>(order, result);
        } else {
            check_standard_limit<Metric>(order, result);
        }
        return result;
    }

    // Check if an order update would breach a specific metric's limit
    template<typename Metric>
    PreTradeCheckResult pre_trade_check_single(const fix::OrderCancelReplaceRequest& update) const {
        PreTradeCheckResult result;

        const TrackedOrder* existing = engine_.order_book().get_order(update.orig_key);
        if (!existing) {
            return result;
        }

        check_standard_update_limit<Metric>(update, *existing, result);
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
    void check_all_limits(const fix::NewOrderSingle& order, PreTradeCheckResult& result) const {
        check_metric_limit<First>(order, result);
        if constexpr (sizeof...(Rest) > 0) {
            check_all_limits<Rest...>(order, result);
        }
    }

    // Base case for empty pack
    void check_all_limits(const fix::NewOrderSingle&, PreTradeCheckResult&) const {}

    // Recursive helper for order update limit checking
    template<typename First, typename... Rest>
    void check_all_update_limits(const fix::OrderCancelReplaceRequest& update,
                                 const TrackedOrder& existing,
                                 PreTradeCheckResult& result) const {
        check_standard_update_limit<First>(update, existing, result);
        if constexpr (sizeof...(Rest) > 0) {
            check_all_update_limits<Rest...>(update, existing, result);
        }
    }

    // Base case for empty pack
    void check_all_update_limits(const fix::OrderCancelReplaceRequest&,
                                 const TrackedOrder&,
                                 PreTradeCheckResult&) const {}

    // Check limit for a single metric
    template<typename Metric>
    void check_metric_limit(const fix::NewOrderSingle& order, PreTradeCheckResult& result) const {
        // Special handling for QuotedInstrumentCountMetric
        if constexpr (is_quoted_instrument_metric<Metric>::value) {
            check_quoted_instrument_limit<Metric>(order, result);
        } else {
            check_standard_limit<Metric>(order, result);
        }
    }

    // Helper to get provider pointer with correct type
    auto get_provider_ptr() const {
        if constexpr (std::is_void_v<Provider>) {
            return static_cast<const void*>(nullptr);
        } else {
            return instrument_provider();
        }
    }

    // Standard limit check for metrics with compute_order_contribution
    template<typename Metric>
    void check_standard_limit(const fix::NewOrderSingle& order, PreTradeCheckResult& result) const {
        auto key = Metric::extract_key(order);
        auto contribution = Metric::compute_order_contribution(order, get_provider_ptr());
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
                                     PreTradeCheckResult& result) const {
        // Extract key from the existing order (not the update request)
        auto key = extract_key_from_tracked_order<Metric>(existing);
        auto contribution = Metric::compute_update_contribution(update, existing, get_provider_ptr());

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
    void check_quoted_instrument_limit(const fix::NewOrderSingle& order, PreTradeCheckResult& result) const {
        // Check if instrument already has orders - if so, won't increase quoted count
        if (is_instrument_already_quoted(order.symbol)) {
            return;
        }

        auto key = Metric::extract_key(order);
        auto contribution = Metric::compute_order_contribution(order, get_provider_ptr());
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
// Type aliases for common configurations with limits
// ============================================================================

using DefaultProvider = instrument::StaticInstrumentProvider;

// Template alias for custom provider types with new metric structure
template<typename Provider>
using RiskAggregationEngineWithAllLimitsUsing = RiskAggregationEngineWithLimits<
    Provider,
    metrics::GrossDeltaMetric<aggregation::UnderlyerKey, Provider, aggregation::AllStages>,
    metrics::NetDeltaMetric<aggregation::UnderlyerKey, Provider, aggregation::AllStages>,
    metrics::OrderCountMetric<aggregation::InstrumentSideKey, aggregation::AllStages>,
    metrics::QuotedInstrumentCountMetric<aggregation::AllStages>,
    metrics::NotionalMetric<aggregation::GlobalKey, Provider, aggregation::AllStages>,
    metrics::NotionalMetric<aggregation::StrategyKey, Provider, aggregation::AllStages>,
    metrics::NotionalMetric<aggregation::PortfolioKey, Provider, aggregation::AllStages>
>;

// Standard engine with all metrics and limits (using AllStages)
using RiskAggregationEngineWithAllLimits = RiskAggregationEngineWithAllLimitsUsing<DefaultProvider>;

// Order count only with limits (useful for quoted instrument limits)
using OrderCountEngineWithLimits = RiskAggregationEngineWithLimits<
    void,
    metrics::OrderCountMetric<aggregation::InstrumentSideKey, aggregation::AllStages>,
    metrics::QuotedInstrumentCountMetric<aggregation::AllStages>
>;

} // namespace engine
