#pragma once

// Include the generic aggregation engine
#include "generic_aggregation_engine.hpp"

// Include single-purpose metrics
#include "../metrics/order_count_metric.hpp"
#include "../metrics/notional_metric.hpp"
#include "../metrics/delta_metric.hpp"

namespace engine {

// ============================================================================
// Single-purpose metric type aliases
// ============================================================================
//
// Each metric tracks ONE grouping level and specifies stages via template params.
// This allows composing exactly the metrics needed for a given use case.
//

// Order count metrics - count orders per key
template<typename... Stages>
using InstrumentSideOrderCount = metrics::OrderCountMetric<aggregation::InstrumentSideKey, Stages...>;

// Quoted instrument count metric - count unique instruments per underlyer
template<typename... Stages>
using QuotedInstrumentCount = metrics::QuotedInstrumentCountMetric<Stages...>;

// Notional metrics - track notional exposure per key
template<typename Context, typename Instrument, typename... Stages>
using GlobalGrossNotional = metrics::GlobalGrossNotionalMetric<Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using StrategyGrossNotional = metrics::StrategyGrossNotionalMetric<Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using PortfolioGrossNotional = metrics::PortfolioGrossNotionalMetric<Context, Instrument, Stages...>;

// Delta metrics - track delta exposure per key
template<typename Context, typename Instrument, typename... Stages>
using GlobalGrossDelta = metrics::GlobalGrossDeltaMetric<Context, Instrument, Stages...>;

template<typename Context, typename Instrument, typename... Stages>
using UnderlyerGrossDelta = metrics::UnderlyerGrossDeltaMetric<Context, Instrument, Stages...>;

// ============================================================================
// Example engine configurations using single-purpose metrics
// ============================================================================

// Minimal engine with just order counts (no provider needed)
template<typename... Stages>
using OrderCountEngine = GenericRiskAggregationEngine<
    void,
    metrics::OrderCountMetric<aggregation::InstrumentSideKey, Stages...>
>;

// Engine with order counts and quoted instruments
template<typename... Stages>
using OrderAndQuotedEngine = GenericRiskAggregationEngine<
    void,
    metrics::OrderCountMetric<aggregation::InstrumentSideKey, Stages...>,
    metrics::QuotedInstrumentCountMetric<Stages...>
>;

// Engine with global gross notional only
template<typename Context, typename Instrument, typename... Stages>
using GlobalNotionalEngine = GenericRiskAggregationEngine<
    Context,
    metrics::GlobalGrossNotionalMetric<Context, Instrument, Stages...>
>;

// Engine with global gross delta only
template<typename Context, typename Instrument, typename... Stages>
using GlobalDeltaEngine = GenericRiskAggregationEngine<
    Context,
    metrics::GlobalGrossDeltaMetric<Context, Instrument, Stages...>
>;

// Engine with per-underlyer gross delta
template<typename Context, typename Instrument, typename... Stages>
using UnderlyerDeltaEngine = GenericRiskAggregationEngine<
    Context,
    metrics::UnderlyerGrossDeltaMetric<Context, Instrument, Stages...>
>;

// Full engine with global and underlyer gross delta
template<typename Context, typename Instrument, typename... Stages>
using FullDeltaEngine = GenericRiskAggregationEngine<
    Context,
    metrics::GlobalGrossDeltaMetric<Context, Instrument, Stages...>,
    metrics::UnderlyerGrossDeltaMetric<Context, Instrument, Stages...>
>;

// Comprehensive engine with multiple metrics
template<typename Context, typename Instrument, typename... Stages>
using ComprehensiveEngine = GenericRiskAggregationEngine<
    Context,
    metrics::OrderCountMetric<aggregation::InstrumentSideKey, Stages...>,
    metrics::QuotedInstrumentCountMetric<Stages...>,
    metrics::GlobalGrossNotionalMetric<Context, Instrument, Stages...>,
    metrics::StrategyGrossNotionalMetric<Context, Instrument, Stages...>,
    metrics::GlobalGrossDeltaMetric<Context, Instrument, Stages...>,
    metrics::UnderlyerGrossDeltaMetric<Context, Instrument, Stages...>
>;

} // namespace engine
