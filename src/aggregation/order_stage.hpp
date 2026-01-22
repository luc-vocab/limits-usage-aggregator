#pragma once

namespace engine {
    enum class OrderState;  // Forward declaration
}

namespace aggregation {

// ============================================================================
// OrderStage - Categorization of orders for metric tracking
// ============================================================================
//
// Orders flow through different stages during their lifecycle:
// - POSITION: Filled contracts, SOD positions, external position updates
// - OPEN: Acknowledged, live orders (OrderState::OPEN)
// - IN_FLIGHT: Orders pending acknowledgment or modification
//
// This categorization allows tracking metrics separately for:
// - What we actually own (position)
// - What we're actively quoting (open orders)
// - What's pending (in-flight)
//
// Limit checks can be applied to individual stages or combinations.
//

enum class OrderStage {
    POSITION,    // Filled contracts, SOD positions, external updates
    OPEN,        // Acknowledged, live orders
    IN_FLIGHT    // PENDING_NEW, PENDING_REPLACE, PENDING_CANCEL
};

inline const char* to_string(OrderStage stage) {
    switch (stage) {
        case OrderStage::POSITION: return "POSITION";
        case OrderStage::OPEN: return "OPEN";
        case OrderStage::IN_FLIGHT: return "IN_FLIGHT";
        default: return "UNKNOWN";
    }
}

} // namespace aggregation

// Include the full OrderState definition for the mapping function
#include "../engine/order_state.hpp"

namespace aggregation {

// Map OrderState to OrderStage
// Note: FILLED orders are terminal and don't belong to a stage (fills go to POSITION)
inline OrderStage stage_from_order_state(engine::OrderState state) {
    switch (state) {
        case engine::OrderState::PENDING_NEW:
        case engine::OrderState::PENDING_REPLACE:
        case engine::OrderState::PENDING_CANCEL:
            return OrderStage::IN_FLIGHT;
        case engine::OrderState::OPEN:
            return OrderStage::OPEN;
        default:
            // FILLED, CANCELED, REJECTED - these are terminal states
            // Fills should go to POSITION via explicit fill handling
            return OrderStage::POSITION;
    }
}

// Check if an order state is non-terminal (contributes to open or in-flight)
inline bool is_active_order_state(engine::OrderState state) {
    switch (state) {
        case engine::OrderState::PENDING_NEW:
        case engine::OrderState::OPEN:
        case engine::OrderState::PENDING_REPLACE:
        case engine::OrderState::PENDING_CANCEL:
            return true;
        default:
            return false;
    }
}

} // namespace aggregation
