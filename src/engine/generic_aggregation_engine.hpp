#pragma once

#include "accessor_mixin.hpp"
#include "order_state.hpp"
#include "../fix/fix_messages.hpp"
#include "../instrument/instrument.hpp"
#include <tuple>

namespace engine {

// ============================================================================
// GenericRiskAggregationEngine - Template-based aggregation engine
// ============================================================================
//
// A generic engine that processes FIX messages and maintains real-time
// aggregate metrics. The engine is parameterized on:
//   - Provider: The InstrumentProvider type (must satisfy is_instrument_provider)
//   - Metrics...: Zero or more metric types
//
// Example usage:
//
//   using Provider = instrument::StaticInstrumentProvider;
//   using MyEngine = GenericRiskAggregationEngine<Provider,
//       metrics::DeltaMetrics<Provider>,
//       metrics::NotionalMetrics<Provider>>;
//
// Accessor methods are automatically available based on which metrics are present,
// provided via CRTP-based AccessorMixin specializations. Each metric type should
// define its AccessorMixin specialization in its own header file.
//
// Each metric type must implement the following interface:
//   - void set_instrument_provider(const Provider* provider)
//   - void on_order_added(const TrackedOrder& order)
//   - void on_order_removed(const TrackedOrder& order)
//   - void on_order_updated(const TrackedOrder& order, int64_t old_qty)
//   - void on_partial_fill(const TrackedOrder& order, int64_t filled_qty)
//   - void clear()
//
// ============================================================================

template<typename Provider, typename... Metrics>
class GenericRiskAggregationEngine
    : public AccessorMixin<GenericRiskAggregationEngine<Provider, Metrics...>, Metrics>... {

    static_assert(instrument::is_instrument_provider_v<Provider>,
                  "Provider must satisfy InstrumentProvider requirements");

private:
    const Provider* provider_ = nullptr;
    OrderBook order_book_;
    std::tuple<Metrics...> metrics_;

    template<typename Func>
    void for_each_metric(Func&& func) {
        std::apply([&func](auto&... metrics) {
            (func(metrics), ...);
        }, metrics_);
    }

public:
    using provider_type = Provider;

    GenericRiskAggregationEngine() = default;

    explicit GenericRiskAggregationEngine(const Provider* provider)
        : provider_(provider) {
        // Set provider on all metrics
        for_each_metric([provider](auto& metric) {
            metric.set_instrument_provider(provider);
        });
    }

    // ========================================================================
    // InstrumentProvider access
    // ========================================================================

    void set_instrument_provider(const Provider* provider) {
        provider_ = provider;
        for_each_metric([provider](auto& metric) {
            metric.set_instrument_provider(provider);
        });
    }

    const Provider* instrument_provider() const {
        return provider_;
    }

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
                int64_t old_qty = result->old_leaves_qty;
                for_each_metric([updated_order, old_qty](auto& metric) {
                    metric.on_order_updated(*updated_order, old_qty);
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
            int64_t filled_qty = result->filled_qty;
            for_each_metric([order, filled_qty](auto& metric) {
                metric.on_partial_fill(*order, filled_qty);
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
// Specialization for engine with no provider (metrics-only, no provider needed)
// ============================================================================
//
// This specialization allows creating an engine with just metrics that don't
// need an InstrumentProvider (e.g., OrderCountMetrics).
//

template<typename... Metrics>
class GenericRiskAggregationEngine<void, Metrics...>
    : public AccessorMixin<GenericRiskAggregationEngine<void, Metrics...>, Metrics>... {

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
    using provider_type = void;

    GenericRiskAggregationEngine() = default;

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
                int64_t old_qty = result->old_leaves_qty;
                for_each_metric([updated_order, old_qty](auto& metric) {
                    metric.on_order_updated(*updated_order, old_qty);
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
            int64_t filled_qty = result->filled_qty;
            for_each_metric([order, filled_qty](auto& metric) {
                metric.on_partial_fill(*order, filled_qty);
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

} // namespace engine
