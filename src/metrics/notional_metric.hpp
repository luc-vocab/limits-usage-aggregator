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
// GrossNotionalMetric - Single-value metric tracking absolute notional
// ============================================================================
//
// Tracks only gross (absolute) notional. Designed for use with the
// generic limit checking system where each metric has one value type.
// BID and ASK both add positive values (sum of |notional|).
//
// Template parameters:
//   Key: The grouping key type (GlobalKey, StrategyKey, PortfolioKey, etc.)
//   Context: Provides instrument accessor methods (contract_size, spot_price, fx_rate)
//   Instrument: Must satisfy the notional instrument requirements
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//

template<typename Key, typename Context, typename Instrument, typename... Stages>
using GrossNotionalMetric = BaseExposureMetric<
    Key, Context, Instrument,
    NotionalInputPolicy<Context, Instrument>,
    GrossValuePolicy,
    engine::LimitType::GLOBAL_GROSS_NOTIONAL,
    Stages...
>;

// ============================================================================
// NetNotionalMetric - Single-value metric tracking signed notional
// ============================================================================
//
// Tracks only net (signed) notional. Designed for use with the
// generic limit checking system where each metric has one value type.
// BID = +notional, ASK = -notional
//
// Template parameters:
//   Key: The grouping key type (GlobalKey, StrategyKey, PortfolioKey, etc.)
//   Context: Provides instrument accessor methods (contract_size, spot_price, fx_rate)
//   Instrument: Must satisfy the notional instrument requirements
//   Stages...: Stage types to track (PositionStage, OpenStage, InFlightStage, or AllStages)
//

template<typename Key, typename Context, typename Instrument, typename... Stages>
using NetNotionalMetric = BaseExposureMetric<
    Key, Context, Instrument,
    NotionalInputPolicy<Context, Instrument>,
    NetValuePolicy,
    engine::LimitType::GLOBAL_NET_NOTIONAL,
    Stages...
>;

// ============================================================================
// Type aliases for Gross/Net notional metrics at common grouping levels
// ============================================================================

template<typename Context, typename Instrument, typename... Stages>
using GlobalGrossNotionalMetric = GrossNotionalMetric<aggregation::GlobalKey, Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using GlobalNetNotionalMetric = NetNotionalMetric<aggregation::GlobalKey, Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using StrategyGrossNotionalMetric = GrossNotionalMetric<aggregation::StrategyKey, Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using StrategyNetNotionalMetric = NetNotionalMetric<aggregation::StrategyKey, Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using PortfolioGrossNotionalMetric = GrossNotionalMetric<aggregation::PortfolioKey, Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using PortfolioNetNotionalMetric = NetNotionalMetric<aggregation::PortfolioKey, Context, Instrument, Stages...>;

} // namespace metrics
