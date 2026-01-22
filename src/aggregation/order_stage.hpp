#pragma once

#include <type_traits>

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

// ============================================================================
// Stage type tags for compile-time metric configuration
// ============================================================================
//
// These type tags allow metrics to explicitly declare which stages they track
// via template parameters. This provides compile-time type safety and allows
// metrics to be configured for specific stage tracking requirements.
//
// Usage:
//   DeltaMetrics<Provider, AllStages>  // Track all stages (default)
//   DeltaMetrics<Provider, OpenStage, InFlightStage>  // Track only open and in-flight
//

struct PositionStage {
    static constexpr OrderStage value = OrderStage::POSITION;
    static constexpr const char* name = "position";
};

struct OpenStage {
    static constexpr OrderStage value = OrderStage::OPEN;
    static constexpr const char* name = "open";
};

struct InFlightStage {
    static constexpr OrderStage value = OrderStage::IN_FLIGHT;
    static constexpr const char* name = "in_flight";
};

// Meta-tag representing all stages (Position + Open + InFlight)
struct AllStages {
    static constexpr const char* name = "all";
};

// ============================================================================
// Stage configuration helper
// ============================================================================
//
// StageConfig determines which stages a metric tracks based on template params.
// If AllStages is provided OR no stages are specified, all stages are tracked.
// Otherwise, only the explicitly listed stages are tracked.
//

template<typename... Stages>
struct StageConfig {
private:
    template<typename Target, typename... List>
    static constexpr bool contains() {
        return (std::is_same_v<Target, List> || ...);
    }

    // If AllStages is provided OR no stages specified, track all stages
    static constexpr bool has_all_stages = (sizeof...(Stages) == 0) || contains<AllStages, Stages...>();

public:
    static constexpr bool track_position = has_all_stages || contains<PositionStage, Stages...>();
    static constexpr bool track_open = has_all_stages || contains<OpenStage, Stages...>();
    static constexpr bool track_in_flight = has_all_stages || contains<InFlightStage, Stages...>();
    static constexpr size_t stage_count =
        (track_position ? 1 : 0) + (track_open ? 1 : 0) + (track_in_flight ? 1 : 0);
};

// Convenience alias for tracking all stages
using DefaultStageConfig = StageConfig<AllStages>;

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
