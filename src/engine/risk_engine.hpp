#pragma once

#include "order_state.hpp"
#include "../fix/fix_messages.hpp"
#include <tuple>
#include <type_traits>

namespace engine {

// ============================================================================
// Type traits for metric detection
// ============================================================================

template<typename T, typename... Types>
struct contains_type : std::disjunction<std::is_same<T, Types>...> {};

template<typename T, typename... Types>
inline constexpr bool contains_type_v = contains_type<T, Types...>::value;

// Forward declaration
template<typename... Metrics>
class GenericRiskAggregationEngine;

// ============================================================================
// Accessor Mixin Templates - Provide accessor methods via CRTP
// ============================================================================
//
// Primary template: no accessors by default
// Specializations for each metric type provide the relevant accessor methods
//

template<typename Derived, typename Metric>
class AccessorMixin {
    // Empty by default - no accessors for unknown metric types
};

} // namespace engine

// Include metric headers for specializations
#include "../metrics/delta_metrics.hpp"
#include "../metrics/order_count_metrics.hpp"
#include "../metrics/notional_metrics.hpp"

namespace engine {

// ============================================================================
// DeltaMetrics Accessor Mixin
// ============================================================================

template<typename Derived>
class AccessorMixin<Derived, metrics::DeltaMetrics> {
protected:
    const metrics::DeltaMetrics& delta_metrics_() const {
        return static_cast<const Derived*>(this)->template get_metric<metrics::DeltaMetrics>();
    }

public:
    double global_gross_delta() const {
        return delta_metrics_().global_gross_delta();
    }

    double global_net_delta() const {
        return delta_metrics_().global_net_delta();
    }

    double underlyer_gross_delta(const std::string& underlyer) const {
        return delta_metrics_().underlyer_gross_delta(underlyer);
    }

    double underlyer_net_delta(const std::string& underlyer) const {
        return delta_metrics_().underlyer_net_delta(underlyer);
    }

    aggregation::DeltaValue global_delta() const {
        return delta_metrics_().global_delta();
    }

    aggregation::DeltaValue underlyer_delta(const std::string& underlyer) const {
        return delta_metrics_().underlyer_delta(underlyer);
    }
};

// ============================================================================
// OrderCountMetrics Accessor Mixin
// ============================================================================

template<typename Derived>
class AccessorMixin<Derived, metrics::OrderCountMetrics> {
protected:
    const metrics::OrderCountMetrics& order_count_metrics_() const {
        return static_cast<const Derived*>(this)->template get_metric<metrics::OrderCountMetrics>();
    }

public:
    int64_t bid_order_count(const std::string& symbol) const {
        return order_count_metrics_().bid_order_count(symbol);
    }

    int64_t ask_order_count(const std::string& symbol) const {
        return order_count_metrics_().ask_order_count(symbol);
    }

    int64_t total_order_count(const std::string& symbol) const {
        return order_count_metrics_().total_order_count(symbol);
    }

    int64_t quoted_instruments_count(const std::string& underlyer) const {
        return order_count_metrics_().quoted_instruments_count(underlyer);
    }
};

// ============================================================================
// NotionalMetrics Accessor Mixin
// ============================================================================

template<typename Derived>
class AccessorMixin<Derived, metrics::NotionalMetrics> {
protected:
    const metrics::NotionalMetrics& notional_metrics_() const {
        return static_cast<const Derived*>(this)->template get_metric<metrics::NotionalMetrics>();
    }

public:
    double global_notional() const {
        return notional_metrics_().global_notional();
    }

    double strategy_notional(const std::string& strategy_id) const {
        return notional_metrics_().strategy_notional(strategy_id);
    }

    double portfolio_notional(const std::string& portfolio_id) const {
        return notional_metrics_().portfolio_notional(portfolio_id);
    }
};

// ============================================================================
// GenericRiskAggregationEngine - Template-based aggregation engine
// ============================================================================
//
// A generic engine that processes FIX messages and maintains real-time
// aggregate metrics. Metrics are specified as template parameters.
//
// By default, the engine has no metrics (empty parameter pack).
// Add metrics by specifying them as template arguments:
//
//   using MyEngine = GenericRiskAggregationEngine<DeltaMetrics, NotionalMetrics>;
//
// Accessor methods are automatically available based on which metrics are present:
//   - DeltaMetrics: global_gross_delta(), global_net_delta(), underlyer_*_delta()
//   - OrderCountMetrics: bid_order_count(), ask_order_count(), quoted_instruments_count()
//   - NotionalMetrics: global_notional(), strategy_notional(), portfolio_notional()
//
// Each metric type must implement the following interface:
//   - void on_order_added(const TrackedOrder& order)
//   - void on_order_removed(const TrackedOrder& order)
//   - void on_order_updated(const TrackedOrder& order, double old_delta, double old_notional)
//   - void on_partial_fill(const TrackedOrder& order, double filled_delta, double filled_notional)
//   - void clear()
//
// ============================================================================

template<typename... Metrics>
class GenericRiskAggregationEngine
    : public AccessorMixin<GenericRiskAggregationEngine<Metrics...>, Metrics>... {
private:
    OrderBook order_book_;
    std::tuple<Metrics...> metrics_;

    template<typename Func>
    void for_each_metric(Func&& func) {
        std::apply([&func](auto&... metrics) {
            (func(metrics), ...);
        }, metrics_);
    }

public:
    // ========================================================================
    // Metric access
    // ========================================================================

    template<typename Metric>
    Metric& get_metric() {
        static_assert(contains_type_v<Metric, Metrics...>,
                      "Metric type not found in this engine configuration");
        return std::get<Metric>(metrics_);
    }

    template<typename Metric>
    const Metric& get_metric() const {
        static_assert(contains_type_v<Metric, Metrics...>,
                      "Metric type not found in this engine configuration");
        return std::get<Metric>(metrics_);
    }

    template<typename Metric>
    static constexpr bool has_metric() {
        return contains_type_v<Metric, Metrics...>;
    }

    static constexpr size_t metric_count() {
        return sizeof...(Metrics);
    }

    // ========================================================================
    // Outgoing message handlers (order sent)
    // ========================================================================

    void on_new_order_single(const fix::NewOrderSingle& msg) {
        order_book_.add_order(msg);
        auto* order = order_book_.get_order(msg.key);
        if (order) {
            for_each_metric([order](auto& metric) {
                metric.on_order_added(*order);
            });
        }
    }

    void on_order_cancel_replace(const fix::OrderCancelReplaceRequest& msg) {
        auto* order = order_book_.get_order(msg.orig_key);
        if (!order) return;

        order_book_.start_replace(msg.orig_key, msg.key, msg.price, msg.quantity);
    }

    void on_order_cancel_request(const fix::OrderCancelRequest& msg) {
        auto* order = order_book_.get_order(msg.orig_key);
        if (!order) return;

        order_book_.start_cancel(msg.orig_key, msg.key);
    }

    // ========================================================================
    // Incoming message handlers (execution reports)
    // ========================================================================

    void on_execution_report(const fix::ExecutionReport& msg) {
        switch (msg.report_type()) {
            case fix::ExecutionReportType::INSERT_ACK:
                handle_insert_ack(msg);
                break;
            case fix::ExecutionReportType::INSERT_NACK:
                handle_insert_nack(msg);
                break;
            case fix::ExecutionReportType::UPDATE_ACK:
                handle_update_ack(msg);
                break;
            case fix::ExecutionReportType::UPDATE_NACK:
                handle_update_nack(msg);
                break;
            case fix::ExecutionReportType::CANCEL_ACK:
            case fix::ExecutionReportType::UNSOLICITED_CANCEL:
                handle_cancel(msg);
                break;
            case fix::ExecutionReportType::CANCEL_NACK:
                handle_cancel_nack(msg);
                break;
            case fix::ExecutionReportType::PARTIAL_FILL:
                handle_partial_fill(msg);
                break;
            case fix::ExecutionReportType::FULL_FILL:
                handle_full_fill(msg);
                break;
        }
    }

    void on_order_cancel_reject(const fix::OrderCancelReject& msg) {
        if (msg.report_type() == fix::ExecutionReportType::CANCEL_NACK) {
            order_book_.reject_cancel(msg.orig_key);
        } else {
            order_book_.reject_replace(msg.orig_key);
        }
    }

    // ========================================================================
    // Order book access
    // ========================================================================

    const OrderBook& order_book() const { return order_book_; }
    size_t active_order_count() const { return order_book_.active_orders().size(); }

    void clear() {
        order_book_.clear();
        for_each_metric([](auto& metric) {
            metric.clear();
        });
    }

private:
    void handle_insert_ack(const fix::ExecutionReport& msg) {
        order_book_.acknowledge_order(msg.key);
    }

    void handle_insert_nack(const fix::ExecutionReport& msg) {
        auto* order = order_book_.get_order(msg.key);
        if (!order) return;

        for_each_metric([order](auto& metric) {
            metric.on_order_removed(*order);
        });

        order_book_.reject_order(msg.key);
    }

    void handle_update_ack(const fix::ExecutionReport& msg) {
        fix::OrderKey orig_key = msg.orig_key.value_or(msg.key);
        auto* order = order_book_.get_order(orig_key);
        if (!order) return;

        auto result = order_book_.complete_replace(orig_key);
        if (result.has_value()) {
            auto* updated_order = order_book_.resolve_order(msg.key);
            if (updated_order) {
                for_each_metric([updated_order, &result](auto& metric) {
                    metric.on_order_updated(*updated_order,
                                            result->old_delta_exposure,
                                            result->old_notional);
                });
            }
        }
    }

    void handle_update_nack(const fix::ExecutionReport& msg) {
        fix::OrderKey orig_key = msg.orig_key.value_or(msg.key);
        order_book_.reject_replace(orig_key);
    }

    void handle_cancel(const fix::ExecutionReport& msg) {
        fix::OrderKey key = msg.orig_key.value_or(msg.key);
        auto* order = order_book_.resolve_order(key);
        if (!order) return;

        for_each_metric([order](auto& metric) {
            metric.on_order_removed(*order);
        });

        order_book_.complete_cancel(key);
    }

    void handle_cancel_nack(const fix::ExecutionReport& msg) {
        fix::OrderKey orig_key = msg.orig_key.value_or(msg.key);
        order_book_.reject_cancel(orig_key);
    }

    void handle_partial_fill(const fix::ExecutionReport& msg) {
        auto* order = order_book_.resolve_order(msg.key);
        if (!order) return;

        auto result = order_book_.apply_fill(msg.key, msg.last_qty, msg.last_px);
        if (result.has_value()) {
            for_each_metric([order, &result](auto& metric) {
                metric.on_partial_fill(*order,
                                       result->filled_delta_exposure,
                                       result->filled_notional);
            });
        }
    }

    void handle_full_fill(const fix::ExecutionReport& msg) {
        auto* order = order_book_.resolve_order(msg.key);
        if (!order) return;

        // Remove metrics BEFORE apply_fill updates leaves_qty to 0
        for_each_metric([order](auto& metric) {
            metric.on_order_removed(*order);
        });

        order_book_.apply_fill(msg.key, msg.last_qty, msg.last_px);
    }
};

// ============================================================================
// Type aliases for common configurations
// ============================================================================

// Standard engine with all built-in metrics
using RiskAggregationEngine = GenericRiskAggregationEngine<
    metrics::DeltaMetrics,
    metrics::OrderCountMetrics,
    metrics::NotionalMetrics
>;

// Alias for backward compatibility
using StandardRiskEngine = RiskAggregationEngine;

// Engine with only delta metrics
using DeltaOnlyEngine = GenericRiskAggregationEngine<metrics::DeltaMetrics>;

// Engine with only order count metrics
using OrderCountOnlyEngine = GenericRiskAggregationEngine<metrics::OrderCountMetrics>;

// Engine with only notional metrics
using NotionalOnlyEngine = GenericRiskAggregationEngine<metrics::NotionalMetrics>;

} // namespace engine
