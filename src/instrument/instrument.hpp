#pragma once

#include <string>
#include <unordered_map>
#include <cstdint>
#include <type_traits>

namespace instrument {

// ============================================================================
// InstrumentProvider Traits (C++17 compatible via SFINAE)
// ============================================================================
//
// Hierarchical traits for InstrumentProvider capabilities:
//
// Base traits (required for ALL providers):
//   - double get_spot_price(const std::string& symbol) const
//   - double get_fx_rate(const std::string& symbol) const
//
// Notional traits (base + contract_size):
//   - double get_contract_size(const std::string& symbol) const
//
// Option traits (full provider, adds underlyer/delta support):
//   - std::string get_underlyer(const std::string& symbol) const
//   - double get_underlyer_spot(const std::string& symbol) const
//   - double get_delta(const std::string& symbol) const
//
// No virtual functions - compile-time polymorphism via templates.
//

// ============================================================================
// Individual method traits
// ============================================================================

template<typename T, typename = void>
struct has_spot_price : std::false_type {};

template<typename T>
struct has_spot_price<T, std::void_t<
    decltype(std::declval<const T&>().get_spot_price(std::declval<const std::string&>()))
>> : std::true_type {};

template<typename T>
inline constexpr bool has_spot_price_v = has_spot_price<T>::value;

template<typename T, typename = void>
struct has_fx_rate : std::false_type {};

template<typename T>
struct has_fx_rate<T, std::void_t<
    decltype(std::declval<const T&>().get_fx_rate(std::declval<const std::string&>()))
>> : std::true_type {};

template<typename T>
inline constexpr bool has_fx_rate_v = has_fx_rate<T>::value;

template<typename T, typename = void>
struct has_contract_size : std::false_type {};

template<typename T>
struct has_contract_size<T, std::void_t<
    decltype(std::declval<const T&>().get_contract_size(std::declval<const std::string&>()))
>> : std::true_type {};

template<typename T>
inline constexpr bool has_contract_size_v = has_contract_size<T>::value;

template<typename T, typename = void>
struct has_underlyer : std::false_type {};

template<typename T>
struct has_underlyer<T, std::void_t<
    decltype(std::declval<const T&>().get_underlyer(std::declval<const std::string&>()))
>> : std::true_type {};

template<typename T>
inline constexpr bool has_underlyer_v = has_underlyer<T>::value;

template<typename T, typename = void>
struct has_underlyer_spot : std::false_type {};

template<typename T>
struct has_underlyer_spot<T, std::void_t<
    decltype(std::declval<const T&>().get_underlyer_spot(std::declval<const std::string&>()))
>> : std::true_type {};

template<typename T>
inline constexpr bool has_underlyer_spot_v = has_underlyer_spot<T>::value;

template<typename T, typename = void>
struct has_delta : std::false_type {};

template<typename T>
struct has_delta<T, std::void_t<
    decltype(std::declval<const T&>().get_delta(std::declval<const std::string&>()))
>> : std::true_type {};

template<typename T>
inline constexpr bool has_delta_v = has_delta<T>::value;

// ============================================================================
// Combined provider traits
// ============================================================================

// Base provider: spot_price + fx_rate
template<typename T>
struct is_base_provider : std::conjunction<has_spot_price<T>, has_fx_rate<T>> {};

template<typename T>
inline constexpr bool is_base_provider_v = is_base_provider<T>::value;

// Notional provider: base + contract_size
template<typename T>
struct is_notional_provider : std::conjunction<
    is_base_provider<T>,
    has_contract_size<T>
> {};

template<typename T>
inline constexpr bool is_notional_provider_v = is_notional_provider<T>::value;

// Option provider: all 6 methods (full InstrumentProvider)
template<typename T>
struct is_option_provider : std::conjunction<
    is_base_provider<T>,
    has_contract_size<T>,
    has_underlyer<T>,
    has_underlyer_spot<T>,
    has_delta<T>
> {};

template<typename T>
inline constexpr bool is_option_provider_v = is_option_provider<T>::value;

// Backward compatibility: is_instrument_provider is alias to is_option_provider
template<typename T>
using is_instrument_provider = is_option_provider<T>;

template<typename T>
inline constexpr bool is_instrument_provider_v = is_option_provider_v<T>;

// ============================================================================
// Free function templates for computing values from any provider
// ============================================================================

// Compute notional: quantity * contract_size * spot_price * fx_rate
// Requires: is_notional_provider_v<Provider> (spot, fx, contract_size)
template<typename Provider>
double compute_notional(const Provider& provider, const std::string& symbol, int64_t quantity) {
    static_assert(is_notional_provider_v<Provider>,
                  "Provider must satisfy notional provider requirements (spot, fx, contract_size)");
    return static_cast<double>(quantity)
         * provider.get_contract_size(symbol)
         * provider.get_spot_price(symbol)
         * provider.get_fx_rate(symbol);
}

// Compute delta exposure: quantity * delta * contract_size * underlyer_spot * fx_rate
// Requires: is_option_provider_v<Provider> (full option-aware provider)
template<typename Provider>
double compute_delta_exposure(const Provider& provider, const std::string& symbol, int64_t quantity) {
    static_assert(is_option_provider_v<Provider>,
                  "Provider must satisfy option provider requirements (underlyer, delta support)");
    return static_cast<double>(quantity)
         * provider.get_delta(symbol)
         * provider.get_contract_size(symbol)
         * provider.get_underlyer_spot(symbol)
         * provider.get_fx_rate(symbol);
}

// ============================================================================
// InstrumentData - Data structure for instrument properties
// ============================================================================

struct InstrumentData {
    double spot_price = 0.0;
    double fx_rate = 1.0;           // 1.0 for USD
    double contract_size = 1.0;     // 1.0 for equities
    std::string underlyer;
    double underlyer_spot = 0.0;    // Same as spot for equities
    double delta = 1.0;             // 1.0 for equities/futures
};

// ============================================================================
// SimpleInstrumentProvider - Minimal provider for testing
// ============================================================================
//
// Provides only base traits (spot_price, fx_rate) plus contract_size.
// Useful for testing NotionalMetrics without option-specific methods.
// Does NOT provide: get_underlyer, get_underlyer_spot, get_delta
//

class SimpleInstrumentProvider {
private:
    std::unordered_map<std::string, double> spot_prices_;
    std::unordered_map<std::string, double> fx_rates_;
    std::unordered_map<std::string, double> contract_sizes_;
    double default_spot_ = 1.0;
    double default_fx_ = 1.0;
    double default_contract_size_ = 1.0;

public:
    SimpleInstrumentProvider() = default;

    void set_spot_price(const std::string& symbol, double price) {
        spot_prices_[symbol] = price;
    }

    void set_fx_rate(const std::string& symbol, double rate) {
        fx_rates_[symbol] = rate;
    }

    void set_contract_size(const std::string& symbol, double size) {
        contract_sizes_[symbol] = size;
    }

    void set_defaults(double spot, double fx, double contract_size) {
        default_spot_ = spot;
        default_fx_ = fx;
        default_contract_size_ = contract_size;
    }

    // Base provider interface
    double get_spot_price(const std::string& symbol) const {
        auto it = spot_prices_.find(symbol);
        return it != spot_prices_.end() ? it->second : default_spot_;
    }

    double get_fx_rate(const std::string& symbol) const {
        auto it = fx_rates_.find(symbol);
        return it != fx_rates_.end() ? it->second : default_fx_;
    }

    double get_contract_size(const std::string& symbol) const {
        auto it = contract_sizes_.find(symbol);
        return it != contract_sizes_.end() ? it->second : default_contract_size_;
    }
};

// Static assertions for SimpleInstrumentProvider
static_assert(is_base_provider_v<SimpleInstrumentProvider>,
              "SimpleInstrumentProvider must satisfy base provider");
static_assert(is_notional_provider_v<SimpleInstrumentProvider>,
              "SimpleInstrumentProvider must satisfy notional provider");
static_assert(!is_option_provider_v<SimpleInstrumentProvider>,
              "SimpleInstrumentProvider should NOT satisfy option provider");

// ============================================================================
// StaticInstrumentProvider - Concrete implementation with pre-loaded data
// ============================================================================
//
// Useful for testing and scenarios where instrument data doesn't change
// during the session. No virtual functions - all methods are non-virtual.
//

class StaticInstrumentProvider {
private:
    std::unordered_map<std::string, InstrumentData> instruments_;
    InstrumentData default_data_;

public:
    StaticInstrumentProvider() = default;

    // Add or update instrument data
    void add_instrument(const std::string& symbol, const InstrumentData& data) {
        instruments_[symbol] = data;
    }

    // Add equity (simple case: contract_size=1, fx_rate=1, delta=1)
    void add_equity(const std::string& symbol, double spot_price) {
        InstrumentData data;
        data.spot_price = spot_price;
        data.fx_rate = 1.0;
        data.contract_size = 1.0;
        data.underlyer = symbol;
        data.underlyer_spot = spot_price;
        data.delta = 1.0;
        instruments_[symbol] = data;
    }

    // Add option
    void add_option(const std::string& symbol,
                   const std::string& underlyer,
                   double spot_price,
                   double underlyer_spot,
                   double delta,
                   double contract_size = 100.0,
                   double fx_rate = 1.0) {
        InstrumentData data;
        data.spot_price = spot_price;
        data.fx_rate = fx_rate;
        data.contract_size = contract_size;
        data.underlyer = underlyer;
        data.underlyer_spot = underlyer_spot;
        data.delta = delta;
        instruments_[symbol] = data;
    }

    // Add future
    void add_future(const std::string& symbol,
                   const std::string& underlyer,
                   double spot_price,
                   double underlyer_spot,
                   double contract_size = 1.0,
                   double fx_rate = 1.0) {
        InstrumentData data;
        data.spot_price = spot_price;
        data.fx_rate = fx_rate;
        data.contract_size = contract_size;
        data.underlyer = underlyer;
        data.underlyer_spot = underlyer_spot;
        data.delta = 1.0;  // Futures have delta of 1
        instruments_[symbol] = data;
    }

    // Set default data for unknown instruments
    void set_default(const InstrumentData& data) {
        default_data_ = data;
    }

    // Update spot price for an instrument
    void update_spot_price(const std::string& symbol, double new_spot) {
        auto it = instruments_.find(symbol);
        if (it != instruments_.end()) {
            it->second.spot_price = new_spot;
        }
    }

    // Update underlyer spot for all options on that underlyer
    void update_underlyer_spot(const std::string& underlyer, double new_spot) {
        for (auto& [symbol, data] : instruments_) {
            if (data.underlyer == underlyer) {
                data.underlyer_spot = new_spot;
                // Also update spot for the underlyer itself
                if (symbol == underlyer) {
                    data.spot_price = new_spot;
                }
            }
        }
    }

    // Update delta for an option
    void update_delta(const std::string& symbol, double new_delta) {
        auto it = instruments_.find(symbol);
        if (it != instruments_.end()) {
            it->second.delta = new_delta;
        }
    }

    // Check if instrument exists
    bool has_instrument(const std::string& symbol) const {
        return instruments_.find(symbol) != instruments_.end();
    }

    // Clear all instruments
    void clear() {
        instruments_.clear();
    }

    // ========================================================================
    // InstrumentProvider interface (non-virtual)
    // ========================================================================

    double get_spot_price(const std::string& symbol) const {
        auto it = instruments_.find(symbol);
        if (it != instruments_.end()) {
            return it->second.spot_price;
        }
        return default_data_.spot_price;
    }

    double get_fx_rate(const std::string& symbol) const {
        auto it = instruments_.find(symbol);
        if (it != instruments_.end()) {
            return it->second.fx_rate;
        }
        return default_data_.fx_rate;
    }

    double get_contract_size(const std::string& symbol) const {
        auto it = instruments_.find(symbol);
        if (it != instruments_.end()) {
            return it->second.contract_size;
        }
        return default_data_.contract_size;
    }

    std::string get_underlyer(const std::string& symbol) const {
        auto it = instruments_.find(symbol);
        if (it != instruments_.end()) {
            return it->second.underlyer;
        }
        return symbol;  // Default: symbol is its own underlyer
    }

    double get_underlyer_spot(const std::string& symbol) const {
        auto it = instruments_.find(symbol);
        if (it != instruments_.end()) {
            return it->second.underlyer_spot;
        }
        return default_data_.underlyer_spot;
    }

    double get_delta(const std::string& symbol) const {
        auto it = instruments_.find(symbol);
        if (it != instruments_.end()) {
            return it->second.delta;
        }
        return default_data_.delta;
    }

    // Convenience methods that use the free function templates
    double compute_notional(const std::string& symbol, int64_t quantity) const {
        return instrument::compute_notional(*this, symbol, quantity);
    }

    double compute_delta_exposure(const std::string& symbol, int64_t quantity) const {
        return instrument::compute_delta_exposure(*this, symbol, quantity);
    }
};

// Static assertion to verify StaticInstrumentProvider satisfies the concept
static_assert(is_instrument_provider_v<StaticInstrumentProvider>,
              "StaticInstrumentProvider must satisfy InstrumentProvider requirements");

} // namespace instrument
