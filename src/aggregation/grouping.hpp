#pragma once

#include <string>
#include <functional>

namespace aggregation {

// ============================================================================
// Grouping Keys - Define the levels at which metrics can be aggregated
// ============================================================================

// Global aggregation (singleton key for system-wide totals)
struct GlobalKey {
    bool operator==(const GlobalKey&) const { return true; }
    bool operator!=(const GlobalKey&) const { return false; }

    static GlobalKey instance() { return GlobalKey{}; }
};

// Per-underlyer aggregation
struct UnderlyerKey {
    std::string underlyer;

    bool operator==(const UnderlyerKey& other) const {
        return underlyer == other.underlyer;
    }
    bool operator!=(const UnderlyerKey& other) const {
        return !(*this == other);
    }
};

// Per-instrument aggregation
struct InstrumentKey {
    std::string symbol;

    bool operator==(const InstrumentKey& other) const {
        return symbol == other.symbol;
    }
    bool operator!=(const InstrumentKey& other) const {
        return !(*this == other);
    }
};

// Per-strategy aggregation
struct StrategyKey {
    std::string strategy_id;

    bool operator==(const StrategyKey& other) const {
        return strategy_id == other.strategy_id;
    }
    bool operator!=(const StrategyKey& other) const {
        return !(*this == other);
    }
};

// Per-portfolio aggregation
struct PortfolioKey {
    std::string portfolio_id;

    bool operator==(const PortfolioKey& other) const {
        return portfolio_id == other.portfolio_id;
    }
    bool operator!=(const PortfolioKey& other) const {
        return !(*this == other);
    }
};

// Composite key for instrument + side
struct InstrumentSideKey {
    std::string symbol;
    int side;  // 1=Bid, 2=Ask

    bool operator==(const InstrumentSideKey& other) const {
        return symbol == other.symbol && side == other.side;
    }
    bool operator!=(const InstrumentSideKey& other) const {
        return !(*this == other);
    }
};

} // namespace aggregation

// Hash specializations
namespace std {
    template<>
    struct hash<aggregation::GlobalKey> {
        size_t operator()(const aggregation::GlobalKey&) const {
            return 0;  // Singleton key, constant hash
        }
    };

    template<>
    struct hash<aggregation::UnderlyerKey> {
        size_t operator()(const aggregation::UnderlyerKey& key) const {
            return hash<string>{}(key.underlyer);
        }
    };

    template<>
    struct hash<aggregation::InstrumentKey> {
        size_t operator()(const aggregation::InstrumentKey& key) const {
            return hash<string>{}(key.symbol);
        }
    };

    template<>
    struct hash<aggregation::StrategyKey> {
        size_t operator()(const aggregation::StrategyKey& key) const {
            return hash<string>{}(key.strategy_id);
        }
    };

    template<>
    struct hash<aggregation::PortfolioKey> {
        size_t operator()(const aggregation::PortfolioKey& key) const {
            return hash<string>{}(key.portfolio_id);
        }
    };

    template<>
    struct hash<aggregation::InstrumentSideKey> {
        size_t operator()(const aggregation::InstrumentSideKey& key) const {
            size_t h1 = hash<string>{}(key.symbol);
            size_t h2 = hash<int>{}(key.side);
            return h1 ^ (h2 << 1);
        }
    };
}
