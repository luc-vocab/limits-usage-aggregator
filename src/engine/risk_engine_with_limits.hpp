#pragma once

#include "generic_aggregation_engine.hpp"
#include "limits_config.hpp"
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
// Supports limits for:
// - Quoted instruments count per underlyer
// - Gross/net delta per underlyer or global
// - Notional per strategy/portfolio
//
// Example usage:
//   using Provider = instrument::StaticInstrumentProvider;
//   using EngineWithLimits = RiskAggregationEngineWithLimits<Provider,
//       metrics::DeltaMetrics<Provider, aggregation::AllStages>,
//       metrics::OrderCountMetrics<aggregation::AllStages>>;
//
//   EngineWithLimits engine;
//   engine.set_quoted_instruments_limit("AAPL", 5);
//   if (engine.would_breach_quoted_instruments_limit("AAPL", "AAPL_OPT1")) {
//       // Reject order
//   }
//

template<typename Provider, typename... Metrics>
class RiskAggregationEngineWithLimits {
private:
    GenericRiskAggregationEngine<Provider, Metrics...> engine_;

    // Limit stores for different metrics
    StringLimitStore order_count_limits_;         // Key: "symbol:side" composite
    StringLimitStore quoted_instruments_limits_;
    StringLimitStore gross_delta_limits_;
    StringLimitStore net_delta_limits_;
    StringLimitStore strategy_notional_limits_;
    StringLimitStore portfolio_notional_limits_;
    StringLimitStore global_notional_limits_;     // Key: "" (single global limit)

    // Type aliases for finding the correct metric types
    using OrderCountMetricType = order_count_metric_t<Metrics...>;
    using DeltaMetricType = delta_metric_t<Metrics...>;
    using NotionalMetricType = notional_metric_t<Metrics...>;

public:
    using provider_type = Provider;

    RiskAggregationEngineWithLimits() = default;

    explicit RiskAggregationEngineWithLimits(const Provider* provider)
        : engine_(provider) {}

    // ========================================================================
    // Forwarding to underlying engine
    // ========================================================================

    GenericRiskAggregationEngine<Provider, Metrics...>& engine() { return engine_; }
    const GenericRiskAggregationEngine<Provider, Metrics...>& engine() const { return engine_; }

    void set_instrument_provider(const Provider* provider) {
        engine_.set_instrument_provider(provider);
    }

    const Provider* instrument_provider() const {
        return engine_.instrument_provider();
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
        order_count_limits_.reset();
        quoted_instruments_limits_.reset();
        gross_delta_limits_.reset();
        net_delta_limits_.reset();
        strategy_notional_limits_.reset();
        portfolio_notional_limits_.reset();
        global_notional_limits_.reset();
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
    // Order Count Limits (per instrument-side)
    // ========================================================================

    // Set limit for a specific instrument-side combination
    void set_order_count_limit(const std::string& symbol, fix::Side side, int64_t limit) {
        order_count_limits_.set_limit(make_order_count_key(symbol, side), static_cast<double>(limit));
    }

    // Set default limit for all instrument-side combinations
    void set_default_order_count_limit(int64_t limit) {
        order_count_limits_.set_default_limit(static_cast<double>(limit));
    }

    double get_order_count_limit(const std::string& symbol, fix::Side side) const {
        return order_count_limits_.get_limit(make_order_count_key(symbol, side));
    }

    // ========================================================================
    // Quoted Instruments Limits (requires OrderCountMetrics)
    // ========================================================================

    void set_quoted_instruments_limit(const std::string& underlyer, double limit) {
        quoted_instruments_limits_.set_limit(underlyer, limit);
    }

    void set_default_quoted_instruments_limit(double limit) {
        quoted_instruments_limits_.set_default_limit(limit);
    }

    double get_quoted_instruments_limit(const std::string& underlyer) const {
        return quoted_instruments_limits_.get_limit(underlyer);
    }


    // ========================================================================
    // Delta Limits (requires DeltaMetrics<Provider>)
    // ========================================================================

    void set_gross_delta_limit(const std::string& underlyer, double limit) {
        gross_delta_limits_.set_limit(underlyer, limit);
    }

    void set_default_gross_delta_limit(double limit) {
        gross_delta_limits_.set_default_limit(limit);
    }

    void set_gross_delta_comparison_mode(LimitComparisonMode mode) {
        gross_delta_limits_.set_comparison_mode(mode);
    }

    double get_gross_delta_limit(const std::string& underlyer) const {
        return gross_delta_limits_.get_limit(underlyer);
    }

    void set_net_delta_limit(const std::string& underlyer, double limit) {
        net_delta_limits_.set_limit(underlyer, limit);
    }

    void set_default_net_delta_limit(double limit) {
        net_delta_limits_.set_default_limit(limit);
    }

    void set_net_delta_comparison_mode(LimitComparisonMode mode) {
        net_delta_limits_.set_comparison_mode(mode);
    }

    double get_net_delta_limit(const std::string& underlyer) const {
        return net_delta_limits_.get_limit(underlyer);
    }

    // ========================================================================
    // Notional Limits (requires NotionalMetrics<Provider>)
    // ========================================================================

    void set_strategy_notional_limit(const std::string& strategy_id, double limit) {
        strategy_notional_limits_.set_limit(strategy_id, limit);
    }

    void set_default_strategy_notional_limit(double limit) {
        strategy_notional_limits_.set_default_limit(limit);
    }

    double get_strategy_notional_limit(const std::string& strategy_id) const {
        return strategy_notional_limits_.get_limit(strategy_id);
    }

    void set_portfolio_notional_limit(const std::string& portfolio_id, double limit) {
        portfolio_notional_limits_.set_limit(portfolio_id, limit);
    }

    void set_default_portfolio_notional_limit(double limit) {
        portfolio_notional_limits_.set_default_limit(limit);
    }

    double get_portfolio_notional_limit(const std::string& portfolio_id) const {
        return portfolio_notional_limits_.get_limit(portfolio_id);
    }

    void set_global_notional_limit(double limit) {
        global_notional_limits_.set_limit("", limit);
    }

    double get_global_notional_limit() const {
        return global_notional_limits_.get_limit("");
    }

    // ========================================================================
    // Unified Pre-Trade Check
    // ========================================================================

    // Check if an order would breach any configured limits
    // Returns a structured result with all breaches
    PreTradeCheckResult pre_trade_check(const fix::NewOrderSingle& order) const {
        PreTradeCheckResult result;

        // Check each limit type (SFINAE-enabled based on metrics present)
        check_order_count_limits(order, result);
        check_quoted_instruments_limits(order, result);
        check_delta_limits(order, result);
        check_notional_limits(order, result);

        return result;
    }

private:
    // Helper to create composite key for order count limits
    static std::string make_order_count_key(const std::string& symbol, fix::Side side) {
        return symbol + ":" + std::to_string(static_cast<int>(side));
    }

    // ========================================================================
    // Type traits for metric key detection
    // ========================================================================

    // Check if a metric type has InstrumentSideKey as its key_type
    template<typename T, typename = void>
    struct is_instrument_side_metric : std::false_type {};

    template<typename T>
    struct is_instrument_side_metric<T, std::void_t<typename T::key_type>>
        : std::bool_constant<std::is_same_v<typename T::key_type, aggregation::InstrumentSideKey>> {};

    template<typename T>
    static constexpr bool is_instrument_side_metric_v = is_instrument_side_metric<T>::value;

    // ========================================================================
    // Order Count Limit Check Helpers
    // ========================================================================

    // Helper to get order count from one metric (returns 0 if not applicable)
    template<typename Metric>
    int64_t get_order_count_contribution(const aggregation::InstrumentSideKey& key) const {
        if constexpr (is_order_count_metric_v<Metric> && is_instrument_side_metric_v<Metric>) {
            return engine_.template get_metric<Metric>().get(key);
        }
        return 0;
    }

    // Sum order counts across all metrics using fold expression
    int64_t total_instrument_side_order_count(const aggregation::InstrumentSideKey& key) const {
        return (get_order_count_contribution<Metrics>(key) + ...);
    }

    void check_order_count_limits(const fix::NewOrderSingle& order, PreTradeCheckResult& result) const {
        if constexpr (has_order_count_metric_v<Metrics...>) {
            std::string limit_key = make_order_count_key(order.symbol, order.side);

            // Get current count by summing from all applicable metrics
            aggregation::InstrumentSideKey count_key{order.symbol, static_cast<int>(order.side)};
            int64_t current = total_instrument_side_order_count(count_key);

            double limit = order_count_limits_.get_limit(limit_key);
            double hypothetical = static_cast<double>(current + 1);

            // Order count uses >= for breach (at_or_above_limit)
            if (hypothetical > limit) {
                result.add_breach({
                    LimitType::ORDER_COUNT,
                    limit_key,
                    limit,
                    static_cast<double>(current),
                    hypothetical
                });
            }
        }
    }

    // ========================================================================
    // Quoted Instruments Limit Check Helpers
    // ========================================================================

    // Trait to detect QuotedInstrumentCountMetric
    template<typename T>
    struct is_quoted_instrument_metric : std::false_type {};

    template<typename... Stages>
    struct is_quoted_instrument_metric<metrics::QuotedInstrumentCountMetric<Stages...>> : std::true_type {};

    template<typename T>
    static constexpr bool is_quoted_instrument_metric_v = is_quoted_instrument_metric<T>::value;

    // Check if any metric in the pack is a QuotedInstrumentCountMetric
    static constexpr bool has_quoted_instrument_metric_v = (is_quoted_instrument_metric_v<Metrics> || ...);

    // Helper to get quoted instrument count from one metric
    template<typename Metric>
    int64_t get_quoted_count_contribution(const aggregation::UnderlyerKey& key) const {
        if constexpr (is_quoted_instrument_metric_v<Metric>) {
            return engine_.template get_metric<Metric>().get(key);
        }
        return 0;
    }

    // Sum quoted instrument counts across all metrics
    int64_t total_quoted_instruments(const aggregation::UnderlyerKey& key) const {
        return (get_quoted_count_contribution<Metrics>(key) + ...);
    }

    // Check if instrument is already quoted by summing order counts for that symbol
    bool is_instrument_already_quoted(const std::string& symbol) const {
        if constexpr (has_order_count_metric_v<Metrics...>) {
            aggregation::InstrumentSideKey bid_key{symbol, static_cast<int>(fix::Side::BID)};
            aggregation::InstrumentSideKey ask_key{symbol, static_cast<int>(fix::Side::ASK)};
            return (total_instrument_side_order_count(bid_key) +
                    total_instrument_side_order_count(ask_key)) > 0;
        }
        return false;
    }

    void check_quoted_instruments_limits(const fix::NewOrderSingle& order, PreTradeCheckResult& result) const {
        if constexpr (has_quoted_instrument_metric_v) {
            // If instrument already has orders, adding more won't increase the quoted count
            if (is_instrument_already_quoted(order.symbol)) {
                return;
            }

            aggregation::UnderlyerKey underlyer_key{order.underlyer};
            int64_t current = total_quoted_instruments(underlyer_key);
            double limit = quoted_instruments_limits_.get_limit(order.underlyer);
            double hypothetical = static_cast<double>(current + 1);

            // Quoted instruments uses >= for breach
            if (hypothetical > limit) {
                result.add_breach({
                    LimitType::QUOTED_INSTRUMENTS,
                    order.underlyer,
                    limit,
                    static_cast<double>(current),
                    hypothetical
                });
            }
        }
    }

    // ========================================================================
    // Delta Limit Check Helpers
    // ========================================================================

    // Check if a metric type has UnderlyerKey as its key_type
    template<typename T, typename = void>
    struct is_underlyer_key_metric : std::false_type {};

    template<typename T>
    struct is_underlyer_key_metric<T, std::void_t<typename T::key_type>>
        : std::bool_constant<std::is_same_v<typename T::key_type, aggregation::UnderlyerKey>> {};

    template<typename T>
    static constexpr bool is_underlyer_key_metric_v = is_underlyer_key_metric<T>::value;

    // Helper to get delta from one metric (returns {0,0} if not applicable)
    template<typename Metric>
    aggregation::DeltaValue get_underlyer_delta_contribution(const aggregation::UnderlyerKey& key) const {
        if constexpr (is_delta_metric_v<Metric> && is_underlyer_key_metric_v<Metric>) {
            return engine_.template get_metric<Metric>().get(key);
        }
        return aggregation::DeltaValue{0.0, 0.0};
    }

    // Sum deltas across all metrics using fold expression
    aggregation::DeltaValue total_underlyer_delta(const aggregation::UnderlyerKey& key) const {
        aggregation::DeltaValue total{0.0, 0.0};
        ((total.gross += get_underlyer_delta_contribution<Metrics>(key).gross,
          total.net += get_underlyer_delta_contribution<Metrics>(key).net), ...);
        return total;
    }

    void check_delta_limits(const fix::NewOrderSingle& order, PreTradeCheckResult& result) const {
        if constexpr (has_delta_metric_v<Metrics...>) {
            // Compute delta exposure for this order
            const auto* provider = instrument_provider();
            if (!provider) return;

            double delta_exp = provider->get_delta(order.symbol) * static_cast<double>(order.quantity);
            double gross_add = std::abs(delta_exp);
            double net_add = (order.side == fix::Side::BID) ? delta_exp : -delta_exp;

            // Get current delta values from underlyer delta metrics
            aggregation::UnderlyerKey underlyer_key{order.underlyer};
            auto current_delta = total_underlyer_delta(underlyer_key);

            // Check gross delta limit
            double gross_limit = gross_delta_limits_.get_limit(order.underlyer);
            double hypothetical_gross = current_delta.gross + gross_add;

            if (gross_delta_limits_.would_breach(order.underlyer, current_delta.gross, gross_add)) {
                result.add_breach({
                    LimitType::GROSS_DELTA,
                    order.underlyer,
                    gross_limit,
                    current_delta.gross,
                    hypothetical_gross
                });
            }

            // Check net delta limit
            double net_limit = net_delta_limits_.get_limit(order.underlyer);
            double hypothetical_net = current_delta.net + net_add;

            if (net_delta_limits_.would_breach(order.underlyer, current_delta.net, net_add)) {
                result.add_breach({
                    LimitType::NET_DELTA,
                    order.underlyer,
                    net_limit,
                    current_delta.net,
                    hypothetical_net
                });
            }
        }
    }

    // ========================================================================
    // Notional Limit Check Helpers
    // ========================================================================

    // Check if a metric type has GlobalKey as its key_type
    template<typename T, typename = void>
    struct is_global_key_metric : std::false_type {};

    template<typename T>
    struct is_global_key_metric<T, std::void_t<typename T::key_type>>
        : std::bool_constant<std::is_same_v<typename T::key_type, aggregation::GlobalKey>> {};

    template<typename T>
    static constexpr bool is_global_key_metric_v = is_global_key_metric<T>::value;

    // Check if a metric type has StrategyKey as its key_type
    template<typename T, typename = void>
    struct is_strategy_key_metric : std::false_type {};

    template<typename T>
    struct is_strategy_key_metric<T, std::void_t<typename T::key_type>>
        : std::bool_constant<std::is_same_v<typename T::key_type, aggregation::StrategyKey>> {};

    template<typename T>
    static constexpr bool is_strategy_key_metric_v = is_strategy_key_metric<T>::value;

    // Check if a metric type has PortfolioKey as its key_type
    template<typename T, typename = void>
    struct is_portfolio_key_metric : std::false_type {};

    template<typename T>
    struct is_portfolio_key_metric<T, std::void_t<typename T::key_type>>
        : std::bool_constant<std::is_same_v<typename T::key_type, aggregation::PortfolioKey>> {};

    template<typename T>
    static constexpr bool is_portfolio_key_metric_v = is_portfolio_key_metric<T>::value;

    // Helper to get global notional from one metric
    template<typename Metric>
    double get_global_notional_contribution() const {
        if constexpr (is_notional_metric_v<Metric> && is_global_key_metric_v<Metric>) {
            return engine_.template get_metric<Metric>().get(aggregation::GlobalKey::instance());
        }
        return 0.0;
    }

    // Sum global notional across all metrics
    double total_global_notional() const {
        return (get_global_notional_contribution<Metrics>() + ...);
    }

    // Helper to get strategy notional from one metric
    template<typename Metric>
    double get_strategy_notional_contribution(const aggregation::StrategyKey& key) const {
        if constexpr (is_notional_metric_v<Metric> && is_strategy_key_metric_v<Metric>) {
            return engine_.template get_metric<Metric>().get(key);
        }
        return 0.0;
    }

    // Sum strategy notional across all metrics
    double total_strategy_notional(const aggregation::StrategyKey& key) const {
        return (get_strategy_notional_contribution<Metrics>(key) + ...);
    }

    // Helper to get portfolio notional from one metric
    template<typename Metric>
    double get_portfolio_notional_contribution(const aggregation::PortfolioKey& key) const {
        if constexpr (is_notional_metric_v<Metric> && is_portfolio_key_metric_v<Metric>) {
            return engine_.template get_metric<Metric>().get(key);
        }
        return 0.0;
    }

    // Sum portfolio notional across all metrics
    double total_portfolio_notional(const aggregation::PortfolioKey& key) const {
        return (get_portfolio_notional_contribution<Metrics>(key) + ...);
    }

    void check_notional_limits(const fix::NewOrderSingle& order, PreTradeCheckResult& result) const {
        if constexpr (has_notional_metric_v<Metrics...>) {
            // Compute notional for this order
            const auto* provider = instrument_provider();
            if (!provider) return;

            double notional = static_cast<double>(order.quantity) *
                             provider->get_spot_price(order.symbol) *
                             provider->get_contract_size(order.symbol) *
                             provider->get_fx_rate(order.symbol);

            // Check global notional limit
            double current_global = total_global_notional();
            double global_limit = global_notional_limits_.get_limit("");
            double hypothetical_global = current_global + notional;

            if (global_notional_limits_.would_breach("", current_global, notional)) {
                result.add_breach({
                    LimitType::GLOBAL_NOTIONAL,
                    "global",
                    global_limit,
                    current_global,
                    hypothetical_global
                });
            }

            // Check strategy notional limit
            if (!order.strategy_id.empty()) {
                aggregation::StrategyKey strategy_key{order.strategy_id};
                double current_strategy = total_strategy_notional(strategy_key);
                double strategy_limit = strategy_notional_limits_.get_limit(order.strategy_id);
                double hypothetical_strategy = current_strategy + notional;

                if (strategy_notional_limits_.would_breach(order.strategy_id, current_strategy, notional)) {
                    result.add_breach({
                        LimitType::STRATEGY_NOTIONAL,
                        order.strategy_id,
                        strategy_limit,
                        current_strategy,
                        hypothetical_strategy
                    });
                }
            }

            // Check portfolio notional limit
            if (!order.portfolio_id.empty()) {
                aggregation::PortfolioKey portfolio_key{order.portfolio_id};
                double current_portfolio = total_portfolio_notional(portfolio_key);
                double portfolio_limit = portfolio_notional_limits_.get_limit(order.portfolio_id);
                double hypothetical_portfolio = current_portfolio + notional;

                if (portfolio_notional_limits_.would_breach(order.portfolio_id, current_portfolio, notional)) {
                    result.add_breach({
                        LimitType::PORTFOLIO_NOTIONAL,
                        order.portfolio_id,
                        portfolio_limit,
                        current_portfolio,
                        hypothetical_portfolio
                    });
                }
            }
        }
    }
};

// ============================================================================
// Type aliases for common configurations with limits
// ============================================================================

using DefaultProvider = instrument::StaticInstrumentProvider;

// Standard engine with all metrics and limits (using AllStages)
using RiskAggregationEngineWithAllLimits = RiskAggregationEngineWithLimits<
    DefaultProvider,
    metrics::DeltaMetrics<DefaultProvider, aggregation::AllStages>,
    metrics::OrderCountMetrics<aggregation::AllStages>,
    metrics::NotionalMetrics<DefaultProvider, aggregation::AllStages>
>;

// Order count only with limits (useful for quoted instrument limits)
using OrderCountEngineWithLimits = RiskAggregationEngineWithLimits<
    DefaultProvider,
    metrics::OrderCountMetrics<aggregation::AllStages>
>;

// Template alias for custom provider types
template<typename Provider>
using RiskAggregationEngineWithAllLimitsUsing = RiskAggregationEngineWithLimits<
    Provider,
    metrics::DeltaMetrics<Provider, aggregation::AllStages>,
    metrics::OrderCountMetrics<aggregation::AllStages>,
    metrics::NotionalMetrics<Provider, aggregation::AllStages>
>;

} // namespace engine
