#pragma once

#include "grouping.hpp"

// Forward declarations
namespace engine {
    struct TrackedOrder;
}

namespace aggregation {

// ============================================================================
// KeyExtractor - Extracts grouping keys from TrackedOrder
// ============================================================================
//
// Each specialization provides:
// - extract(order): Extract the key from a TrackedOrder
// - is_applicable(order): Returns true if this grouping level applies to the order
//
// The is_applicable() method allows conditional aggregation, e.g., skipping
// strategy-level aggregation for orders with empty strategy_id.
//

// Primary template - must be specialized for each key type
template<typename Key>
struct KeyExtractor;

// GlobalKey extractor - always applicable
template<>
struct KeyExtractor<GlobalKey> {
    static GlobalKey extract(const engine::TrackedOrder& /*order*/) {
        return GlobalKey::instance();
    }

    static bool is_applicable(const engine::TrackedOrder& /*order*/) {
        return true;
    }
};

// UnderlyerKey extractor - always applicable (underlyer should always be set)
template<>
struct KeyExtractor<UnderlyerKey> {
    static UnderlyerKey extract(const engine::TrackedOrder& order);

    static bool is_applicable(const engine::TrackedOrder& /*order*/) {
        return true;
    }
};

// InstrumentKey extractor - always applicable
template<>
struct KeyExtractor<InstrumentKey> {
    static InstrumentKey extract(const engine::TrackedOrder& order);

    static bool is_applicable(const engine::TrackedOrder& /*order*/) {
        return true;
    }
};

// StrategyKey extractor - only applicable if strategy_id is non-empty
template<>
struct KeyExtractor<StrategyKey> {
    static StrategyKey extract(const engine::TrackedOrder& order);

    static bool is_applicable(const engine::TrackedOrder& order);
};

// PortfolioKey extractor - only applicable if portfolio_id is non-empty
template<>
struct KeyExtractor<PortfolioKey> {
    static PortfolioKey extract(const engine::TrackedOrder& order);

    static bool is_applicable(const engine::TrackedOrder& order);
};

// InstrumentSideKey extractor - always applicable
template<>
struct KeyExtractor<InstrumentSideKey> {
    static InstrumentSideKey extract(const engine::TrackedOrder& order);

    static bool is_applicable(const engine::TrackedOrder& /*order*/) {
        return true;
    }
};

// PortfolioInstrumentKey extractor - only applicable if portfolio_id is non-empty
template<>
struct KeyExtractor<PortfolioInstrumentKey> {
    static PortfolioInstrumentKey extract(const engine::TrackedOrder& order);

    static bool is_applicable(const engine::TrackedOrder& order);
};

} // namespace aggregation

// Include TrackedOrder definition and implement extractors
#include "../engine/order_state.hpp"

namespace aggregation {

inline UnderlyerKey KeyExtractor<UnderlyerKey>::extract(const engine::TrackedOrder& order) {
    return UnderlyerKey{order.underlyer};
}

inline InstrumentKey KeyExtractor<InstrumentKey>::extract(const engine::TrackedOrder& order) {
    return InstrumentKey{order.symbol};
}

inline StrategyKey KeyExtractor<StrategyKey>::extract(const engine::TrackedOrder& order) {
    return StrategyKey{order.strategy_id};
}

inline bool KeyExtractor<StrategyKey>::is_applicable(const engine::TrackedOrder& order) {
    return !order.strategy_id.empty();
}

inline PortfolioKey KeyExtractor<PortfolioKey>::extract(const engine::TrackedOrder& order) {
    return PortfolioKey{order.portfolio_id};
}

inline bool KeyExtractor<PortfolioKey>::is_applicable(const engine::TrackedOrder& order) {
    return !order.portfolio_id.empty();
}

inline InstrumentSideKey KeyExtractor<InstrumentSideKey>::extract(const engine::TrackedOrder& order) {
    return InstrumentSideKey{order.symbol, static_cast<int>(order.side)};
}

inline PortfolioInstrumentKey KeyExtractor<PortfolioInstrumentKey>::extract(const engine::TrackedOrder& order) {
    return PortfolioInstrumentKey{order.portfolio_id, order.symbol};
}

inline bool KeyExtractor<PortfolioInstrumentKey>::is_applicable(const engine::TrackedOrder& order) {
    return !order.portfolio_id.empty();
}

} // namespace aggregation
