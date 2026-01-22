#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <iomanip>

namespace engine {

// ============================================================================
// Limit Types
// ============================================================================

enum class LimitType {
    ORDER_COUNT,           // Per-instrument-side order count
    QUOTED_INSTRUMENTS,    // Per-underlyer unique instruments
    GROSS_DELTA,           // Per-underlyer gross delta
    NET_DELTA,             // Per-underlyer net delta
    STRATEGY_NOTIONAL,     // Per-strategy notional
    PORTFOLIO_NOTIONAL,    // Per-portfolio notional
    GLOBAL_NOTIONAL,       // Global notional
    GLOBAL_GROSS_NOTIONAL, // Global gross notional (sum of |notional|)
    GLOBAL_NET_NOTIONAL    // Global net notional (BID - ASK)
};

inline const char* to_string(LimitType type) {
    switch (type) {
        case LimitType::ORDER_COUNT: return "ORDER_COUNT";
        case LimitType::QUOTED_INSTRUMENTS: return "QUOTED_INSTRUMENTS";
        case LimitType::GROSS_DELTA: return "GROSS_DELTA";
        case LimitType::NET_DELTA: return "NET_DELTA";
        case LimitType::STRATEGY_NOTIONAL: return "STRATEGY_NOTIONAL";
        case LimitType::PORTFOLIO_NOTIONAL: return "PORTFOLIO_NOTIONAL";
        case LimitType::GLOBAL_NOTIONAL: return "GLOBAL_NOTIONAL";
        case LimitType::GLOBAL_GROSS_NOTIONAL: return "GLOBAL_GROSS_NOTIONAL";
        case LimitType::GLOBAL_NET_NOTIONAL: return "GLOBAL_NET_NOTIONAL";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// LimitBreachInfo - Details about a single limit breach
// ============================================================================

struct LimitBreachInfo {
    LimitType type;
    std::string key;           // Grouping key (symbol:side, underlyer, strategy_id, etc.)
    double limit_value;        // Configured limit
    double current_usage;      // Current aggregate value
    double hypothetical_usage; // Value after order

    std::string to_string() const {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2);
        ss << ::engine::to_string(type) << " limit breached on '" << key << "': "
           << "current=" << current_usage
           << ", after_order=" << hypothetical_usage
           << ", limit=" << limit_value;
        return ss.str();
    }
};

// ============================================================================
// PreTradeCheckResult - Result of a pre-trade check
// ============================================================================

struct PreTradeCheckResult {
    bool would_breach = false;
    std::vector<LimitBreachInfo> breaches;

    // Implicit conversion: true = order is OK to proceed
    explicit operator bool() const { return !would_breach; }

    // Add a breach
    void add_breach(LimitBreachInfo info) {
        would_breach = true;
        breaches.push_back(std::move(info));
    }

    // Human-readable summary
    std::string to_string() const {
        if (!would_breach) {
            return "Pre-trade check passed: no limits breached";
        }
        std::ostringstream ss;
        ss << "Pre-trade check FAILED: " << breaches.size() << " limit(s) breached";
        for (const auto& breach : breaches) {
            ss << "\n  - " << breach.to_string();
        }
        return ss.str();
    }

    // Check if a specific limit type was breached
    bool has_breach(LimitType type) const {
        for (const auto& breach : breaches) {
            if (breach.type == type) {
                return true;
            }
        }
        return false;
    }

    // Get first breach of a specific type (if any)
    const LimitBreachInfo* get_breach(LimitType type) const {
        for (const auto& breach : breaches) {
            if (breach.type == type) {
                return &breach;
            }
        }
        return nullptr;
    }
};

}  // namespace engine
