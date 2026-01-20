#pragma once

#include "../fix/fix_messages.hpp"
#include <unordered_map>
#include <optional>

namespace engine {

// Order lifecycle states for internal tracking
enum class OrderState {
    PENDING_NEW,       // NewOrderSingle sent, awaiting ack
    OPEN,              // Order acknowledged and live
    PENDING_REPLACE,   // Replace request sent, awaiting ack
    PENDING_CANCEL,    // Cancel request sent, awaiting ack
    FILLED,            // Completely filled
    CANCELED,          // Canceled (solicited or unsolicited)
    REJECTED           // Rejected
};

inline const char* to_string(OrderState state) {
    switch (state) {
        case OrderState::PENDING_NEW: return "PENDING_NEW";
        case OrderState::OPEN: return "OPEN";
        case OrderState::PENDING_REPLACE: return "PENDING_REPLACE";
        case OrderState::PENDING_CANCEL: return "PENDING_CANCEL";
        case OrderState::FILLED: return "FILLED";
        case OrderState::CANCELED: return "CANCELED";
        case OrderState::REJECTED: return "REJECTED";
        default: return "UNKNOWN";
    }
}

// Tracked order information
struct TrackedOrder {
    fix::OrderKey key;
    std::string symbol;
    std::string underlyer;
    std::string strategy_id;
    std::string portfolio_id;
    fix::Side side;
    double price;
    double quantity;           // Original/current order quantity
    double leaves_qty;         // Remaining unfilled quantity
    double cum_qty;            // Cumulative filled quantity
    double delta;              // Delta per contract
    OrderState state;

    // Pending replace values (stored while awaiting ack)
    std::optional<double> pending_price;
    std::optional<double> pending_quantity;
    std::optional<fix::OrderKey> pending_key;  // New ClOrdID for pending replace

    // Computed values
    double notional() const { return price * leaves_qty; }
    double delta_exposure() const { return delta * leaves_qty; }

    // Check if order is in a terminal state
    bool is_terminal() const {
        return state == OrderState::FILLED ||
               state == OrderState::CANCELED ||
               state == OrderState::REJECTED;
    }

    // Check if order contributes to metrics
    bool contributes_to_metrics() const {
        return state == OrderState::PENDING_NEW ||
               state == OrderState::OPEN ||
               state == OrderState::PENDING_REPLACE ||
               state == OrderState::PENDING_CANCEL;
    }
};

// Order book maintaining state of all tracked orders
class OrderBook {
private:
    // Primary index: ClOrdID -> Order
    std::unordered_map<fix::OrderKey, TrackedOrder> orders_;

    // Mapping from pending replace ClOrdID to original ClOrdID
    std::unordered_map<fix::OrderKey, fix::OrderKey> pending_replace_map_;

public:
    // Add a new order (on NewOrderSingle sent)
    void add_order(const fix::NewOrderSingle& msg) {
        TrackedOrder order;
        order.key = msg.key;
        order.symbol = msg.symbol;
        order.underlyer = msg.underlyer;
        order.strategy_id = msg.strategy_id;
        order.portfolio_id = msg.portfolio_id;
        order.side = msg.side;
        order.price = msg.price;
        order.quantity = msg.quantity;
        order.leaves_qty = msg.quantity;
        order.cum_qty = 0.0;
        order.delta = msg.delta;
        order.state = OrderState::PENDING_NEW;

        orders_[msg.key] = std::move(order);
    }

    // Get order by ClOrdID
    TrackedOrder* get_order(const fix::OrderKey& key) {
        auto it = orders_.find(key);
        if (it != orders_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    const TrackedOrder* get_order(const fix::OrderKey& key) const {
        auto it = orders_.find(key);
        if (it != orders_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // Resolve a ClOrdID that might be a pending replace key
    TrackedOrder* resolve_order(const fix::OrderKey& key) {
        // First check if this is a pending replace key
        auto pending_it = pending_replace_map_.find(key);
        if (pending_it != pending_replace_map_.end()) {
            return get_order(pending_it->second);
        }
        return get_order(key);
    }

    // Mark order as acknowledged (OPEN)
    void acknowledge_order(const fix::OrderKey& key) {
        auto* order = get_order(key);
        if (order && order->state == OrderState::PENDING_NEW) {
            order->state = OrderState::OPEN;
        }
    }

    // Mark order as rejected
    void reject_order(const fix::OrderKey& key) {
        auto* order = get_order(key);
        if (order) {
            order->state = OrderState::REJECTED;
        }
    }

    // Start a pending replace
    void start_replace(const fix::OrderKey& orig_key, const fix::OrderKey& new_key,
                       double new_price, double new_quantity) {
        auto* order = get_order(orig_key);
        if (order && (order->state == OrderState::OPEN || order->state == OrderState::PENDING_NEW)) {
            order->state = OrderState::PENDING_REPLACE;
            order->pending_key = new_key;
            order->pending_price = new_price;
            order->pending_quantity = new_quantity;
            pending_replace_map_[new_key] = orig_key;
        }
    }

    // Complete a successful replace - returns old values for metrics update
    struct ReplaceResult {
        double old_price;
        double old_leaves_qty;
        double old_notional;
        double old_delta_exposure;
    };

    std::optional<ReplaceResult> complete_replace(const fix::OrderKey& orig_key) {
        auto* order = get_order(orig_key);
        if (order && order->state == OrderState::PENDING_REPLACE &&
            order->pending_price.has_value() && order->pending_quantity.has_value()) {

            ReplaceResult result;
            result.old_price = order->price;
            result.old_leaves_qty = order->leaves_qty;
            result.old_notional = order->notional();
            result.old_delta_exposure = order->delta_exposure();

            // Apply pending values
            order->price = order->pending_price.value();
            order->quantity = order->pending_quantity.value();
            order->leaves_qty = order->pending_quantity.value();

            // Update key mapping if new key was assigned
            if (order->pending_key.has_value()) {
                pending_replace_map_.erase(order->pending_key.value());
                fix::OrderKey new_key = order->pending_key.value();
                TrackedOrder updated_order = std::move(*order);
                updated_order.key = new_key;
                orders_.erase(orig_key);
                orders_[new_key] = std::move(updated_order);
            }

            // Clear pending state
            order = get_order(order->pending_key.value_or(orig_key));
            if (order) {
                order->state = OrderState::OPEN;
                order->pending_price.reset();
                order->pending_quantity.reset();
                order->pending_key.reset();
            }

            return result;
        }
        return std::nullopt;
    }

    // Reject a replace - revert to original state
    void reject_replace(const fix::OrderKey& orig_key) {
        auto* order = get_order(orig_key);
        if (order && order->state == OrderState::PENDING_REPLACE) {
            if (order->pending_key.has_value()) {
                pending_replace_map_.erase(order->pending_key.value());
            }
            order->state = OrderState::OPEN;
            order->pending_price.reset();
            order->pending_quantity.reset();
            order->pending_key.reset();
        }
    }

    // Start a pending cancel
    void start_cancel(const fix::OrderKey& orig_key, const fix::OrderKey& cancel_key) {
        auto* order = get_order(orig_key);
        if (order && (order->state == OrderState::OPEN || order->state == OrderState::PENDING_NEW)) {
            order->state = OrderState::PENDING_CANCEL;
            pending_replace_map_[cancel_key] = orig_key;
        }
    }

    // Complete a cancel
    void complete_cancel(const fix::OrderKey& key) {
        auto* order = resolve_order(key);
        if (order) {
            order->state = OrderState::CANCELED;
        }
    }

    // Reject a cancel - revert to original state
    void reject_cancel(const fix::OrderKey& orig_key) {
        auto* order = get_order(orig_key);
        if (order && order->state == OrderState::PENDING_CANCEL) {
            order->state = OrderState::OPEN;
        }
    }

    // Apply a fill - returns fill details for metrics update
    struct FillResult {
        double filled_qty;
        double filled_notional;
        double filled_delta_exposure;
        bool is_complete;
    };

    std::optional<FillResult> apply_fill(const fix::OrderKey& key, double last_qty, double /*last_px*/) {
        auto* order = resolve_order(key);
        if (order && !order->is_terminal()) {
            FillResult result;
            result.filled_qty = last_qty;
            result.filled_notional = last_qty * order->price;  // Use order price for notional
            result.filled_delta_exposure = last_qty * order->delta;

            order->leaves_qty -= last_qty;
            order->cum_qty += last_qty;

            if (order->leaves_qty <= 0) {
                order->state = OrderState::FILLED;
                order->leaves_qty = 0;
                result.is_complete = true;
            } else {
                result.is_complete = false;
            }

            return result;
        }
        return std::nullopt;
    }

    // Remove terminal orders (cleanup)
    void cleanup_terminal_orders() {
        for (auto it = orders_.begin(); it != orders_.end(); ) {
            if (it->second.is_terminal()) {
                it = orders_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Get all active orders
    std::vector<const TrackedOrder*> active_orders() const {
        std::vector<const TrackedOrder*> result;
        for (const auto& [_, order] : orders_) {
            if (!order.is_terminal()) {
                result.push_back(&order);
            }
        }
        return result;
    }

    size_t size() const { return orders_.size(); }

    void clear() {
        orders_.clear();
        pending_replace_map_.clear();
    }
};

} // namespace engine
