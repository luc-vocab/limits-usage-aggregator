#pragma once

#include "../aggregation/aggregation_core.hpp"
#include "../fix/fix_types.hpp"
#include <unordered_set>

namespace metrics {

// ============================================================================
// Order Count Metrics - Tracks order counts per instrument/side
// ============================================================================

class OrderCountMetrics {
private:
    aggregation::InstrumentOrderCountBucket per_instrument_side_;

    // Track which instruments have orders per underlyer (for quoted count)
    // underlyer -> set of instruments
    std::unordered_map<std::string, std::unordered_set<std::string>> instruments_per_underlyer_;
    aggregation::UnderlyerInstrumentCountBucket quoted_instruments_;

public:
    // Add an order
    void add_order(const std::string& symbol, const std::string& underlyer, fix::Side side) {
        aggregation::InstrumentSideKey key{symbol, static_cast<int>(side)};
        per_instrument_side_.add(key, 1);

        // Track quoted instruments per underlyer
        auto& instruments = instruments_per_underlyer_[underlyer];
        if (instruments.find(symbol) == instruments.end()) {
            instruments.insert(symbol);
            // First order for this instrument under this underlyer
            quoted_instruments_.add(aggregation::UnderlyerKey{underlyer}, 1);
        }
    }

    // Remove an order
    void remove_order(const std::string& symbol, const std::string& underlyer, fix::Side side) {
        aggregation::InstrumentSideKey key{symbol, static_cast<int>(side)};
        per_instrument_side_.remove(key, 1);

        // Check if we still have any orders for this instrument
        aggregation::InstrumentSideKey bid_key{symbol, static_cast<int>(fix::Side::BID)};
        aggregation::InstrumentSideKey ask_key{symbol, static_cast<int>(fix::Side::ASK)};

        int64_t bid_count = per_instrument_side_.get(bid_key);
        int64_t ask_count = per_instrument_side_.get(ask_key);

        if (bid_count == 0 && ask_count == 0) {
            // No more orders for this instrument
            auto it = instruments_per_underlyer_.find(underlyer);
            if (it != instruments_per_underlyer_.end()) {
                it->second.erase(symbol);
                quoted_instruments_.remove(aggregation::UnderlyerKey{underlyer}, 1);
                if (it->second.empty()) {
                    instruments_per_underlyer_.erase(it);
                }
            }
        }
    }

    // Accessors
    int64_t bid_order_count(const std::string& symbol) const {
        return per_instrument_side_.get(aggregation::InstrumentSideKey{symbol, static_cast<int>(fix::Side::BID)});
    }

    int64_t ask_order_count(const std::string& symbol) const {
        return per_instrument_side_.get(aggregation::InstrumentSideKey{symbol, static_cast<int>(fix::Side::ASK)});
    }

    int64_t total_order_count(const std::string& symbol) const {
        return bid_order_count(symbol) + ask_order_count(symbol);
    }

    int64_t quoted_instruments_count(const std::string& underlyer) const {
        return quoted_instruments_.get(aggregation::UnderlyerKey{underlyer});
    }

    // Get all underlyers with quoted instruments
    std::vector<aggregation::UnderlyerKey> underlyers() const {
        return quoted_instruments_.keys();
    }

    void clear() {
        per_instrument_side_.clear();
        instruments_per_underlyer_.clear();
        quoted_instruments_.clear();
    }
};

} // namespace metrics
