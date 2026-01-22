#pragma once

#include "../aggregation/container_types.hpp"
#include <string>
#include <cstdint>
#include <type_traits>

namespace instrument {

// ============================================================================
// Instrument Types and Context Traits
// ============================================================================
//
// This header provides:
//   1. Context traits - compile-time validation for Context types that provide
//      accessor methods for retrieving instrument data
//   2. InstrumentData - a concrete instrument data type
//   3. NullInstrument - placeholder for metrics that don't need instrument data
//   4. Provider classes - SimpleInstrumentProvider and StaticInstrumentProvider
//
// The Instrument template type is treated as opaque - no compile-time validation
// is performed on Instrument types. Any type can be used as an Instrument, and
// errors will occur if required methods are missing when called.
//
// No virtual functions - compile-time polymorphism via templates.
//

// ============================================================================
// Context Traits - Validates Context types provide accessor methods
// ============================================================================
//
// These traits check that a Context type provides the required accessor methods
// for retrieving instrument data. The Context is parameterized on the Instrument
// type to allow for flexible instrument lookup patterns.
//
// Context requirements for notional computation:
//   - double contract_size(const Instrument&) const
//   - double spot_price(const Instrument&) const
//   - double fx_rate(const Instrument&) const
//
// Additional context requirements for delta computation:
//   - double delta(const Instrument&) const
//   - double underlyer_spot(const Instrument&) const
//
// Additional context requirements for vega computation:
//   - double vega(const Instrument&) const
//

// Individual Context method traits
template<typename C, typename I, typename = void>
struct has_context_contract_size : std::false_type {};

template<typename C, typename I>
struct has_context_contract_size<C, I, std::void_t<
    decltype(std::declval<const C&>().contract_size(std::declval<const I&>()))
>> : std::true_type {};

template<typename C, typename I>
inline constexpr bool has_context_contract_size_v = has_context_contract_size<C, I>::value;

template<typename C, typename I, typename = void>
struct has_context_spot_price : std::false_type {};

template<typename C, typename I>
struct has_context_spot_price<C, I, std::void_t<
    decltype(std::declval<const C&>().spot_price(std::declval<const I&>()))
>> : std::true_type {};

template<typename C, typename I>
inline constexpr bool has_context_spot_price_v = has_context_spot_price<C, I>::value;

template<typename C, typename I, typename = void>
struct has_context_fx_rate : std::false_type {};

template<typename C, typename I>
struct has_context_fx_rate<C, I, std::void_t<
    decltype(std::declval<const C&>().fx_rate(std::declval<const I&>()))
>> : std::true_type {};

template<typename C, typename I>
inline constexpr bool has_context_fx_rate_v = has_context_fx_rate<C, I>::value;

template<typename C, typename I, typename = void>
struct has_context_delta : std::false_type {};

template<typename C, typename I>
struct has_context_delta<C, I, std::void_t<
    decltype(std::declval<const C&>().delta(std::declval<const I&>()))
>> : std::true_type {};

template<typename C, typename I>
inline constexpr bool has_context_delta_v = has_context_delta<C, I>::value;

template<typename C, typename I, typename = void>
struct has_context_underlyer_spot : std::false_type {};

template<typename C, typename I>
struct has_context_underlyer_spot<C, I, std::void_t<
    decltype(std::declval<const C&>().underlyer_spot(std::declval<const I&>()))
>> : std::true_type {};

template<typename C, typename I>
inline constexpr bool has_context_underlyer_spot_v = has_context_underlyer_spot<C, I>::value;

template<typename C, typename I, typename = void>
struct has_context_vega : std::false_type {};

template<typename C, typename I>
struct has_context_vega<C, I, std::void_t<
    decltype(std::declval<const C&>().vega(std::declval<const I&>()))
>> : std::true_type {};

template<typename C, typename I>
inline constexpr bool has_context_vega_v = has_context_vega<C, I>::value;

// ============================================================================
// Combined Context traits
// ============================================================================

// Notional context: contract_size + spot_price + fx_rate
template<typename C, typename I>
struct is_notional_context : std::conjunction<
    has_context_contract_size<C, I>,
    has_context_spot_price<C, I>,
    has_context_fx_rate<C, I>
> {};

template<typename C, typename I>
inline constexpr bool is_notional_context_v = is_notional_context<C, I>::value;

// Delta context: notional context + delta + underlyer_spot
template<typename C, typename I>
struct is_delta_context : std::conjunction<
    is_notional_context<C, I>,
    has_context_delta<C, I>,
    has_context_underlyer_spot<C, I>
> {};

template<typename C, typename I>
inline constexpr bool is_delta_context_v = is_delta_context<C, I>::value;

// Vega context: delta context + vega
template<typename C, typename I>
struct is_vega_context : std::conjunction<
    is_delta_context<C, I>,
    has_context_vega<C, I>
> {};

template<typename C, typename I>
inline constexpr bool is_vega_context_v = is_vega_context<C, I>::value;

// ============================================================================
// Free function templates for computing values from any Instrument type
// ============================================================================
//
// These functions work with any Instrument type that provides the required
// methods. The Instrument type is not validated at compile time - errors
// will occur if required methods are missing.
//

// Compute notional: quantity * contract_size * spot_price * fx_rate
template<typename Instrument>
double compute_notional(const Instrument& inst, int64_t quantity) {
    return static_cast<double>(quantity)
         * inst.contract_size()
         * inst.spot_price()
         * inst.fx_rate();
}

// Compute delta exposure: quantity * delta * contract_size * underlyer_spot * fx_rate
template<typename Instrument>
double compute_delta_exposure(const Instrument& inst, int64_t quantity) {
    return static_cast<double>(quantity)
         * inst.delta()
         * inst.contract_size()
         * inst.underlyer_spot()
         * inst.fx_rate();
}

// Compute vega exposure: quantity * vega * contract_size * underlyer_spot * fx_rate
template<typename Instrument>
double compute_vega_exposure(const Instrument& inst, int64_t quantity) {
    return static_cast<double>(quantity)
         * inst.vega()
         * inst.contract_size()
         * inst.underlyer_spot()
         * inst.fx_rate();
}

// ============================================================================
// Context-aware free function templates
// ============================================================================
//
// These versions accept a Context that provides accessor methods for instrument
// properties. This allows the caller to control how instrument data is retrieved
// (e.g., from a cache, database, or real-time feed).
//
// Context requirements for notional:
//   - double contract_size(const Instrument&) const
//   - double spot_price(const Instrument&) const
//   - double fx_rate(const Instrument&) const
//
// Additional context requirements for delta exposure:
//   - double delta(const Instrument&) const
//   - double underlyer_spot(const Instrument&) const
//
// Additional context requirements for vega exposure:
//   - double vega(const Instrument&) const
//

template<typename Context, typename Instrument>
double compute_notional(const Context& ctx, const Instrument& inst, int64_t quantity) {
    static_assert(is_notional_context_v<Context, Instrument>,
                  "Context must provide contract_size, spot_price, fx_rate methods");
    return static_cast<double>(quantity)
         * ctx.contract_size(inst)
         * ctx.spot_price(inst)
         * ctx.fx_rate(inst);
}

template<typename Context, typename Instrument>
double compute_delta_exposure(const Context& ctx, const Instrument& inst, int64_t quantity) {
    static_assert(is_delta_context_v<Context, Instrument>,
                  "Context must provide delta, underlyer_spot methods (plus notional context)");
    return static_cast<double>(quantity)
         * ctx.delta(inst)
         * ctx.contract_size(inst)
         * ctx.underlyer_spot(inst)
         * ctx.fx_rate(inst);
}

template<typename Context, typename Instrument>
double compute_vega_exposure(const Context& ctx, const Instrument& inst, int64_t quantity) {
    static_assert(is_vega_context_v<Context, Instrument>,
                  "Context must provide vega method (plus delta context)");
    return static_cast<double>(quantity)
         * ctx.vega(inst)
         * ctx.contract_size(inst)
         * ctx.underlyer_spot(inst)
         * ctx.fx_rate(inst);
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

// ============================================================================
// SimpleInstrumentProvider - Minimal provider for testing (legacy)
// ============================================================================

class SimpleInstrumentProvider {
private:
    aggregation::HashMap<std::string, double> spot_prices_;
    aggregation::HashMap<std::string, double> fx_rates_;
    aggregation::HashMap<std::string, double> contract_sizes_;
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

// ============================================================================
// StaticInstrumentProvider - Full provider with pre-loaded data
// ============================================================================

class StaticInstrumentProvider {
private:
    aggregation::HashMap<std::string, InstrumentData> instruments_;
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

    // Convenience methods for computing exposures
    double compute_notional(const std::string& symbol, int64_t quantity) const {
        return static_cast<double>(quantity)
             * get_contract_size(symbol)
             * get_spot_price(symbol)
             * get_fx_rate(symbol);
    }

    double compute_delta_exposure(const std::string& symbol, int64_t quantity) const {
        return static_cast<double>(quantity)
             * get_delta(symbol)
             * get_contract_size(symbol)
             * get_underlyer_spot(symbol)
             * get_fx_rate(symbol);
    }

    double compute_vega_exposure(const std::string& symbol, int64_t quantity) const {
        return static_cast<double>(quantity)
             * get_vega(symbol)
             * get_contract_size(symbol)
             * get_underlyer_spot(symbol)
             * get_fx_rate(symbol);
    }
};

} // namespace instrument
