#pragma once

#include "order_stage.hpp"
#include <type_traits>

namespace aggregation {

// ============================================================================
// ConditionalStorage - Storage that is conditionally included based on template param
// ============================================================================
//
// When Include=false, this is an empty struct with zero size (Empty Base Optimization).
// When Include=true, this contains the data member.
//

template<bool Include, typename Data>
struct ConditionalStorage {
    // Empty when Include=false - no data member
    void clear() {}
};

template<typename Data>
struct ConditionalStorage<true, Data> {
    Data data{};

    void clear() {
        if constexpr (std::is_class_v<Data>) {
            data.clear();
        } else {
            data = Data{};
        }
    }
};

// ============================================================================
// StagedMetric - Automatically manages per-stage storage based on template params
// ============================================================================
//
// This template eliminates the boilerplate of manually defining position_data_,
// open_data_, in_flight_data_ and the get_stage_data() switch statement.
//
// Usage:
//   StagedMetric<MyData, OpenStage, InFlightStage>  // Only open and in-flight
//   StagedMetric<MyData, AllStages>                  // All three stages
//   StagedMetric<MyData>                             // Default: all stages
//
// The Data type must have a clear() method.
//

template<typename Data, typename... Stages>
class StagedMetric {
public:
    using data_type = Data;
    using Config = StageConfig<Stages...>;

    static constexpr bool tracks_position = Config::track_position;
    static constexpr bool tracks_open = Config::track_open;
    static constexpr bool tracks_in_flight = Config::track_in_flight;

private:
    ConditionalStorage<Config::track_position, Data> position_;
    ConditionalStorage<Config::track_open, Data> open_;
    ConditionalStorage<Config::track_in_flight, Data> in_flight_;

public:
    // ========================================================================
    // Compile-time enabled accessors
    // ========================================================================

    // Position stage accessors
    template<typename Dummy = void>
    std::enable_if_t<Config::track_position && std::is_void_v<Dummy>, Data&>
    position() {
        return position_.data;
    }

    template<typename Dummy = void>
    std::enable_if_t<Config::track_position && std::is_void_v<Dummy>, const Data&>
    position() const {
        return position_.data;
    }

    // Open stage accessors
    template<typename Dummy = void>
    std::enable_if_t<Config::track_open && std::is_void_v<Dummy>, Data&>
    open() {
        return open_.data;
    }

    template<typename Dummy = void>
    std::enable_if_t<Config::track_open && std::is_void_v<Dummy>, const Data&>
    open() const {
        return open_.data;
    }

    // In-flight stage accessors
    template<typename Dummy = void>
    std::enable_if_t<Config::track_in_flight && std::is_void_v<Dummy>, Data&>
    in_flight() {
        return in_flight_.data;
    }

    template<typename Dummy = void>
    std::enable_if_t<Config::track_in_flight && std::is_void_v<Dummy>, const Data&>
    in_flight() const {
        return in_flight_.data;
    }

    // ========================================================================
    // Runtime stage accessor (for event handlers)
    // ========================================================================
    //
    // Returns a pointer to the Data for the given stage, or nullptr if that
    // stage is not tracked.
    //

    Data* get_stage(OrderStage stage) {
        switch (stage) {
            case OrderStage::POSITION:
                if constexpr (Config::track_position) {
                    return &position_.data;
                }
                return nullptr;
            case OrderStage::OPEN:
                if constexpr (Config::track_open) {
                    return &open_.data;
                }
                return nullptr;
            case OrderStage::IN_FLIGHT:
                if constexpr (Config::track_in_flight) {
                    return &in_flight_.data;
                }
                return nullptr;
        }
        return nullptr;
    }

    const Data* get_stage(OrderStage stage) const {
        switch (stage) {
            case OrderStage::POSITION:
                if constexpr (Config::track_position) {
                    return &position_.data;
                }
                return nullptr;
            case OrderStage::OPEN:
                if constexpr (Config::track_open) {
                    return &open_.data;
                }
                return nullptr;
            case OrderStage::IN_FLIGHT:
                if constexpr (Config::track_in_flight) {
                    return &in_flight_.data;
                }
                return nullptr;
        }
        return nullptr;
    }

    // ========================================================================
    // Utility methods
    // ========================================================================

    void clear() {
        position_.clear();
        open_.clear();
        in_flight_.clear();
    }

    // Apply a function to each tracked stage
    template<typename Func>
    void for_each_stage(Func&& func) {
        if constexpr (Config::track_position) {
            func(OrderStage::POSITION, position_.data);
        }
        if constexpr (Config::track_open) {
            func(OrderStage::OPEN, open_.data);
        }
        if constexpr (Config::track_in_flight) {
            func(OrderStage::IN_FLIGHT, in_flight_.data);
        }
    }

    template<typename Func>
    void for_each_stage(Func&& func) const {
        if constexpr (Config::track_position) {
            func(OrderStage::POSITION, position_.data);
        }
        if constexpr (Config::track_open) {
            func(OrderStage::OPEN, open_.data);
        }
        if constexpr (Config::track_in_flight) {
            func(OrderStage::IN_FLIGHT, in_flight_.data);
        }
    }
};

} // namespace aggregation
