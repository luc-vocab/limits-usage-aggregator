#pragma once

// Include the generic aggregation engine
#include "generic_aggregation_engine.hpp"

// Include all standard metric types (each brings its AccessorMixin specialization)
#include "../metrics/delta_metrics.hpp"
#include "../metrics/order_count_metrics.hpp"
#include "../metrics/notional_metrics.hpp"

namespace engine {

// ============================================================================
// Type aliases for common configurations
// ============================================================================
//
// The engine is now templated on Provider type first, then metrics.
// All metrics now explicitly define their tracked stages via template parameters.
// Using AllStages tracks Position, Open, and InFlight stages.
//

using DefaultProvider = instrument::StaticInstrumentProvider;

// Standard engine with all built-in metrics (tracking all stages by default)
using RiskAggregationEngine = GenericRiskAggregationEngine<
    DefaultProvider,
    metrics::DeltaMetrics<DefaultProvider, aggregation::AllStages>,
    metrics::OrderCountMetrics<aggregation::AllStages>,
    metrics::NotionalMetrics<DefaultProvider, aggregation::AllStages>
>;

// Alias for backward compatibility
using StandardRiskEngine = RiskAggregationEngine;

// Engine with only delta metrics (all stages)
using DeltaOnlyEngine = GenericRiskAggregationEngine<
    DefaultProvider,
    metrics::DeltaMetrics<DefaultProvider, aggregation::AllStages>
>;

// Engine with only order count metrics (all stages, no provider needed)
using OrderCountOnlyEngine = GenericRiskAggregationEngine<
    DefaultProvider,
    metrics::OrderCountMetrics<aggregation::AllStages>
>;

// Engine with only notional metrics (all stages)
using NotionalOnlyEngine = GenericRiskAggregationEngine<
    DefaultProvider,
    metrics::NotionalMetrics<DefaultProvider, aggregation::AllStages>
>;

// ============================================================================
// Template alias for custom provider types
// ============================================================================
//
// Use these when you have a custom InstrumentProvider implementation:
//
//   using MyEngine = RiskAggregationEngineWith<MyCustomProvider>;
//

template<typename Provider>
using RiskAggregationEngineWith = GenericRiskAggregationEngine<
    Provider,
    metrics::DeltaMetrics<Provider, aggregation::AllStages>,
    metrics::OrderCountMetrics<aggregation::AllStages>,
    metrics::NotionalMetrics<Provider, aggregation::AllStages>
>;

template<typename Provider>
using DeltaOnlyEngineWith = GenericRiskAggregationEngine<
    Provider,
    metrics::DeltaMetrics<Provider, aggregation::AllStages>
>;

template<typename Provider>
using NotionalOnlyEngineWith = GenericRiskAggregationEngine<
    Provider,
    metrics::NotionalMetrics<Provider, aggregation::AllStages>
>;

// ============================================================================
// Template aliases for custom stage configurations
// ============================================================================
//
// Use these for engines that only track specific stages:
//
//   // Track only open and in-flight orders (no position tracking)
//   using ActiveOrdersEngine = StagedRiskEngine<
//       DefaultProvider,
//       aggregation::OpenStage, aggregation::InFlightStage>;
//

template<typename Provider, typename... Stages>
using StagedRiskEngine = GenericRiskAggregationEngine<
    Provider,
    metrics::DeltaMetrics<Provider, Stages...>,
    metrics::OrderCountMetrics<Stages...>,
    metrics::NotionalMetrics<Provider, Stages...>
>;

template<typename Provider, typename... Stages>
using StagedDeltaOnlyEngine = GenericRiskAggregationEngine<
    Provider,
    metrics::DeltaMetrics<Provider, Stages...>
>;

template<typename... Stages>
using StagedOrderCountOnlyEngine = GenericRiskAggregationEngine<
    void,
    metrics::OrderCountMetrics<Stages...>
>;

template<typename Provider, typename... Stages>
using StagedNotionalOnlyEngine = GenericRiskAggregationEngine<
    Provider,
    metrics::NotionalMetrics<Provider, Stages...>
>;

} // namespace engine
