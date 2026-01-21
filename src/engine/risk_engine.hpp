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

// Standard engine with all built-in metrics
using RiskAggregationEngine = GenericRiskAggregationEngine<
    metrics::DeltaMetrics,
    metrics::OrderCountMetrics,
    metrics::NotionalMetrics
>;

// Alias for backward compatibility
using StandardRiskEngine = RiskAggregationEngine;

// Engine with only delta metrics
using DeltaOnlyEngine = GenericRiskAggregationEngine<metrics::DeltaMetrics>;

// Engine with only order count metrics
using OrderCountOnlyEngine = GenericRiskAggregationEngine<metrics::OrderCountMetrics>;

// Engine with only notional metrics
using NotionalOnlyEngine = GenericRiskAggregationEngine<metrics::NotionalMetrics>;

} // namespace engine
