#pragma once

#include "base_exposure_metric.hpp"
#include "metric_policies.hpp"
#include "../aggregation/staged_metric.hpp"
#include "../aggregation/aggregation_core.hpp"
#include "../aggregation/key_extractors.hpp"
#include "../instrument/instrument.hpp"
#include "../engine/pre_trade_check.hpp"

namespace metrics {

// ============================================================================
// GrossVegaMetric - Single-value metric tracking absolute vega exposure
// ============================================================================
//
// Tracks only gross (absolute) vega exposure. Designed for use with the
// generic limit checking system where each metric has one value type.
//
// Template parameters:
//   Key: The grouping key type (GlobalKey, UnderlyerKey, etc.)
//   Context: Provides instrument accessor methods (vega, contract_size, etc.)
//   Instrument: Must satisfy the vega instrument requirements (vega support)
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//

template<typename Key, typename Context, typename Instrument, typename... Stages>
using GrossVegaMetric = BaseExposureMetric<
    Key, Context, Instrument,
    VegaInputPolicy<Context, Instrument>,
    GrossValuePolicy,
    engine::LimitType::GROSS_VEGA,
    Stages...
>;

// ============================================================================
// NetVegaMetric - Single-value metric tracking signed vega exposure
// ============================================================================
//
// Tracks only net (signed) vega exposure. Designed for use with the
// generic limit checking system where each metric has one value type.
// BID = +exposure, ASK = -exposure
//
// Template parameters:
//   Key: The grouping key type (GlobalKey, UnderlyerKey, etc.)
//   Context: Provides instrument accessor methods (vega, contract_size, etc.)
//   Instrument: Must satisfy the vega instrument requirements (vega support)
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//

template<typename Key, typename Context, typename Instrument, typename... Stages>
using NetVegaMetric = BaseExposureMetric<
    Key, Context, Instrument,
    VegaInputPolicy<Context, Instrument>,
    NetValuePolicy,
    engine::LimitType::NET_VEGA,
    Stages...
>;

// ============================================================================
// Type aliases for Gross/Net vega metrics
// ============================================================================

template<typename Context, typename Instrument, typename... Stages>
using GlobalGrossVegaMetric = GrossVegaMetric<aggregation::GlobalKey, Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using UnderlyerGrossVegaMetric = GrossVegaMetric<aggregation::UnderlyerKey, Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using GlobalNetVegaMetric = NetVegaMetric<aggregation::GlobalKey, Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using UnderlyerNetVegaMetric = NetVegaMetric<aggregation::UnderlyerKey, Context, Instrument, Stages...>;

} // namespace metrics
