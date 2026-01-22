#pragma once

#include <unordered_map>
#include <optional>
#include <string>
#include <cmath>
#include <functional>

namespace engine {

// ============================================================================
// Limit Comparison Modes
// ============================================================================

enum class LimitComparisonMode {
    SIGNED,     // Compare signed value directly (value > limit triggers breach)
    ABSOLUTE    // Compare absolute value (|value| > limit triggers breach)
};

// ============================================================================
// LimitStore - Generic limit storage for any key type
// ============================================================================
//
// Stores limits keyed by a given Key type (e.g., std::string for underlyer).
// Supports a default limit and per-key overrides.
//

template<typename Key>
class LimitStore {
private:
    std::unordered_map<Key, double> limits_;
    double default_limit_ = std::numeric_limits<double>::max();
    LimitComparisonMode mode_ = LimitComparisonMode::ABSOLUTE;

public:
    // Set the default limit (used when no specific limit is set)
    void set_default_limit(double limit) {
        default_limit_ = limit;
    }

    double default_limit() const {
        return default_limit_;
    }

    // Set limit for a specific key
    void set_limit(const Key& key, double limit) {
        limits_[key] = limit;
    }

    // Remove limit for a specific key (falls back to default)
    void remove_limit(const Key& key) {
        limits_.erase(key);
    }

    // Get limit for a key (returns default if not set)
    double get_limit(const Key& key) const {
        auto it = limits_.find(key);
        return (it != limits_.end()) ? it->second : default_limit_;
    }

    // Check if a specific limit is set (vs using default)
    bool has_specific_limit(const Key& key) const {
        return limits_.find(key) != limits_.end();
    }

    // Set comparison mode
    void set_comparison_mode(LimitComparisonMode mode) {
        mode_ = mode;
    }

    LimitComparisonMode comparison_mode() const {
        return mode_;
    }

    // Check if a value would breach the limit for a given key
    bool would_breach(const Key& key, double current_value, double delta = 0.0) const {
        double limit = get_limit(key);
        double new_value = current_value + delta;

        if (mode_ == LimitComparisonMode::ABSOLUTE) {
            return std::abs(new_value) > limit;
        }
        return new_value > limit;
    }

    // Check if a value is at or above the limit (for count-based limits where >= triggers breach)
    bool at_or_above_limit(const Key& key, double current_value) const {
        double limit = get_limit(key);
        if (mode_ == LimitComparisonMode::ABSOLUTE) {
            return std::abs(current_value) >= limit;
        }
        return current_value >= limit;
    }

    // Clear all limits (keeps default)
    void clear() {
        limits_.clear();
    }

    // Clear everything including default
    void reset() {
        limits_.clear();
        default_limit_ = std::numeric_limits<double>::max();
        mode_ = LimitComparisonMode::ABSOLUTE;
    }
};

// Type alias for string-keyed limit stores (common case)
using StringLimitStore = LimitStore<std::string>;

} // namespace engine
