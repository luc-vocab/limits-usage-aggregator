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
// Use the default StaticInstrumentProvider for convenience aliases.
//

using DefaultProvider = instrument::StaticInstrumentProvider;

// Standard engine with all built-in metrics (using default provider)
using RiskAggregationEngine = GenericRiskAggregationEngine<
    DefaultProvider,
    metrics::DeltaMetrics<DefaultProvider>,
    metrics::OrderCountMetrics,
    metrics::NotionalMetrics<DefaultProvider>
>;

// Alias for backward compatibility
using StandardRiskEngine = RiskAggregationEngine;

// Engine with only delta metrics
using DeltaOnlyEngine = GenericRiskAggregationEngine<
    DefaultProvider,
    metrics::DeltaMetrics<DefaultProvider>
>;

// Engine with only order count metrics (no provider needed)
using OrderCountOnlyEngine = GenericRiskAggregationEngine<
    DefaultProvider,
    metrics::OrderCountMetrics
>;

// Engine with only notional metrics
using NotionalOnlyEngine = GenericRiskAggregationEngine<
    DefaultProvider,
    metrics::NotionalMetrics<DefaultProvider>
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
    metrics::DeltaMetrics<Provider>,
    metrics::OrderCountMetrics,
    metrics::NotionalMetrics<Provider>
>;

template<typename Provider>
using DeltaOnlyEngineWith = GenericRiskAggregationEngine<
    Provider,
    metrics::DeltaMetrics<Provider>
>;

template<typename Provider>
using NotionalOnlyEngineWith = GenericRiskAggregationEngine<
    Provider,
    metrics::NotionalMetrics<Provider>
>;

} // namespace engine
