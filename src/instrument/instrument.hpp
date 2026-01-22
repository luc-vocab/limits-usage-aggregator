#pragma once

#include <string>
#include <unordered_map>
#include <cstdint>
#include <type_traits>

namespace instrument {

// ============================================================================
// Instrument Traits (C++17 compatible via SFINAE)
// ============================================================================
//
// Traits to validate that an Instrument type satisfies the required interface.
// Instrument types must provide methods (not member access), allowing any
// type that implements the required methods to be used.
//
// Required methods for base Instrument:
//   - double spot_price() const
//   - double fx_rate() const
//   - double contract_size() const
//
// Additional methods for option-aware Instrument:
//   - std::string underlyer() const (or const std::string&)
//   - double underlyer_spot() const
//   - double delta() const
//
// Additional methods for vega-aware Instrument:
//   - double vega() const
//
// No virtual functions - compile-time polymorphism via templates.
//

// ============================================================================
// Individual method traits for Instrument types
// ============================================================================

template<typename T, typename = void>
struct has_spot_price_method : std::false_type {};

template<typename T>
struct has_spot_price_method<T, std::void_t<
    decltype(std::declval<const T&>().spot_price())
>> : std::true_type {};

template<typename T>
inline constexpr bool has_spot_price_method_v = has_spot_price_method<T>::value;

template<typename T, typename = void>
struct has_fx_rate_method : std::false_type {};

template<typename T>
struct has_fx_rate_method<T, std::void_t<
    decltype(std::declval<const T&>().fx_rate())
>> : std::true_type {};

template<typename T>
inline constexpr bool has_fx_rate_method_v = has_fx_rate_method<T>::value;

template<typename T, typename = void>
struct has_contract_size_method : std::false_type {};

template<typename T>
struct has_contract_size_method<T, std::void_t<
    decltype(std::declval<const T&>().contract_size())
>> : std::true_type {};

template<typename T>
inline constexpr bool has_contract_size_method_v = has_contract_size_method<T>::value;

template<typename T, typename = void>
struct has_underlyer_method : std::false_type {};

template<typename T>
struct has_underlyer_method<T, std::void_t<
    decltype(std::declval<const T&>().underlyer())
>> : std::true_type {};

template<typename T>
inline constexpr bool has_underlyer_method_v = has_underlyer_method<T>::value;

template<typename T, typename = void>
struct has_underlyer_spot_method : std::false_type {};

template<typename T>
struct has_underlyer_spot_method<T, std::void_t<
    decltype(std::declval<const T&>().underlyer_spot())
>> : std::true_type {};

template<typename T>
inline constexpr bool has_underlyer_spot_method_v = has_underlyer_spot_method<T>::value;

template<typename T, typename = void>
struct has_delta_method : std::false_type {};

template<typename T>
struct has_delta_method<T, std::void_t<
    decltype(std::declval<const T&>().delta())
>> : std::true_type {};

template<typename T>
inline constexpr bool has_delta_method_v = has_delta_method<T>::value;

template<typename T, typename = void>
struct has_vega_method : std::false_type {};

template<typename T>
struct has_vega_method<T, std::void_t<
    decltype(std::declval<const T&>().vega())
>> : std::true_type {};

template<typename T>
inline constexpr bool has_vega_method_v = has_vega_method<T>::value;

// ============================================================================
// Combined Instrument traits
// ============================================================================

// Base instrument: spot_price + fx_rate + contract_size
template<typename T>
struct is_base_instrument : std::conjunction<
    has_spot_price_method<T>,
    has_fx_rate_method<T>,
    has_contract_size_method<T>
> {};

template<typename T>
inline constexpr bool is_base_instrument_v = is_base_instrument<T>::value;

// Notional instrument: same as base (alias for clarity)
template<typename T>
using is_notional_instrument = is_base_instrument<T>;

template<typename T>
inline constexpr bool is_notional_instrument_v = is_base_instrument_v<T>;

// Option instrument: base + underlyer, underlyer_spot, delta
template<typename T>
struct is_option_instrument : std::conjunction<
    is_base_instrument<T>,
    has_underlyer_method<T>,
    has_underlyer_spot_method<T>,
    has_delta_method<T>
> {};

template<typename T>
inline constexpr bool is_option_instrument_v = is_option_instrument<T>::value;

// Backward compatibility alias
template<typename T>
using is_instrument = is_option_instrument<T>;

template<typename T>
inline constexpr bool is_instrument_v = is_option_instrument_v<T>;

// Vega instrument: option instrument + vega
template<typename T>
struct is_vega_instrument : std::conjunction<
    is_option_instrument<T>,
    has_vega_method<T>
> {};

template<typename T>
inline constexpr bool is_vega_instrument_v = is_vega_instrument<T>::value;

// ============================================================================
// Legacy Provider Traits (for backward compatibility during transition)
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

template<typename T, typename = void>
struct has_vega : std::false_type {};

template<typename T>
struct has_vega<T, std::void_t<
    decltype(std::declval<const T&>().get_vega(std::declval<const std::string&>()))
>> : std::true_type {};

template<typename T>
inline constexpr bool has_vega_v = has_vega<T>::value;

// Combined provider traits (legacy)
template<typename T>
struct is_base_provider : std::conjunction<has_spot_price<T>, has_fx_rate<T>> {};

template<typename T>
inline constexpr bool is_base_provider_v = is_base_provider<T>::value;

template<typename T>
struct is_notional_provider : std::conjunction<
    is_base_provider<T>,
    has_contract_size<T>
> {};

template<typename T>
inline constexpr bool is_notional_provider_v = is_notional_provider<T>::value;

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

template<typename T>
using is_instrument_provider = is_option_provider<T>;

template<typename T>
inline constexpr bool is_instrument_provider_v = is_option_provider_v<T>;

template<typename T>
struct is_vega_provider : std::conjunction<
    is_option_provider<T>,
    has_vega<T>
> {};

template<typename T>
inline constexpr bool is_vega_provider_v = is_vega_provider<T>::value;

// ============================================================================
// Free function templates for computing values from any Instrument type
// ============================================================================

// Compute notional: quantity * contract_size * spot_price * fx_rate
// Requires: is_notional_instrument_v<Instrument>
template<typename Instrument>
double compute_notional(const Instrument& inst, int64_t quantity) {
    static_assert(is_notional_instrument_v<Instrument>,
                  "Instrument must satisfy notional instrument requirements (spot_price, fx_rate, contract_size)");
    return static_cast<double>(quantity)
         * inst.contract_size()
         * inst.spot_price()
         * inst.fx_rate();
}

// Compute delta exposure: quantity * delta * contract_size * underlyer_spot * fx_rate
// Requires: is_option_instrument_v<Instrument>
template<typename Instrument>
double compute_delta_exposure(const Instrument& inst, int64_t quantity) {
    static_assert(is_option_instrument_v<Instrument>,
                  "Instrument must satisfy option instrument requirements (underlyer, delta support)");
    return static_cast<double>(quantity)
         * inst.delta()
         * inst.contract_size()
         * inst.underlyer_spot()
         * inst.fx_rate();
}

// Compute vega exposure: quantity * vega * contract_size * underlyer_spot * fx_rate
// Requires: is_vega_instrument_v<Instrument>
template<typename Instrument>
double compute_vega_exposure(const Instrument& inst, int64_t quantity) {
    static_assert(is_vega_instrument_v<Instrument>,
                  "Instrument must satisfy vega instrument requirements (vega support)");
    return static_cast<double>(quantity)
         * inst.vega()
         * inst.contract_size()
         * inst.underlyer_spot()
         * inst.fx_rate();
}

// Legacy overloads for Provider-based API (backward compatibility)
template<typename Provider>
double compute_notional(const Provider& provider, const std::string& symbol, int64_t quantity) {
    static_assert(is_notional_provider_v<Provider>,
                  "Provider must satisfy notional provider requirements (spot, fx, contract_size)");
    return static_cast<double>(quantity)
         * provider.get_contract_size(symbol)
         * provider.get_spot_price(symbol)
         * provider.get_fx_rate(symbol);
}

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

template<typename Provider>
double compute_vega_exposure(const Provider& provider, const std::string& symbol, int64_t quantity) {
    static_assert(is_vega_provider_v<Provider>,
                  "Provider must satisfy vega provider requirements (vega support)");
    return static_cast<double>(quantity)
         * provider.get_vega(symbol)
         * provider.get_contract_size(symbol)
         * provider.get_underlyer_spot(symbol)
         * provider.get_fx_rate(symbol);
}

// ============================================================================
// InstrumentData - Instrument value type with method-based access
// ============================================================================
//
// A concrete Instrument type that can be passed to engine methods.
// Implements the full Instrument interface via accessor methods.
//

class InstrumentData {
private:
    double spot_price_ = 0.0;
    double fx_rate_ = 1.0;           // 1.0 for USD
    double contract_size_ = 1.0;     // 1.0 for equities
    std::string underlyer_;
    double underlyer_spot_ = 0.0;    // Same as spot for equities
    double delta_ = 1.0;             // 1.0 for equities/futures
    double vega_ = 0.0;              // 0.0 for equities/futures

public:
    InstrumentData() = default;

    // ========================================================================
    // Accessors (required by Instrument traits)
    // ========================================================================

    double spot_price() const { return spot_price_; }
    double fx_rate() const { return fx_rate_; }
    double contract_size() const { return contract_size_; }
    const std::string& underlyer() const { return underlyer_; }
    double underlyer_spot() const { return underlyer_spot_; }
    double delta() const { return delta_; }
    double vega() const { return vega_; }

    // ========================================================================
    // Builder pattern for construction
    // ========================================================================

    InstrumentData& with_spot_price(double value) { spot_price_ = value; return *this; }
    InstrumentData& with_fx_rate(double value) { fx_rate_ = value; return *this; }
    InstrumentData& with_contract_size(double value) { contract_size_ = value; return *this; }
    InstrumentData& with_underlyer(const std::string& value) { underlyer_ = value; return *this; }
    InstrumentData& with_underlyer_spot(double value) { underlyer_spot_ = value; return *this; }
    InstrumentData& with_delta(double value) { delta_ = value; return *this; }
    InstrumentData& with_vega(double value) { vega_ = value; return *this; }

    // ========================================================================
    // Static factory methods
    // ========================================================================

    // Create an equity instrument
    static InstrumentData equity(double spot_price, double fx_rate = 1.0) {
        InstrumentData data;
        data.spot_price_ = spot_price;
        data.fx_rate_ = fx_rate;
        data.contract_size_ = 1.0;
        data.underlyer_ = "";  // Will be set based on symbol
        data.underlyer_spot_ = spot_price;
        data.delta_ = 1.0;
        data.vega_ = 0.0;
        return data;
    }

    // Create an option instrument
    static InstrumentData option(
        double spot_price,
        const std::string& underlyer,
        double underlyer_spot,
        double delta,
        double contract_size = 100.0,
        double fx_rate = 1.0,
        double vega = 0.0) {
        InstrumentData data;
        data.spot_price_ = spot_price;
        data.fx_rate_ = fx_rate;
        data.contract_size_ = contract_size;
        data.underlyer_ = underlyer;
        data.underlyer_spot_ = underlyer_spot;
        data.delta_ = delta;
        data.vega_ = vega;
        return data;
    }

    // Create a future instrument
    static InstrumentData future(
        double spot_price,
        const std::string& underlyer,
        double underlyer_spot,
        double contract_size = 1.0,
        double fx_rate = 1.0) {
        InstrumentData data;
        data.spot_price_ = spot_price;
        data.fx_rate_ = fx_rate;
        data.contract_size_ = contract_size;
        data.underlyer_ = underlyer;
        data.underlyer_spot_ = underlyer_spot;
        data.delta_ = 1.0;  // Futures have delta of 1
        data.vega_ = 0.0;
        return data;
    }
};

// Static assertion to verify InstrumentData satisfies instrument traits
static_assert(is_instrument_v<InstrumentData>,
              "InstrumentData must satisfy Instrument requirements");
static_assert(is_vega_instrument_v<InstrumentData>,
              "InstrumentData must satisfy VegaInstrument requirements");

// ============================================================================
// NullInstrument - Placeholder for metrics that don't need instrument data
// ============================================================================
//
// Use this type as the Instrument template parameter for metrics that
// don't use instrument data (e.g., OrderCountMetric, QuotedInstrumentCountMetric).
// All methods return default values.
//

struct NullInstrument {
    double spot_price() const { return 0.0; }
    double fx_rate() const { return 1.0; }
    double contract_size() const { return 1.0; }
    const std::string& underlyer() const { static std::string empty; return empty; }
    double underlyer_spot() const { return 0.0; }
    double delta() const { return 1.0; }
    double vega() const { return 0.0; }

    // Singleton instance for convenience
    static const NullInstrument& instance() {
        static NullInstrument inst;
        return inst;
    }
};

// Static assertion to verify NullInstrument satisfies instrument traits
static_assert(is_vega_instrument_v<NullInstrument>,
              "NullInstrument must satisfy Instrument requirements");

// ============================================================================
// SimpleInstrumentProvider - Minimal provider for testing (legacy)
// ============================================================================

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

    // Get instrument data for passing to engine methods
    InstrumentData get_instrument(const std::string& symbol) const {
        return InstrumentData::equity(get_spot_price(symbol), get_fx_rate(symbol))
            .with_contract_size(get_contract_size(symbol))
            .with_underlyer(symbol);
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
// StaticInstrumentProvider - Full provider with pre-loaded data
// ============================================================================

class StaticInstrumentProvider {
private:
    std::unordered_map<std::string, InstrumentData> instruments_;
    InstrumentData default_data_;

public:
    StaticInstrumentProvider() = default;

    // ========================================================================
    // Lookup - returns InstrumentData for caller to pass to engine
    // ========================================================================

    const InstrumentData& get_instrument(const std::string& symbol) const {
        auto it = instruments_.find(symbol);
        return (it != instruments_.end()) ? it->second : default_data_;
    }

    // ========================================================================
    // Management methods
    // ========================================================================

    void add_instrument(const std::string& symbol, const InstrumentData& data) {
        instruments_[symbol] = data;
    }

    void add_equity(const std::string& symbol, double spot_price, double fx_rate = 1.0) {
        InstrumentData data = InstrumentData::equity(spot_price, fx_rate);
        data.with_underlyer(symbol);
        instruments_[symbol] = data;
    }

    void add_option(const std::string& symbol,
                   const std::string& underlyer,
                   double spot_price,
                   double underlyer_spot,
                   double delta,
                   double contract_size = 100.0,
                   double fx_rate = 1.0,
                   double vega = 0.0) {
        instruments_[symbol] = InstrumentData::option(
            spot_price, underlyer, underlyer_spot, delta, contract_size, fx_rate, vega);
    }

    void add_future(const std::string& symbol,
                   const std::string& underlyer,
                   double spot_price,
                   double underlyer_spot,
                   double contract_size = 1.0,
                   double fx_rate = 1.0) {
        instruments_[symbol] = InstrumentData::future(
            spot_price, underlyer, underlyer_spot, contract_size, fx_rate);
    }

    void set_default(const InstrumentData& data) {
        default_data_ = data;
    }

    void update_spot_price(const std::string& symbol, double new_spot) {
        auto it = instruments_.find(symbol);
        if (it != instruments_.end()) {
            it->second.with_spot_price(new_spot);
        }
    }

    void update_underlyer_spot(const std::string& underlyer, double new_spot) {
        for (auto& [symbol, data] : instruments_) {
            if (data.underlyer() == underlyer) {
                data.with_underlyer_spot(new_spot);
                if (symbol == underlyer) {
                    data.with_spot_price(new_spot);
                }
            }
        }
    }

    void update_delta(const std::string& symbol, double new_delta) {
        auto it = instruments_.find(symbol);
        if (it != instruments_.end()) {
            it->second.with_delta(new_delta);
        }
    }

    bool has_instrument(const std::string& symbol) const {
        return instruments_.find(symbol) != instruments_.end();
    }

    void clear() {
        instruments_.clear();
    }

    // ========================================================================
    // Legacy InstrumentProvider interface (for backward compatibility)
    // ========================================================================

    double get_spot_price(const std::string& symbol) const {
        return get_instrument(symbol).spot_price();
    }

    double get_fx_rate(const std::string& symbol) const {
        return get_instrument(symbol).fx_rate();
    }

    double get_contract_size(const std::string& symbol) const {
        return get_instrument(symbol).contract_size();
    }

    std::string get_underlyer(const std::string& symbol) const {
        return get_instrument(symbol).underlyer();
    }

    double get_underlyer_spot(const std::string& symbol) const {
        return get_instrument(symbol).underlyer_spot();
    }

    double get_delta(const std::string& symbol) const {
        return get_instrument(symbol).delta();
    }

    double get_vega(const std::string& symbol) const {
        return get_instrument(symbol).vega();
    }

    // Convenience methods using free function templates
    double compute_notional(const std::string& symbol, int64_t quantity) const {
        return instrument::compute_notional(*this, symbol, quantity);
    }

    double compute_delta_exposure(const std::string& symbol, int64_t quantity) const {
        return instrument::compute_delta_exposure(*this, symbol, quantity);
    }

    double compute_vega_exposure(const std::string& symbol, int64_t quantity) const {
        return instrument::compute_vega_exposure(*this, symbol, quantity);
    }
};

// Static assertion to verify StaticInstrumentProvider satisfies provider traits
static_assert(is_instrument_provider_v<StaticInstrumentProvider>,
              "StaticInstrumentProvider must satisfy InstrumentProvider requirements");
static_assert(is_vega_provider_v<StaticInstrumentProvider>,
              "StaticInstrumentProvider must satisfy VegaProvider requirements");

} // namespace instrument
