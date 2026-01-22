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
template<typename Provider, typename... Stages>
using GlobalNotional = metrics::GlobalNotionalMetric<Provider, Stages...>;

template<typename Provider, typename... Stages>
using StrategyNotional = metrics::NotionalMetric<aggregation::StrategyKey, Provider, Stages...>;

template<typename Provider, typename... Stages>
using PortfolioNotional = metrics::NotionalMetric<aggregation::PortfolioKey, Provider, Stages...>;

// Delta metrics - track delta exposure per key
template<typename Provider, typename... Stages>
using GlobalDelta = metrics::GlobalDeltaMetric<Provider, Stages...>;

template<typename Provider, typename... Stages>
using UnderlyerDelta = metrics::UnderlyerDeltaMetric<Provider, Stages...>;

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

// Engine with global notional only
template<typename Provider, typename... Stages>
using GlobalNotionalEngine = GenericRiskAggregationEngine<
    Provider,
    metrics::GlobalNotionalMetric<Provider, Stages...>
>;

// Engine with global delta only
template<typename Provider, typename... Stages>
using GlobalDeltaEngine = GenericRiskAggregationEngine<
    Provider,
    metrics::GlobalDeltaMetric<Provider, Stages...>
>;

// Engine with per-underlyer delta
template<typename Provider, typename... Stages>
using UnderlyerDeltaEngine = GenericRiskAggregationEngine<
    Provider,
    metrics::UnderlyerDeltaMetric<Provider, Stages...>
>;

// Full engine with global and underlyer delta
template<typename Provider, typename... Stages>
using FullDeltaEngine = GenericRiskAggregationEngine<
    Provider,
    metrics::GlobalDeltaMetric<Provider, Stages...>,
    metrics::UnderlyerDeltaMetric<Provider, Stages...>
>;

// Comprehensive engine with multiple metrics
template<typename Provider, typename... Stages>
using ComprehensiveEngine = GenericRiskAggregationEngine<
    Provider,
    metrics::OrderCountMetric<aggregation::InstrumentSideKey, Stages...>,
    metrics::QuotedInstrumentCountMetric<Stages...>,
    metrics::GlobalNotionalMetric<Provider, Stages...>,
    metrics::NotionalMetric<aggregation::StrategyKey, Provider, Stages...>,
    metrics::GlobalDeltaMetric<Provider, Stages...>,
    metrics::UnderlyerDeltaMetric<Provider, Stages...>
>;

} // namespace engine
