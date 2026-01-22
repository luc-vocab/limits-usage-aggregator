#pragma once

#include "../aggregation/multi_group_aggregator.hpp"
#include "../fix/fix_types.hpp"
#include "../instrument/instrument.hpp"
#include <unordered_map>
#include <set>

// Forward declarations
namespace engine {
    struct TrackedOrder;
    class OrderBook;
}

namespace metrics {

// ============================================================================
// Delta Metrics - Tracks gross and net delta at various grouping levels
// ============================================================================
//
// Uses quantity-based tracking with lazy delta computation via InstrumentProvider.
// Delta exposure is computed as: quantity * delta * contract_size * underlyer_spot * fx_rate
//
// Template parameter Provider must satisfy the InstrumentProvider concept.
//
// Tracks quantities at:
// - Global level (system-wide totals)
// - Per-underlyer level (e.g., AAPL, MSFT)
//

template<typename Provider>
class DeltaMetrics {
    static_assert(instrument::is_instrument_provider_v<Provider>,
                  "Provider must satisfy InstrumentProvider requirements");

private:
    const Provider* provider_ = nullptr;

    // Track quantities per instrument and side
    // Key: symbol, Value: quantity
    std::unordered_map<std::string, int64_t> instrument_bid_qty_;
    std::unordered_map<std::string, int64_t> instrument_ask_qty_;

    // Track which instruments belong to which underlyer
    std::unordered_map<std::string, std::set<std::string>> underlyer_instruments_;

    // Global quantity totals (for quick access)
    int64_t global_bid_qty_ = 0;
    int64_t global_ask_qty_ = 0;

    // Get underlyer for a symbol
    std::string get_underlyer(const std::string& symbol) const {
        if (provider_) {
            return provider_->get_underlyer(symbol);
        }
        return symbol;  // Default: symbol is its own underlyer
    }

    // Compute delta exposure for a single instrument
    double compute_instrument_delta(const std::string& symbol, int64_t quantity) const {
        if (!provider_) {
            return 0.0;
        }
        return instrument::compute_delta_exposure(*provider_, symbol, quantity);
    }

public:
    DeltaMetrics() = default;

    void set_instrument_provider(const Provider* provider) {
        provider_ = provider;
    }

    const Provider* instrument_provider() const {
        return provider_;
    }

    // ========================================================================
    // Generic metric interface (used by template RiskAggregationEngine)
    // ========================================================================

    // Called when order is sent (PENDING_NEW state)
    void on_order_added(const engine::TrackedOrder& order);

    // Called when order is fully removed (nack, cancel, full fill)
    void on_order_removed(const engine::TrackedOrder& order);

    // Called when order is modified (update ack)
    void on_order_updated(const engine::TrackedOrder& order, int64_t old_qty);

    // Called on partial fill
    void on_partial_fill(const engine::TrackedOrder& order, int64_t filled_qty);

    // ========================================================================
    // Direct interface (for backward compatibility and direct usage)
    // ========================================================================

    void add_order(const std::string& symbol, const std::string& underlyer,
                   int64_t quantity, fix::Side side) {
        // Track underlyer mapping
        underlyer_instruments_[underlyer].insert(symbol);

        if (side == fix::Side::BID) {
            instrument_bid_qty_[symbol] += quantity;
            global_bid_qty_ += quantity;
        } else {
            instrument_ask_qty_[symbol] += quantity;
            global_ask_qty_ += quantity;
        }
    }

    void remove_order(const std::string& symbol, const std::string& underlyer,
                      int64_t quantity, fix::Side side) {
        if (side == fix::Side::BID) {
            auto it = instrument_bid_qty_.find(symbol);
            if (it != instrument_bid_qty_.end()) {
                it->second -= quantity;
                global_bid_qty_ -= quantity;
                if (it->second <= 0) {
                    instrument_bid_qty_.erase(it);
                }
            }
        } else {
            auto it = instrument_ask_qty_.find(symbol);
            if (it != instrument_ask_qty_.end()) {
                it->second -= quantity;
                global_ask_qty_ -= quantity;
                if (it->second <= 0) {
                    instrument_ask_qty_.erase(it);
                }
            }
        }

        // Clean up underlyer mapping if no more instruments
        cleanup_underlyer_mapping(symbol, underlyer);
    }

    void update_order(const std::string& symbol, const std::string& underlyer,
                      int64_t old_quantity, int64_t new_quantity, fix::Side side) {
        remove_order(symbol, underlyer, old_quantity, side);
        add_order(symbol, underlyer, new_quantity, side);
    }

    void partial_fill(const std::string& symbol, const std::string& underlyer,
                      int64_t filled_qty, fix::Side side) {
        remove_order(symbol, underlyer, filled_qty, side);
    }

    // ========================================================================
    // Accessors - compute delta values lazily using InstrumentProvider
    // ========================================================================

    // Quantity accessors
    int64_t global_bid_quantity() const { return global_bid_qty_; }
    int64_t global_ask_quantity() const { return global_ask_qty_; }
    int64_t global_quantity() const { return global_bid_qty_ + global_ask_qty_; }

    int64_t instrument_bid_quantity(const std::string& symbol) const {
        auto it = instrument_bid_qty_.find(symbol);
        return it != instrument_bid_qty_.end() ? it->second : 0;
    }

    int64_t instrument_ask_quantity(const std::string& symbol) const {
        auto it = instrument_ask_qty_.find(symbol);
        return it != instrument_ask_qty_.end() ? it->second : 0;
    }

    // Delta value accessors (computed lazily)
    aggregation::DeltaValue global_delta() const {
        double gross = 0.0;
        double net = 0.0;

        // Sum delta exposure across all instruments
        for (const auto& [symbol, qty] : instrument_bid_qty_) {
            double delta_exp = compute_instrument_delta(symbol, qty);
            gross += std::abs(delta_exp);
            net += delta_exp;  // Bids are positive
        }

        for (const auto& [symbol, qty] : instrument_ask_qty_) {
            double delta_exp = compute_instrument_delta(symbol, qty);
            gross += std::abs(delta_exp);
            net -= delta_exp;  // Asks are negative
        }

        return aggregation::DeltaValue{gross, net};
    }

    aggregation::DeltaValue underlyer_delta(const std::string& underlyer) const {
        double gross = 0.0;
        double net = 0.0;

        auto it = underlyer_instruments_.find(underlyer);
        if (it == underlyer_instruments_.end()) {
            return aggregation::DeltaValue{0.0, 0.0};
        }

        for (const auto& symbol : it->second) {
            // Bid side
            auto bid_it = instrument_bid_qty_.find(symbol);
            if (bid_it != instrument_bid_qty_.end()) {
                double delta_exp = compute_instrument_delta(symbol, bid_it->second);
                gross += std::abs(delta_exp);
                net += delta_exp;
            }

            // Ask side
            auto ask_it = instrument_ask_qty_.find(symbol);
            if (ask_it != instrument_ask_qty_.end()) {
                double delta_exp = compute_instrument_delta(symbol, ask_it->second);
                gross += std::abs(delta_exp);
                net -= delta_exp;
            }
        }

        return aggregation::DeltaValue{gross, net};
    }

    double global_gross_delta() const { return global_delta().gross; }
    double global_net_delta() const { return global_delta().net; }

    double underlyer_gross_delta(const std::string& underlyer) const {
        return underlyer_delta(underlyer).gross;
    }

    double underlyer_net_delta(const std::string& underlyer) const {
        return underlyer_delta(underlyer).net;
    }

    std::vector<aggregation::UnderlyerKey> underlyers() const {
        std::vector<aggregation::UnderlyerKey> result;
        for (const auto& [underlyer, _] : underlyer_instruments_) {
            result.push_back(aggregation::UnderlyerKey{underlyer});
        }
        return result;
    }

    void clear() {
        instrument_bid_qty_.clear();
        instrument_ask_qty_.clear();
        underlyer_instruments_.clear();
        global_bid_qty_ = 0;
        global_ask_qty_ = 0;
    }

private:
    void cleanup_underlyer_mapping(const std::string& symbol, const std::string& underlyer) {
        // Check if instrument still has quantities
        bool has_bid = instrument_bid_qty_.find(symbol) != instrument_bid_qty_.end();
        bool has_ask = instrument_ask_qty_.find(symbol) != instrument_ask_qty_.end();

        if (!has_bid && !has_ask) {
            auto it = underlyer_instruments_.find(underlyer);
            if (it != underlyer_instruments_.end()) {
                it->second.erase(symbol);
                if (it->second.empty()) {
                    underlyer_instruments_.erase(it);
                }
            }
        }
    }
};

} // namespace metrics

// Include TrackedOrder definition and implement generic interface
#include "../engine/order_state.hpp"

namespace metrics {

template<typename Provider>
void DeltaMetrics<Provider>::on_order_added(const engine::TrackedOrder& order) {
    add_order(order.symbol, order.underlyer, order.leaves_qty, order.side);
}

template<typename Provider>
void DeltaMetrics<Provider>::on_order_removed(const engine::TrackedOrder& order) {
    remove_order(order.symbol, order.underlyer, order.leaves_qty, order.side);
}

template<typename Provider>
void DeltaMetrics<Provider>::on_order_updated(const engine::TrackedOrder& order, int64_t old_qty) {
    update_order(order.symbol, order.underlyer, old_qty, order.leaves_qty, order.side);
}

template<typename Provider>
void DeltaMetrics<Provider>::on_partial_fill(const engine::TrackedOrder& order, int64_t filled_qty) {
    partial_fill(order.symbol, order.underlyer, filled_qty, order.side);
}

} // namespace metrics

// ============================================================================
// AccessorMixin specialization for DeltaMetrics
// ============================================================================
//
// This specialization provides CRTP-based accessor methods for engines
// that include DeltaMetrics.
//

#include "../engine/accessor_mixin.hpp"

namespace engine {

template<typename Derived, typename Provider>
class AccessorMixin<Derived, metrics::DeltaMetrics<Provider>> {
protected:
    const metrics::DeltaMetrics<Provider>& delta_metrics_() const {
        return static_cast<const Derived*>(this)->template get_metric<metrics::DeltaMetrics<Provider>>();
    }

public:
    double global_gross_delta() const {
        return delta_metrics_().global_gross_delta();
    }

    double global_net_delta() const {
        return delta_metrics_().global_net_delta();
    }

    double underlyer_gross_delta(const std::string& underlyer) const {
        return delta_metrics_().underlyer_gross_delta(underlyer);
    }

    double underlyer_net_delta(const std::string& underlyer) const {
        return delta_metrics_().underlyer_net_delta(underlyer);
    }

    aggregation::DeltaValue global_delta() const {
        return delta_metrics_().global_delta();
    }

    aggregation::DeltaValue underlyer_delta(const std::string& underlyer) const {
        return delta_metrics_().underlyer_delta(underlyer);
    }

    // Quantity accessors
    int64_t global_bid_quantity() const {
        return delta_metrics_().global_bid_quantity();
    }

    int64_t global_ask_quantity() const {
        return delta_metrics_().global_ask_quantity();
    }
};

} // namespace engine
