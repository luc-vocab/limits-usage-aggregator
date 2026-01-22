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
// GrossDeltaMetric - Single-value metric tracking absolute delta exposure
// ============================================================================
//
// Tracks only gross (absolute) delta exposure. Designed for use with the
// generic limit checking system where each metric has one value type.
//
// Template parameters:
//   Key: The grouping key type (GlobalKey, UnderlyerKey, etc.)
//   Context: Provides instrument accessor methods (delta, contract_size, etc.)
//   Instrument: Must satisfy the option instrument requirements (delta support)
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//

template<typename Key, typename Context, typename Instrument, typename... Stages>
using GrossDeltaMetric = BaseExposureMetric<
    Key, Context, Instrument,
    DeltaInputPolicy<Context, Instrument>,
    GrossValuePolicy,
    engine::LimitType::GROSS_DELTA,
    Stages...
>;

// ============================================================================
// NetDeltaMetric - Single-value metric tracking signed delta exposure
// ============================================================================
//
// Tracks only net (signed) delta exposure. Designed for use with the
// generic limit checking system where each metric has one value type.
// BID = +exposure, ASK = -exposure
//
// Template parameters:
//   Key: The grouping key type (GlobalKey, UnderlyerKey, etc.)
//   Context: Provides instrument accessor methods (delta, contract_size, etc.)
//   Instrument: Must satisfy the option instrument requirements (delta support)
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//

template<typename Key, typename Context, typename Instrument, typename... Stages>
using NetDeltaMetric = BaseExposureMetric<
    Key, Context, Instrument,
    DeltaInputPolicy<Context, Instrument>,
    NetValuePolicy,
    engine::LimitType::NET_DELTA,
    Stages...
>;

// ============================================================================
// Type aliases for Gross/Net delta metrics
// ============================================================================

template<typename Context, typename Instrument, typename... Stages>
using GlobalGrossDeltaMetric = GrossDeltaMetric<aggregation::GlobalKey, Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using UnderlyerGrossDeltaMetric = GrossDeltaMetric<aggregation::UnderlyerKey, Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using GlobalNetDeltaMetric = NetDeltaMetric<aggregation::GlobalKey, Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using UnderlyerNetDeltaMetric = NetDeltaMetric<aggregation::UnderlyerKey, Context, Instrument, Stages...>;

} // namespace metrics
