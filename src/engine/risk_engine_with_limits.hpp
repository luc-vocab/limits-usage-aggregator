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
//   - Provider: The InstrumentProvider type
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
//   using EngineWithLimits = RiskAggregationEngineWithLimits<Provider,
//       metrics::GrossDeltaMetric<aggregation::UnderlyerKey, Provider, aggregation::AllStages>,
//       metrics::NetDeltaMetric<aggregation::UnderlyerKey, Provider, aggregation::AllStages>,
//       metrics::OrderCountMetric<aggregation::InstrumentSideKey, aggregation::AllStages>>;
//
//   EngineWithLimits engine(&provider);
//   engine.set_limit<metrics::GrossDeltaMetric<aggregation::UnderlyerKey, Provider, aggregation::AllStages>>(
//       aggregation::UnderlyerKey{"AAPL"}, 10000.0);
//   auto result = engine.pre_trade_check(order);
//

template<typename Provider, typename... Metrics>
class RiskAggregationEngineWithLimits {
private:
    GenericRiskAggregationEngine<Provider, Metrics...> engine_;
    MetricLimitStores<Metrics...> limits_;

    // Type aliases for finding the correct metric types
    using OrderCountMetricType = order_count_metric_t<Metrics...>;
    using DeltaMetricType = delta_metric_t<Metrics...>;
    using NotionalMetricType = notional_metric_t<Metrics...>;

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
    // Backward-compatible Limit API (delegates to generic API)
    // ========================================================================

    // Order Count Limits (per instrument-side)
    void set_order_count_limit(const std::string& symbol, fix::Side side, int64_t limit) {
        set_order_count_limit_internal(
            aggregation::InstrumentSideKey{symbol, static_cast<int>(side)},
            static_cast<double>(limit));
    }

    void set_default_order_count_limit(int64_t limit) {
        set_default_order_count_limit_internal(static_cast<double>(limit));
    }

    double get_order_count_limit(const std::string& symbol, fix::Side side) const {
        return get_order_count_limit_internal(
            aggregation::InstrumentSideKey{symbol, static_cast<int>(side)});
    }

    // Quoted Instruments Limits
    void set_quoted_instruments_limit(const std::string& underlyer, double limit) {
        set_quoted_instruments_limit_internal(aggregation::UnderlyerKey{underlyer}, limit);
    }

    void set_default_quoted_instruments_limit(double limit) {
        set_default_quoted_instruments_limit_internal(limit);
    }

    double get_quoted_instruments_limit(const std::string& underlyer) const {
        return get_quoted_instruments_limit_internal(aggregation::UnderlyerKey{underlyer});
    }

    // Gross Delta Limits
    void set_gross_delta_limit(const std::string& underlyer, double limit) {
        set_gross_delta_limit_internal(aggregation::UnderlyerKey{underlyer}, limit);
    }

    void set_default_gross_delta_limit(double limit) {
        set_default_gross_delta_limit_internal(limit);
    }

    double get_gross_delta_limit(const std::string& underlyer) const {
        return get_gross_delta_limit_internal(aggregation::UnderlyerKey{underlyer});
    }

    // Net Delta Limits
    void set_net_delta_limit(const std::string& underlyer, double limit) {
        set_net_delta_limit_internal(aggregation::UnderlyerKey{underlyer}, limit);
    }

    void set_default_net_delta_limit(double limit) {
        set_default_net_delta_limit_internal(limit);
    }

    double get_net_delta_limit(const std::string& underlyer) const {
        return get_net_delta_limit_internal(aggregation::UnderlyerKey{underlyer});
    }

    // Strategy Notional Limits
    void set_strategy_notional_limit(const std::string& strategy_id, double limit) {
        set_strategy_notional_limit_internal(aggregation::StrategyKey{strategy_id}, limit);
    }

    void set_default_strategy_notional_limit(double limit) {
        set_default_strategy_notional_limit_internal(limit);
    }

    double get_strategy_notional_limit(const std::string& strategy_id) const {
        return get_strategy_notional_limit_internal(aggregation::StrategyKey{strategy_id});
    }

    // Portfolio Notional Limits
    void set_portfolio_notional_limit(const std::string& portfolio_id, double limit) {
        set_portfolio_notional_limit_internal(aggregation::PortfolioKey{portfolio_id}, limit);
    }

    void set_default_portfolio_notional_limit(double limit) {
        set_default_portfolio_notional_limit_internal(limit);
    }

    double get_portfolio_notional_limit(const std::string& portfolio_id) const {
        return get_portfolio_notional_limit_internal(aggregation::PortfolioKey{portfolio_id});
    }

    // Global Notional Limits
    void set_global_notional_limit(double limit) {
        set_global_notional_limit_internal(limit);
    }

    double get_global_notional_limit() const {
        return get_global_notional_limit_internal();
    }

    // ========================================================================
    // Unified Pre-Trade Check
    // ========================================================================

    // Check if an order would breach any configured limits
    // Returns a structured result with all breaches
    PreTradeCheckResult pre_trade_check(const fix::NewOrderSingle& order) const {
        PreTradeCheckResult result;
        check_all_limits<Metrics...>(order, result);
        return result;
    }

private:
    // ========================================================================
    // Internal limit setters/getters for backward compatibility
    // ========================================================================

    // Order count limits - find the InstrumentSideKey metric
    template<typename Metric = void>
    void set_order_count_limit_internal(const aggregation::InstrumentSideKey& key, double limit) {
        set_limit_for_key_type<aggregation::InstrumentSideKey, Metrics...>(key, limit);
    }

    template<typename Metric = void>
    void set_default_order_count_limit_internal(double limit) {
        set_default_limit_for_key_type<aggregation::InstrumentSideKey, Metrics...>(limit);
    }

    template<typename Metric = void>
    double get_order_count_limit_internal(const aggregation::InstrumentSideKey& key) const {
        return get_limit_for_key_type<aggregation::InstrumentSideKey, Metrics...>(key);
    }

    // Quoted instruments limits - find the UnderlyerKey metric (QuotedInstrumentCountMetric)
    template<typename Metric = void>
    void set_quoted_instruments_limit_internal(const aggregation::UnderlyerKey& key, double limit) {
        set_quoted_instrument_limit_impl<Metrics...>(key, limit);
    }

    template<typename Metric = void>
    void set_default_quoted_instruments_limit_internal(double limit) {
        set_default_quoted_instrument_limit_impl<Metrics...>(limit);
    }

    template<typename Metric = void>
    double get_quoted_instruments_limit_internal(const aggregation::UnderlyerKey& key) const {
        return get_quoted_instrument_limit_impl<Metrics...>(key);
    }

    // Gross delta limits
    template<typename Metric = void>
    void set_gross_delta_limit_internal(const aggregation::UnderlyerKey& key, double limit) {
        set_gross_delta_limit_impl<Metrics...>(key, limit);
    }

    template<typename Metric = void>
    void set_default_gross_delta_limit_internal(double limit) {
        set_default_gross_delta_limit_impl<Metrics...>(limit);
    }

    template<typename Metric = void>
    double get_gross_delta_limit_internal(const aggregation::UnderlyerKey& key) const {
        return get_gross_delta_limit_impl<Metrics...>(key);
    }

    // Net delta limits
    template<typename Metric = void>
    void set_net_delta_limit_internal(const aggregation::UnderlyerKey& key, double limit) {
        set_net_delta_limit_impl<Metrics...>(key, limit);
    }

    template<typename Metric = void>
    void set_default_net_delta_limit_internal(double limit) {
        set_default_net_delta_limit_impl<Metrics...>(limit);
    }

    template<typename Metric = void>
    double get_net_delta_limit_internal(const aggregation::UnderlyerKey& key) const {
        return get_net_delta_limit_impl<Metrics...>(key);
    }

    // Strategy notional limits
    template<typename Metric = void>
    void set_strategy_notional_limit_internal(const aggregation::StrategyKey& key, double limit) {
        set_limit_for_key_type<aggregation::StrategyKey, Metrics...>(key, limit);
    }

    template<typename Metric = void>
    void set_default_strategy_notional_limit_internal(double limit) {
        set_default_limit_for_key_type<aggregation::StrategyKey, Metrics...>(limit);
    }

    template<typename Metric = void>
    double get_strategy_notional_limit_internal(const aggregation::StrategyKey& key) const {
        return get_limit_for_key_type<aggregation::StrategyKey, Metrics...>(key);
    }

    // Portfolio notional limits
    template<typename Metric = void>
    void set_portfolio_notional_limit_internal(const aggregation::PortfolioKey& key, double limit) {
        set_limit_for_key_type<aggregation::PortfolioKey, Metrics...>(key, limit);
    }

    template<typename Metric = void>
    void set_default_portfolio_notional_limit_internal(double limit) {
        set_default_limit_for_key_type<aggregation::PortfolioKey, Metrics...>(limit);
    }

    template<typename Metric = void>
    double get_portfolio_notional_limit_internal(const aggregation::PortfolioKey& key) const {
        return get_limit_for_key_type<aggregation::PortfolioKey, Metrics...>(key);
    }

    // Global notional limits
    template<typename Metric = void>
    void set_global_notional_limit_internal(double limit) {
        set_limit_for_key_type<aggregation::GlobalKey, Metrics...>(aggregation::GlobalKey::instance(), limit);
    }

    template<typename Metric = void>
    double get_global_notional_limit_internal() const {
        return get_limit_for_key_type<aggregation::GlobalKey, Metrics...>(aggregation::GlobalKey::instance());
    }

    // ========================================================================
    // Generic limit access by key type
    // ========================================================================

    // Set limit for ALL metrics with matching key type
    template<typename KeyType, typename First, typename... Rest>
    void set_limit_for_key_type(const KeyType& key, double limit) {
        if constexpr (std::is_same_v<typename First::key_type, KeyType>) {
            limits_.template get<First>().set_limit(key, limit);
        }
        if constexpr (sizeof...(Rest) > 0) {
            set_limit_for_key_type<KeyType, Rest...>(key, limit);
        }
    }

    template<typename KeyType>
    void set_limit_for_key_type(const KeyType&, double) {
        // No matching metric - no-op
    }

    // Set default limit for ALL metrics with matching key type
    template<typename KeyType, typename First, typename... Rest>
    void set_default_limit_for_key_type(double limit) {
        if constexpr (std::is_same_v<typename First::key_type, KeyType>) {
            limits_.template get<First>().set_default_limit(limit);
        }
        if constexpr (sizeof...(Rest) > 0) {
            set_default_limit_for_key_type<KeyType, Rest...>(limit);
        }
    }

    template<typename KeyType>
    void set_default_limit_for_key_type(double) {
        // No matching metric - no-op
    }

    // Get limit for the first metric with matching key type
    template<typename KeyType, typename First, typename... Rest>
    double get_limit_for_key_type(const KeyType& key) const {
        if constexpr (std::is_same_v<typename First::key_type, KeyType>) {
            return limits_.template get<First>().get_limit(key);
        } else if constexpr (sizeof...(Rest) > 0) {
            return get_limit_for_key_type<KeyType, Rest...>(key);
        } else {
            return std::numeric_limits<double>::max();
        }
    }

    template<typename KeyType>
    double get_limit_for_key_type(const KeyType&) const {
        return std::numeric_limits<double>::max();
    }

    // ========================================================================
    // Special implementations for metrics with specific traits
    // ========================================================================

    // Quoted instrument metric detection
    template<typename T>
    struct is_quoted_instrument_metric : std::false_type {};

    template<typename... Stages>
    struct is_quoted_instrument_metric<metrics::QuotedInstrumentCountMetric<Stages...>> : std::true_type {};

    template<typename First, typename... Rest>
    void set_quoted_instrument_limit_impl(const aggregation::UnderlyerKey& key, double limit) {
        if constexpr (is_quoted_instrument_metric<First>::value) {
            limits_.template get<First>().set_limit(key, limit);
        } else if constexpr (sizeof...(Rest) > 0) {
            set_quoted_instrument_limit_impl<Rest...>(key, limit);
        }
    }

    void set_quoted_instrument_limit_impl(const aggregation::UnderlyerKey&, double) {}

    template<typename First, typename... Rest>
    void set_default_quoted_instrument_limit_impl(double limit) {
        if constexpr (is_quoted_instrument_metric<First>::value) {
            limits_.template get<First>().set_default_limit(limit);
        } else if constexpr (sizeof...(Rest) > 0) {
            set_default_quoted_instrument_limit_impl<Rest...>(limit);
        }
    }

    void set_default_quoted_instrument_limit_impl(double) {}

    template<typename First, typename... Rest>
    double get_quoted_instrument_limit_impl(const aggregation::UnderlyerKey& key) const {
        if constexpr (is_quoted_instrument_metric<First>::value) {
            return limits_.template get<First>().get_limit(key);
        } else if constexpr (sizeof...(Rest) > 0) {
            return get_quoted_instrument_limit_impl<Rest...>(key);
        } else {
            return std::numeric_limits<double>::max();
        }
    }

    double get_quoted_instrument_limit_impl(const aggregation::UnderlyerKey&) const {
        return std::numeric_limits<double>::max();
    }

    // Gross delta metric detection
    template<typename T>
    struct is_gross_delta_metric : std::false_type {};

    template<typename Key, typename P, typename... Stages>
    struct is_gross_delta_metric<metrics::GrossDeltaMetric<Key, P, Stages...>> : std::true_type {};

    template<typename First, typename... Rest>
    void set_gross_delta_limit_impl(const aggregation::UnderlyerKey& key, double limit) {
        if constexpr (is_gross_delta_metric<First>::value &&
                      std::is_same_v<typename First::key_type, aggregation::UnderlyerKey>) {
            limits_.template get<First>().set_limit(key, limit);
        } else if constexpr (sizeof...(Rest) > 0) {
            set_gross_delta_limit_impl<Rest...>(key, limit);
        }
    }

    void set_gross_delta_limit_impl(const aggregation::UnderlyerKey&, double) {}

    template<typename First, typename... Rest>
    void set_default_gross_delta_limit_impl(double limit) {
        if constexpr (is_gross_delta_metric<First>::value &&
                      std::is_same_v<typename First::key_type, aggregation::UnderlyerKey>) {
            limits_.template get<First>().set_default_limit(limit);
        } else if constexpr (sizeof...(Rest) > 0) {
            set_default_gross_delta_limit_impl<Rest...>(limit);
        }
    }

    void set_default_gross_delta_limit_impl(double) {}

    template<typename First, typename... Rest>
    double get_gross_delta_limit_impl(const aggregation::UnderlyerKey& key) const {
        if constexpr (is_gross_delta_metric<First>::value &&
                      std::is_same_v<typename First::key_type, aggregation::UnderlyerKey>) {
            return limits_.template get<First>().get_limit(key);
        } else if constexpr (sizeof...(Rest) > 0) {
            return get_gross_delta_limit_impl<Rest...>(key);
        } else {
            return std::numeric_limits<double>::max();
        }
    }

    double get_gross_delta_limit_impl(const aggregation::UnderlyerKey&) const {
        return std::numeric_limits<double>::max();
    }

    // Net delta metric detection
    template<typename T>
    struct is_net_delta_metric : std::false_type {};

    template<typename Key, typename P, typename... Stages>
    struct is_net_delta_metric<metrics::NetDeltaMetric<Key, P, Stages...>> : std::true_type {};

    template<typename First, typename... Rest>
    void set_net_delta_limit_impl(const aggregation::UnderlyerKey& key, double limit) {
        if constexpr (is_net_delta_metric<First>::value &&
                      std::is_same_v<typename First::key_type, aggregation::UnderlyerKey>) {
            limits_.template get<First>().set_limit(key, limit);
        } else if constexpr (sizeof...(Rest) > 0) {
            set_net_delta_limit_impl<Rest...>(key, limit);
        }
    }

    void set_net_delta_limit_impl(const aggregation::UnderlyerKey&, double) {}

    template<typename First, typename... Rest>
    void set_default_net_delta_limit_impl(double limit) {
        if constexpr (is_net_delta_metric<First>::value &&
                      std::is_same_v<typename First::key_type, aggregation::UnderlyerKey>) {
            limits_.template get<First>().set_default_limit(limit);
        } else if constexpr (sizeof...(Rest) > 0) {
            set_default_net_delta_limit_impl<Rest...>(limit);
        }
    }

    void set_default_net_delta_limit_impl(double) {}

    template<typename First, typename... Rest>
    double get_net_delta_limit_impl(const aggregation::UnderlyerKey& key) const {
        if constexpr (is_net_delta_metric<First>::value &&
                      std::is_same_v<typename First::key_type, aggregation::UnderlyerKey>) {
            return limits_.template get<First>().get_limit(key);
        } else if constexpr (sizeof...(Rest) > 0) {
            return get_net_delta_limit_impl<Rest...>(key);
        } else {
            return std::numeric_limits<double>::max();
        }
    }

    double get_net_delta_limit_impl(const aggregation::UnderlyerKey&) const {
        return std::numeric_limits<double>::max();
    }

    // ========================================================================
    // Generic Pre-Trade Check Implementation
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
    template<typename P = Provider>
    static constexpr std::enable_if_t<!std::is_void_v<P>, const P*>
    get_provider_ptr_impl(const P* p) { return p; }

    template<typename P = Provider>
    static constexpr std::enable_if_t<std::is_void_v<P>, const void*>
    get_provider_ptr_impl(std::nullptr_t) { return nullptr; }

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
