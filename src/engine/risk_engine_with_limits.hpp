#pragma once

#include "generic_aggregation_engine.hpp"
#include "limits_config.hpp"
#include "../metrics/delta_metrics.hpp"
#include "../metrics/order_count_metrics.hpp"
#include "../metrics/notional_metrics.hpp"

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
    StringLimitStore quoted_instruments_limits_;
    StringLimitStore gross_delta_limits_;
    StringLimitStore net_delta_limits_;
    StringLimitStore strategy_notional_limits_;
    StringLimitStore portfolio_notional_limits_;

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
        quoted_instruments_limits_.reset();
        gross_delta_limits_.reset();
        net_delta_limits_.reset();
        strategy_notional_limits_.reset();
        portfolio_notional_limits_.reset();
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

    // Get current quoted instruments count
    template<typename M = void>
    std::enable_if_t<has_order_count_metric_v<Metrics...>, int64_t>
    quoted_instruments_count(const std::string& underlyer) const {
        return get_metric<OrderCountMetricType>().quoted_instruments_count(underlyer);
    }

    // Check if an instrument is already quoted (has orders)
    template<typename M = void>
    std::enable_if_t<has_order_count_metric_v<Metrics...>, bool>
    is_instrument_quoted(const std::string& symbol) const {
        const auto& m = get_metric<OrderCountMetricType>();
        return (m.bid_order_count(symbol) + m.ask_order_count(symbol)) > 0;
    }

    // Check if adding a new order on symbol/underlyer would breach the limit
    template<typename M = void>
    std::enable_if_t<has_order_count_metric_v<Metrics...>, bool>
    would_breach_quoted_instruments_limit(const std::string& underlyer,
                                          const std::string& symbol) const {
        // If instrument already has orders, adding more won't increase the count
        if (is_instrument_quoted(symbol)) {
            return false;
        }
        // Otherwise check if we're at the limit
        return quoted_instruments_limits_.at_or_above_limit(
            underlyer,
            static_cast<double>(quoted_instruments_count(underlyer)));
    }

    // Forwarded accessors for OrderCountMetrics
    template<typename M = void>
    std::enable_if_t<has_order_count_metric_v<Metrics...>, int64_t>
    bid_order_count(const std::string& symbol) const {
        return get_metric<OrderCountMetricType>().bid_order_count(symbol);
    }

    template<typename M = void>
    std::enable_if_t<has_order_count_metric_v<Metrics...>, int64_t>
    ask_order_count(const std::string& symbol) const {
        return get_metric<OrderCountMetricType>().ask_order_count(symbol);
    }

    template<typename M = void>
    std::enable_if_t<has_order_count_metric_v<Metrics...>, int64_t>
    total_order_count(const std::string& symbol) const {
        return get_metric<OrderCountMetricType>().total_order_count(symbol);
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

    // Check if adding delta_exposure would breach gross delta limit
    template<typename M = void>
    std::enable_if_t<has_delta_metric_v<Metrics...>, bool>
    would_breach_gross_delta_limit(const std::string& underlyer, double delta_exposure) const {
        double current = get_metric<DeltaMetricType>().underlyer_gross_delta(underlyer);
        return gross_delta_limits_.would_breach(underlyer, current, std::abs(delta_exposure));
    }

    // Check if adding signed_delta would breach net delta limit
    template<typename M = void>
    std::enable_if_t<has_delta_metric_v<Metrics...>, bool>
    would_breach_net_delta_limit(const std::string& underlyer, double signed_delta) const {
        double current = get_metric<DeltaMetricType>().underlyer_net_delta(underlyer);
        return net_delta_limits_.would_breach(underlyer, current, signed_delta);
    }

    // Forwarded accessors for DeltaMetrics
    template<typename M = void>
    std::enable_if_t<has_delta_metric_v<Metrics...>, double>
    global_gross_delta() const {
        return get_metric<DeltaMetricType>().global_gross_delta();
    }

    template<typename M = void>
    std::enable_if_t<has_delta_metric_v<Metrics...>, double>
    global_net_delta() const {
        return get_metric<DeltaMetricType>().global_net_delta();
    }

    template<typename M = void>
    std::enable_if_t<has_delta_metric_v<Metrics...>, double>
    underlyer_gross_delta(const std::string& underlyer) const {
        return get_metric<DeltaMetricType>().underlyer_gross_delta(underlyer);
    }

    template<typename M = void>
    std::enable_if_t<has_delta_metric_v<Metrics...>, double>
    underlyer_net_delta(const std::string& underlyer) const {
        return get_metric<DeltaMetricType>().underlyer_net_delta(underlyer);
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

    // Check if adding notional would breach strategy limit
    template<typename M = void>
    std::enable_if_t<has_notional_metric_v<Metrics...>, bool>
    would_breach_strategy_notional_limit(const std::string& strategy_id, double notional) const {
        double current = get_metric<NotionalMetricType>().strategy_notional(strategy_id);
        return strategy_notional_limits_.would_breach(strategy_id, current, notional);
    }

    // Check if adding notional would breach portfolio limit
    template<typename M = void>
    std::enable_if_t<has_notional_metric_v<Metrics...>, bool>
    would_breach_portfolio_notional_limit(const std::string& portfolio_id, double notional) const {
        double current = get_metric<NotionalMetricType>().portfolio_notional(portfolio_id);
        return portfolio_notional_limits_.would_breach(portfolio_id, current, notional);
    }

    // Forwarded accessors for NotionalMetrics
    template<typename M = void>
    std::enable_if_t<has_notional_metric_v<Metrics...>, double>
    global_notional() const {
        return get_metric<NotionalMetricType>().global_notional();
    }

    template<typename M = void>
    std::enable_if_t<has_notional_metric_v<Metrics...>, double>
    strategy_notional(const std::string& strategy_id) const {
        return get_metric<NotionalMetricType>().strategy_notional(strategy_id);
    }

    template<typename M = void>
    std::enable_if_t<has_notional_metric_v<Metrics...>, double>
    portfolio_notional(const std::string& portfolio_id) const {
        return get_metric<NotionalMetricType>().portfolio_notional(portfolio_id);
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
