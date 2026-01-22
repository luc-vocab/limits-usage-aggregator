#pragma once

#include "../fix/fix_types.hpp"
#include "../instrument/instrument.hpp"
#include <cmath>
#include <cstdint>

namespace metrics {

// ============================================================================
// Input Policies - Define how to capture and compute exposure from context
// ============================================================================
//
// Each InputPolicy defines:
// - StoredInputs: what to capture from context (for drift-free tracking)
// - capture(): how to build StoredInputs from context
// - compute_from_context(): fallback computation using current context values
// - supports_position_set: whether set_instrument_position is supported (default false)
//

// ============================================================================
// DeltaInputPolicy - For delta exposure metrics
// ============================================================================

template<typename Context, typename Instrument>
struct DeltaInputPolicy {
    static constexpr bool supports_position_set = false;

    struct StoredInputs {
        int64_t quantity;
        double delta;
        double contract_size;
        double underlyer_spot;
        double fx_rate;
        fix::Side side;

        double compute_exposure() const {
            return static_cast<double>(quantity) * delta * contract_size * underlyer_spot * fx_rate;
        }

        StoredInputs with_quantity(int64_t new_qty) const {
            return StoredInputs{new_qty, delta, contract_size, underlyer_spot, fx_rate, side};
        }
    };

    static StoredInputs capture(const Context& ctx, const Instrument& inst,
                                int64_t quantity, fix::Side side) {
        return StoredInputs{
            quantity,
            ctx.delta(inst),
            ctx.contract_size(inst),
            ctx.underlyer_spot(inst),
            ctx.fx_rate(inst),
            side
        };
    }

    static double compute_from_context(const Context& ctx, const Instrument& inst,
                                       int64_t quantity, fix::Side /*side*/) {
        return instrument::compute_delta_exposure(ctx, inst, quantity);
    }
};

// ============================================================================
// VegaInputPolicy - For vega exposure metrics
// ============================================================================

template<typename Context, typename Instrument>
struct VegaInputPolicy {
    static constexpr bool supports_position_set = false;

    struct StoredInputs {
        int64_t quantity;
        double vega;
        double contract_size;
        double underlyer_spot;
        double fx_rate;
        fix::Side side;

        double compute_exposure() const {
            return static_cast<double>(quantity) * vega * contract_size * underlyer_spot * fx_rate;
        }

        StoredInputs with_quantity(int64_t new_qty) const {
            return StoredInputs{new_qty, vega, contract_size, underlyer_spot, fx_rate, side};
        }
    };

    static StoredInputs capture(const Context& ctx, const Instrument& inst,
                                int64_t quantity, fix::Side side) {
        return StoredInputs{
            quantity,
            ctx.vega(inst),
            ctx.contract_size(inst),
            ctx.underlyer_spot(inst),
            ctx.fx_rate(inst),
            side
        };
    }

    static double compute_from_context(const Context& ctx, const Instrument& inst,
                                       int64_t quantity, fix::Side /*side*/) {
        return instrument::compute_vega_exposure(ctx, inst, quantity);
    }
};

// ============================================================================
// NotionalInputPolicy - For notional exposure metrics
// ============================================================================
//
// Unlike delta/vega, notional uses spot_price instead of underlyer_spot
// and has no greek value.
//

template<typename Context, typename Instrument>
struct NotionalInputPolicy {
    static constexpr bool supports_position_set = true;

    struct StoredInputs {
        int64_t quantity;
        double contract_size;
        double spot_price;
        double fx_rate;
        fix::Side side;

        double compute_exposure() const {
            return static_cast<double>(quantity) * contract_size * spot_price * fx_rate;
        }

        StoredInputs with_quantity(int64_t new_qty) const {
            return StoredInputs{new_qty, contract_size, spot_price, fx_rate, side};
        }
    };

    static StoredInputs capture(const Context& ctx, const Instrument& inst,
                                int64_t quantity, fix::Side side) {
        return StoredInputs{
            quantity,
            ctx.contract_size(inst),
            ctx.spot_price(inst),
            ctx.fx_rate(inst),
            side
        };
    }

    static double compute_from_context(const Context& ctx, const Instrument& inst,
                                       int64_t quantity, fix::Side /*side*/) {
        return instrument::compute_notional(ctx, inst, quantity);
    }
};

// ============================================================================
// Value Policies - Define how to derive final value from exposure
// ============================================================================

// GrossValuePolicy - Returns absolute value of exposure
struct GrossValuePolicy {
    template<typename StoredInputs>
    static double compute(const StoredInputs& inputs) {
        return std::abs(inputs.compute_exposure());
    }

    // For pre-trade check contribution calculation
    static double compute_from_exposure(double exposure, fix::Side /*side*/) {
        return std::abs(exposure);
    }
};

// NetValuePolicy - Returns signed value based on side (BID = positive, ASK = negative)
struct NetValuePolicy {
    template<typename StoredInputs>
    static double compute(const StoredInputs& inputs) {
        double exp = inputs.compute_exposure();
        return (inputs.side == fix::Side::BID) ? exp : -exp;
    }

    // For pre-trade check contribution calculation
    static double compute_from_exposure(double exposure, fix::Side side) {
        return (side == fix::Side::BID) ? exposure : -exposure;
    }
};

} // namespace metrics
