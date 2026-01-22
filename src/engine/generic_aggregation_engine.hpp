#pragma once

#include "accessor_mixin.hpp"
#include "order_state.hpp"
#include "../aggregation/order_stage.hpp"
#include "../fix/fix_messages.hpp"
#include "../instrument/instrument.hpp"
#include <tuple>
#include <type_traits>

namespace engine {

// ============================================================================
// Type trait: has_set_instrument_position
// ============================================================================
//
// Detects if a metric type has a set_instrument_position(symbol, qty) method (2-param)
//

template<typename T, typename = void>
struct has_set_instrument_position : std::false_type {};

template<typename T>
struct has_set_instrument_position<T,
    std::void_t<decltype(std::declval<T>().set_instrument_position(
        std::declval<std::string>(), std::declval<int64_t>()))>
> : std::true_type {};

template<typename T>
inline constexpr bool has_set_instrument_position_v = has_set_instrument_position<T>::value;

// ============================================================================
// Type trait: has_set_instrument_position_with_instrument_and_context
// ============================================================================
//
// Detects if a metric type has a set_instrument_position(symbol, qty, instrument, context) method (4-param)
//

template<typename T, typename Instrument, typename Context, typename = void>
struct has_set_instrument_position_with_instrument_and_context : std::false_type {};

template<typename T, typename Instrument, typename Context>
struct has_set_instrument_position_with_instrument_and_context<T, Instrument, Context,
    std::void_t<decltype(std::declval<T>().set_instrument_position(
        std::declval<std::string>(), std::declval<int64_t>(),
        std::declval<Instrument>(), std::declval<Context>()))>
> : std::true_type {};

template<typename T, typename Instrument, typename Context>
inline constexpr bool has_set_instrument_position_with_instrument_and_context_v =
    has_set_instrument_position_with_instrument_and_context<T, Instrument, Context>::value;

// ============================================================================
// GenericRiskAggregationEngine - Template-based aggregation engine
// ============================================================================
//
// A generic engine that processes FIX messages and maintains real-time
// aggregate metrics. The engine is parameterized on:
//   - ContextType: Provides accessor methods for instrument data (spot_price, fx_rate, etc.)
//   - Instrument: The instrument data type (must satisfy is_instrument)
//   - Metrics...: Zero or more metric types
//
// The caller is responsible for looking up instrument data and passing it
// to the engine methods. This avoids redundant hashmap lookups.
//
// Example usage:
//
//   using Context = MyContext;
//   using Instrument = instrument::InstrumentData;
//   using MyEngine = GenericRiskAggregationEngine<Context, Instrument,
//       metrics::GrossDeltaMetric<aggregation::UnderlyerKey, Context, Instrument>,
//       metrics::GrossNotionalMetric<aggregation::GlobalKey, Context, Instrument>>;
//
//   Context ctx;
//   MyEngine engine(ctx);
//   auto inst = provider.get_instrument(order.symbol);
//   engine.on_new_order_single(order, inst);
//
// Accessor methods are automatically available based on which metrics are present,
// provided via CRTP-based AccessorMixin specializations. Each metric type should
// define its AccessorMixin specialization in its own header file.
//
// Each metric type must implement the following interface:
//   - void on_order_added(const TrackedOrder& order, const Instrument& instrument, const Context& context)
//   - void on_order_removed(const TrackedOrder& order, const Instrument& instrument, const Context& context)
//   - void on_order_updated(const TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t old_qty)
//   - void on_partial_fill(const TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty)
//   - void on_full_fill(const TrackedOrder& order, const Instrument& instrument, const Context& context, int64_t filled_qty)
//   - void on_state_change(const TrackedOrder& order, const Instrument& instrument, const Context& context, OrderState old, OrderState new)
//   - void clear()
//
// ============================================================================

template<typename ContextType, typename Instrument, typename... Metrics>
class GenericRiskAggregationEngine
    : public AccessorMixin<GenericRiskAggregationEngine<ContextType, Instrument, Metrics...>, Metrics>... {

    static_assert(instrument::is_instrument_v<Instrument>,
                  "Instrument must satisfy instrument requirements");

private:
    const ContextType& context_;
    OrderBook order_book_;
    std::tuple<Metrics...> metrics_;

    template<typename Func>
    void for_each_metric(Func&& func) {
        std::apply([&func](auto&... metrics) {
            (func(metrics), ...);
        }, metrics_);
    }

public:
    using instrument_type = Instrument;
    using context_type = ContextType;

    explicit GenericRiskAggregationEngine(const ContextType& context) : context_(context) {}

    // ========================================================================
    // Context access
    // ========================================================================

    const ContextType& context() const { return context_; }

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

    void on_new_order_single(const fix::NewOrderSingle& msg, const Instrument& instrument) {
        order_book_.add_order(msg);
        auto* order = order_book_.get_order(msg.key);
        if (order) {
            for_each_metric([order, &instrument, this](auto& metric) {
                metric.on_order_added(*order, instrument, context_);
            });
        }
    }

    void on_order_cancel_replace(const fix::OrderCancelReplaceRequest& msg, const Instrument& instrument) {
        auto* order = order_book_.get_order(msg.orig_key);
        if (!order) return;

        OrderState old_state = order->state;
        order_book_.start_replace(msg.orig_key, msg.key, msg.price, msg.quantity);
        OrderState new_state = order->state;

        if (old_state != new_state) {
            for_each_metric([order, &instrument, old_state, new_state, this](auto& metric) {
                metric.on_state_change(*order, instrument, context_, old_state, new_state);
            });
        }
    }

    void on_order_cancel_request(const fix::OrderCancelRequest& msg, const Instrument& instrument) {
        auto* order = order_book_.get_order(msg.orig_key);
        if (!order) return;

        OrderState old_state = order->state;
        order_book_.start_cancel(msg.orig_key, msg.key);
        OrderState new_state = order->state;

        if (old_state != new_state) {
            for_each_metric([order, &instrument, old_state, new_state, this](auto& metric) {
                metric.on_state_change(*order, instrument, context_, old_state, new_state);
            });
        }
    }

    // ========================================================================
    // Incoming message handlers (execution reports)
    // ========================================================================

    void on_execution_report(const fix::ExecutionReport& msg, const Instrument& instrument) {
        switch (msg.report_type()) {
            case fix::ExecutionReportType::INSERT_ACK:
                handle_insert_ack(msg, instrument);
                break;
            case fix::ExecutionReportType::INSERT_NACK:
                handle_insert_nack(msg, instrument);
                break;
            case fix::ExecutionReportType::UPDATE_ACK:
                handle_update_ack(msg, instrument);
                break;
            case fix::ExecutionReportType::UPDATE_NACK:
                handle_update_nack(msg);
                break;
            case fix::ExecutionReportType::CANCEL_ACK:
            case fix::ExecutionReportType::UNSOLICITED_CANCEL:
                handle_cancel(msg, instrument);
                break;
            case fix::ExecutionReportType::CANCEL_NACK:
                handle_cancel_nack(msg, instrument);
                break;
            case fix::ExecutionReportType::PARTIAL_FILL:
                handle_partial_fill(msg, instrument);
                break;
            case fix::ExecutionReportType::FULL_FILL:
                handle_full_fill(msg, instrument);
                break;
        }
    }

    void on_order_cancel_reject(const fix::OrderCancelReject& msg, const Instrument& instrument) {
        auto* order = order_book_.get_order(msg.orig_key);
        if (!order) return;

        OrderState old_state = order->state;
        if (msg.report_type() == fix::ExecutionReportType::CANCEL_NACK) {
            order_book_.reject_cancel(msg.orig_key);
        } else {
            order_book_.reject_replace(msg.orig_key);
        }
        OrderState new_state = order->state;

        if (old_state != new_state) {
            for_each_metric([order, &instrument, old_state, new_state, this](auto& metric) {
                metric.on_state_change(*order, instrument, context_, old_state, new_state);
            });
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

    // ========================================================================
    // Position management
    // ========================================================================

    // Set position for a specific instrument across all metrics that support it
    // Signed quantity: positive = long, negative = short
    // For gross metrics, absolute value is used internally
    void set_instrument_position(const std::string& symbol, int64_t signed_quantity, const Instrument& instrument) {
        for_each_metric([&symbol, signed_quantity, &instrument, this](auto& metric) {
            using MetricType = std::decay_t<decltype(metric)>;
            if constexpr (has_set_instrument_position_with_instrument_and_context_v<MetricType, Instrument, ContextType>) {
                metric.set_instrument_position(symbol, signed_quantity, instrument, context_);
            }
        });
    }

private:
    void handle_insert_ack(const fix::ExecutionReport& msg, const Instrument& instrument) {
        auto* order = order_book_.get_order(msg.key);
        if (!order) return;

        OrderState old_state = order->state;
        order_book_.acknowledge_order(msg.key);
        OrderState new_state = order->state;

        if (old_state != new_state) {
            for_each_metric([order, &instrument, old_state, new_state, this](auto& metric) {
                metric.on_state_change(*order, instrument, context_, old_state, new_state);
            });
        }
    }

    void handle_insert_nack(const fix::ExecutionReport& msg, const Instrument& instrument) {
        auto* order = order_book_.get_order(msg.key);
        if (!order) return;

        for_each_metric([order, &instrument, this](auto& metric) {
            metric.on_order_removed(*order, instrument, context_);
        });

        order_book_.reject_order(msg.key);
    }

    void handle_update_ack(const fix::ExecutionReport& msg, const Instrument& instrument) {
        fix::OrderKey orig_key = msg.orig_key.value_or(msg.key);
        auto* order = order_book_.get_order(orig_key);
        if (!order) return;

        // Capture old state and quantity BEFORE complete_replace updates them
        OrderState old_state = order->state;
        int64_t old_leaves_qty = order->leaves_qty;

        auto result = order_book_.complete_replace(orig_key);
        if (result.has_value()) {
            auto* updated_order = order_book_.resolve_order(msg.key);
            if (updated_order) {
                OrderState new_state = updated_order->state;

                // For stage transitions, we need to move the OLD quantity from old stage to new stage
                // Then update the quantity in the new stage
                auto old_stage = aggregation::stage_from_order_state(old_state);
                auto new_stage = aggregation::stage_from_order_state(new_state);

                if (old_stage != new_stage && aggregation::is_active_order_state(new_state)) {
                    // First: remove old_qty from old stage and add old_qty to new stage
                    // Second: update from old_qty to new_qty in new stage
                    // These can be combined: remove old_qty from old stage, add new_qty to new stage
                    for_each_metric([updated_order, &instrument, old_leaves_qty, old_state, new_state, this](auto& metric) {
                        metric.on_order_updated_with_state_change(*updated_order, instrument, context_, old_leaves_qty, old_state, new_state);
                    });
                } else {
                    // Same stage, just quantity update
                    for_each_metric([updated_order, &instrument, old_leaves_qty, this](auto& metric) {
                        metric.on_order_updated(*updated_order, instrument, context_, old_leaves_qty);
                    });
                }
            }
        }
    }

    void handle_update_nack(const fix::ExecutionReport& msg) {
        fix::OrderKey orig_key = msg.orig_key.value_or(msg.key);
        order_book_.reject_replace(orig_key);
    }

    void handle_cancel(const fix::ExecutionReport& msg, const Instrument& instrument) {
        fix::OrderKey key = msg.orig_key.value_or(msg.key);
        auto* order = order_book_.resolve_order(key);
        if (!order) return;

        for_each_metric([order, &instrument, this](auto& metric) {
            metric.on_order_removed(*order, instrument, context_);
        });

        order_book_.complete_cancel(key);
    }

    void handle_cancel_nack(const fix::ExecutionReport& msg, const Instrument& instrument) {
        fix::OrderKey orig_key = msg.orig_key.value_or(msg.key);
        auto* order = order_book_.get_order(orig_key);
        if (!order) return;

        OrderState old_state = order->state;
        order_book_.reject_cancel(orig_key);
        OrderState new_state = order->state;

        if (old_state != new_state) {
            for_each_metric([order, &instrument, old_state, new_state, this](auto& metric) {
                metric.on_state_change(*order, instrument, context_, old_state, new_state);
            });
        }
    }

    void handle_partial_fill(const fix::ExecutionReport& msg, const Instrument& instrument) {
        auto* order = order_book_.resolve_order(msg.key);
        if (!order) return;

        auto result = order_book_.apply_fill(msg.key, msg.last_qty, msg.last_px);
        if (result.has_value()) {
            int64_t filled_qty = result->filled_qty;
            for_each_metric([order, &instrument, filled_qty, this](auto& metric) {
                metric.on_partial_fill(*order, instrument, context_, filled_qty);
            });
        }
    }

    void handle_full_fill(const fix::ExecutionReport& msg, const Instrument& instrument) {
        auto* order = order_book_.resolve_order(msg.key);
        if (!order) return;

        // Get the filled quantity before removal (order->leaves_qty will be updated)
        int64_t filled_qty = msg.last_qty;

        // Remove metrics BEFORE apply_fill updates leaves_qty to 0
        for_each_metric([order, &instrument, this](auto& metric) {
            metric.on_order_removed(*order, instrument, context_);
        });

        // Credit position stage with filled quantity
        for_each_metric([order, &instrument, filled_qty, this](auto& metric) {
            metric.on_full_fill(*order, instrument, context_, filled_qty);
        });

        order_book_.apply_fill(msg.key, msg.last_qty, msg.last_px);
    }
};

// ============================================================================
// Specialization for engine with void Instrument (metrics that don't need instrument data)
// ============================================================================
//
// This specialization allows creating an engine with just metrics that don't
// need instrument data (e.g., OrderCountMetrics).
//

template<typename ContextType, typename... Metrics>
class GenericRiskAggregationEngine<ContextType, void, Metrics...>
    : public AccessorMixin<GenericRiskAggregationEngine<ContextType, void, Metrics...>, Metrics>... {

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
    using instrument_type = void;
    using context_type = ContextType;

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
    // Outgoing message handlers (order sent) - no instrument needed
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

        OrderState old_state = order->state;
        order_book_.start_replace(msg.orig_key, msg.key, msg.price, msg.quantity);
        OrderState new_state = order->state;

        if (old_state != new_state) {
            for_each_metric([order, old_state, new_state](auto& metric) {
                metric.on_state_change(*order, old_state, new_state);
            });
        }
    }

    void on_order_cancel_request(const fix::OrderCancelRequest& msg) {
        auto* order = order_book_.get_order(msg.orig_key);
        if (!order) return;

        OrderState old_state = order->state;
        order_book_.start_cancel(msg.orig_key, msg.key);
        OrderState new_state = order->state;

        if (old_state != new_state) {
            for_each_metric([order, old_state, new_state](auto& metric) {
                metric.on_state_change(*order, old_state, new_state);
            });
        }
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
        auto* order = order_book_.get_order(msg.orig_key);
        if (!order) return;

        OrderState old_state = order->state;
        if (msg.report_type() == fix::ExecutionReportType::CANCEL_NACK) {
            order_book_.reject_cancel(msg.orig_key);
        } else {
            order_book_.reject_replace(msg.orig_key);
        }
        OrderState new_state = order->state;

        if (old_state != new_state) {
            for_each_metric([order, old_state, new_state](auto& metric) {
                metric.on_state_change(*order, old_state, new_state);
            });
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

    // ========================================================================
    // Position management
    // ========================================================================

    // Set position for a specific instrument across all metrics that support it
    // Signed quantity: positive = long, negative = short
    // For gross metrics, absolute value is used internally
    void set_instrument_position(const std::string& symbol, int64_t signed_quantity) {
        for_each_metric([&symbol, signed_quantity](auto& metric) {
            using MetricType = std::decay_t<decltype(metric)>;
            if constexpr (has_set_instrument_position_v<MetricType>) {
                metric.set_instrument_position(symbol, signed_quantity);
            }
        });
    }

private:
    void handle_insert_ack(const fix::ExecutionReport& msg) {
        auto* order = order_book_.get_order(msg.key);
        if (!order) return;

        OrderState old_state = order->state;
        order_book_.acknowledge_order(msg.key);
        OrderState new_state = order->state;

        if (old_state != new_state) {
            for_each_metric([order, old_state, new_state](auto& metric) {
                metric.on_state_change(*order, old_state, new_state);
            });
        }
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

        // Capture old state and quantity BEFORE complete_replace updates them
        OrderState old_state = order->state;
        int64_t old_leaves_qty = order->leaves_qty;

        auto result = order_book_.complete_replace(orig_key);
        if (result.has_value()) {
            auto* updated_order = order_book_.resolve_order(msg.key);
            if (updated_order) {
                OrderState new_state = updated_order->state;

                // For stage transitions, we need to move the OLD quantity from old stage to new stage
                // Then update the quantity in the new stage
                auto old_stage = aggregation::stage_from_order_state(old_state);
                auto new_stage = aggregation::stage_from_order_state(new_state);

                if (old_stage != new_stage && aggregation::is_active_order_state(new_state)) {
                    // First: remove old_qty from old stage and add old_qty to new stage
                    // Second: update from old_qty to new_qty in new stage
                    // These can be combined: remove old_qty from old stage, add new_qty to new stage
                    for_each_metric([updated_order, old_leaves_qty, old_state, new_state](auto& metric) {
                        metric.on_order_updated_with_state_change(*updated_order, old_leaves_qty, old_state, new_state);
                    });
                } else {
                    // Same stage, just quantity update
                    for_each_metric([updated_order, old_leaves_qty](auto& metric) {
                        metric.on_order_updated(*updated_order, old_leaves_qty);
                    });
                }
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
        auto* order = order_book_.get_order(orig_key);
        if (!order) return;

        OrderState old_state = order->state;
        order_book_.reject_cancel(orig_key);
        OrderState new_state = order->state;

        if (old_state != new_state) {
            for_each_metric([order, old_state, new_state](auto& metric) {
                metric.on_state_change(*order, old_state, new_state);
            });
        }
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

        // Get the filled quantity before removal (order->leaves_qty will be updated)
        int64_t filled_qty = msg.last_qty;

        // Remove metrics BEFORE apply_fill updates leaves_qty to 0
        for_each_metric([order](auto& metric) {
            metric.on_order_removed(*order);
        });

        // Credit position stage with filled quantity
        for_each_metric([order, filled_qty](auto& metric) {
            metric.on_full_fill(*order, filled_qty);
        });

        order_book_.apply_fill(msg.key, msg.last_qty, msg.last_px);
    }
};

} // namespace engine
