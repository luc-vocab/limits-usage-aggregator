#pragma once

#include "order_state.hpp"
#include "../metrics/delta_metrics.hpp"
#include "../metrics/order_count_metrics.hpp"
#include "../metrics/notional_metrics.hpp"
#include "../fix/fix_messages.hpp"

namespace engine {

// ============================================================================
// RiskAggregationEngine - Main entry point for processing FIX messages
// ============================================================================

class RiskAggregationEngine {
private:
    OrderBook order_book_;
    metrics::DeltaMetrics delta_metrics_;
    metrics::OrderCountMetrics order_count_metrics_;
    metrics::NotionalMetrics notional_metrics_;

public:
    // ========================================================================
    // Outgoing message handlers (order sent)
    // ========================================================================

    // Process outgoing NewOrderSingle
    void on_new_order_single(const fix::NewOrderSingle& msg) {
        // Add to order book
        order_book_.add_order(msg);

        // Update metrics (order is immediately counted as pending)
        delta_metrics_.add_order(msg.underlyer, msg.delta_exposure(), msg.side);
        order_count_metrics_.add_order(msg.symbol, msg.underlyer, msg.side);
        notional_metrics_.add_order(msg.strategy_id, msg.portfolio_id, msg.notional());
    }

    // Process outgoing OrderCancelReplaceRequest
    void on_order_cancel_replace(const fix::OrderCancelReplaceRequest& msg) {
        auto* order = order_book_.get_order(msg.orig_key);
        if (!order) return;

        order_book_.start_replace(msg.orig_key, msg.key, msg.price, msg.quantity);
        // Metrics remain unchanged until we receive ack/nack
    }

    // Process outgoing OrderCancelRequest
    void on_order_cancel_request(const fix::OrderCancelRequest& msg) {
        auto* order = order_book_.get_order(msg.orig_key);
        if (!order) return;

        order_book_.start_cancel(msg.orig_key, msg.key);
        // Metrics remain unchanged until we receive ack/nack
    }

    // ========================================================================
    // Incoming message handlers (execution reports)
    // ========================================================================

    // Process incoming ExecutionReport
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

    // Process incoming OrderCancelReject
    void on_order_cancel_reject(const fix::OrderCancelReject& msg) {
        if (msg.report_type() == fix::ExecutionReportType::CANCEL_NACK) {
            order_book_.reject_cancel(msg.orig_key);
        } else {
            // UPDATE_NACK from cancel/replace
            order_book_.reject_replace(msg.orig_key);
        }
        // Metrics remain unchanged on rejection
    }

    // ========================================================================
    // Metric accessors
    // ========================================================================

    // Delta metrics
    double global_gross_delta() const { return delta_metrics_.global_gross_delta(); }
    double global_net_delta() const { return delta_metrics_.global_net_delta(); }
    double underlyer_gross_delta(const std::string& underlyer) const {
        return delta_metrics_.underlyer_gross_delta(underlyer);
    }
    double underlyer_net_delta(const std::string& underlyer) const {
        return delta_metrics_.underlyer_net_delta(underlyer);
    }

    // Order count metrics
    int64_t bid_order_count(const std::string& symbol) const {
        return order_count_metrics_.bid_order_count(symbol);
    }
    int64_t ask_order_count(const std::string& symbol) const {
        return order_count_metrics_.ask_order_count(symbol);
    }
    int64_t quoted_instruments_count(const std::string& underlyer) const {
        return order_count_metrics_.quoted_instruments_count(underlyer);
    }

    // Notional metrics
    double global_notional() const { return notional_metrics_.global_notional(); }
    double strategy_notional(const std::string& strategy_id) const {
        return notional_metrics_.strategy_notional(strategy_id);
    }
    double portfolio_notional(const std::string& portfolio_id) const {
        return notional_metrics_.portfolio_notional(portfolio_id);
    }

    // Order book access
    const OrderBook& order_book() const { return order_book_; }
    size_t active_order_count() const { return order_book_.active_orders().size(); }

    // Reset all state
    void clear() {
        order_book_.clear();
        delta_metrics_.clear();
        order_count_metrics_.clear();
        notional_metrics_.clear();
    }

private:
    void handle_insert_ack(const fix::ExecutionReport& msg) {
        order_book_.acknowledge_order(msg.key);
        // Metrics already updated on order send
    }

    void handle_insert_nack(const fix::ExecutionReport& msg) {
        auto* order = order_book_.get_order(msg.key);
        if (!order) return;

        // Remove metrics that were added on order send
        delta_metrics_.remove_order(order->underlyer, order->delta_exposure(), order->side);
        order_count_metrics_.remove_order(order->symbol, order->underlyer, order->side);
        notional_metrics_.remove_order(order->strategy_id, order->portfolio_id, order->notional());

        order_book_.reject_order(msg.key);
    }

    void handle_update_ack(const fix::ExecutionReport& msg) {
        fix::OrderKey orig_key = msg.orig_key.value_or(msg.key);
        auto* order = order_book_.get_order(orig_key);
        if (!order) return;

        auto result = order_book_.complete_replace(orig_key);
        if (result.has_value()) {
            // Get the updated order
            auto* updated_order = order_book_.resolve_order(msg.key);
            if (updated_order) {
                // Update delta metrics
                delta_metrics_.update_order(
                    updated_order->underlyer,
                    result->old_delta_exposure,
                    updated_order->delta_exposure(),
                    updated_order->side);

                // Update notional metrics
                notional_metrics_.update_order(
                    updated_order->strategy_id,
                    updated_order->portfolio_id,
                    result->old_notional,
                    updated_order->notional());
            }
        }
    }

    void handle_update_nack(const fix::ExecutionReport& msg) {
        fix::OrderKey orig_key = msg.orig_key.value_or(msg.key);
        order_book_.reject_replace(orig_key);
        // Metrics remain unchanged on rejection
    }

    void handle_cancel(const fix::ExecutionReport& msg) {
        fix::OrderKey key = msg.orig_key.value_or(msg.key);
        auto* order = order_book_.resolve_order(key);
        if (!order) return;

        // Remove metrics
        delta_metrics_.remove_order(order->underlyer, order->delta_exposure(), order->side);
        order_count_metrics_.remove_order(order->symbol, order->underlyer, order->side);
        notional_metrics_.remove_order(order->strategy_id, order->portfolio_id, order->notional());

        order_book_.complete_cancel(key);
    }

    void handle_cancel_nack(const fix::ExecutionReport& msg) {
        fix::OrderKey orig_key = msg.orig_key.value_or(msg.key);
        order_book_.reject_cancel(orig_key);
        // Metrics remain unchanged on rejection
    }

    void handle_partial_fill(const fix::ExecutionReport& msg) {
        auto* order = order_book_.resolve_order(msg.key);
        if (!order) return;

        auto result = order_book_.apply_fill(msg.key, msg.last_qty, msg.last_px);
        if (result.has_value()) {
            // Reduce metrics by filled amount
            delta_metrics_.partial_fill(order->underlyer, result->filled_delta_exposure, order->side);
            notional_metrics_.partial_fill(order->strategy_id, order->portfolio_id, result->filled_notional);
            // Order count unchanged on partial fill
        }
    }

    void handle_full_fill(const fix::ExecutionReport& msg) {
        auto* order = order_book_.resolve_order(msg.key);
        if (!order) return;

        auto result = order_book_.apply_fill(msg.key, msg.last_qty, msg.last_px);
        if (result.has_value()) {
            // Remove all remaining metrics
            delta_metrics_.remove_order(order->underlyer, result->filled_delta_exposure, order->side);
            order_count_metrics_.remove_order(order->symbol, order->underlyer, order->side);
            notional_metrics_.remove_order(order->strategy_id, order->portfolio_id, result->filled_notional);
        }
    }
};

} // namespace engine
