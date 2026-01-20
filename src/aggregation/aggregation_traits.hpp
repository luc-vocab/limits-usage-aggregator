#pragma once

#include <cstdint>
#include <type_traits>

namespace aggregation {

// ============================================================================
// Combiners - Define how values are combined and uncombined
// ============================================================================

// Sum combiner for additive values (e.g., notional, delta)
template<typename T>
struct SumCombiner {
    using value_type = T;

    static constexpr T identity() { return T{0}; }

    static T combine(T current, T delta) {
        return current + delta;
    }

    static T uncombine(T current, T delta) {
        return current - delta;
    }
};

// Count combiner for counting items
struct CountCombiner {
    using value_type = int64_t;

    static constexpr int64_t identity() { return 0; }

    static int64_t combine(int64_t current, int64_t delta) {
        return current + delta;
    }

    static int64_t uncombine(int64_t current, int64_t delta) {
        return current - delta;
    }
};

// Max combiner for tracking maximum values
template<typename T>
struct MaxCombiner {
    using value_type = T;

    static constexpr T identity() { return std::numeric_limits<T>::lowest(); }

    // Note: Max combiner only supports combine, not efficient uncombine
    static T combine(T current, T value) {
        return std::max(current, value);
    }
};

// Min combiner for tracking minimum values
template<typename T>
struct MinCombiner {
    using value_type = T;

    static constexpr T identity() { return std::numeric_limits<T>::max(); }

    // Note: Min combiner only supports combine, not efficient uncombine
    static T combine(T current, T value) {
        return std::min(current, value);
    }
};

// ============================================================================
// Value types for specific metrics
// ============================================================================

// Delta value holding both gross and net
struct DeltaValue {
    double gross = 0.0;  // Absolute sum of all deltas
    double net = 0.0;    // Signed sum of all deltas

    DeltaValue() = default;
    DeltaValue(double g, double n) : gross(g), net(n) {}

    bool operator==(const DeltaValue& other) const {
        return gross == other.gross && net == other.net;
    }
};

// Combiner for delta values
struct DeltaCombiner {
    using value_type = DeltaValue;

    static DeltaValue identity() { return DeltaValue{0.0, 0.0}; }

    static DeltaValue combine(DeltaValue current, DeltaValue delta) {
        return DeltaValue{
            current.gross + delta.gross,
            current.net + delta.net
        };
    }

    static DeltaValue uncombine(DeltaValue current, DeltaValue delta) {
        return DeltaValue{
            current.gross - delta.gross,
            current.net - delta.net
        };
    }
};

// ============================================================================
// Type traits for aggregation
// ============================================================================

// Check if a combiner supports uncombine operation
template<typename Combiner, typename = void>
struct has_uncombine : std::false_type {};

template<typename Combiner>
struct has_uncombine<Combiner,
    std::void_t<decltype(Combiner::uncombine(
        std::declval<typename Combiner::value_type>(),
        std::declval<typename Combiner::value_type>()))>
    > : std::true_type {};

template<typename Combiner>
inline constexpr bool has_uncombine_v = has_uncombine<Combiner>::value;

} // namespace aggregation
