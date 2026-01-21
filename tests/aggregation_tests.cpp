#include <gtest/gtest.h>
#include "../src/aggregation/grouping.hpp"
#include "../src/aggregation/aggregation_traits.hpp"
#include "../src/aggregation/aggregation_core.hpp"
#include "../src/metrics/delta_metrics.hpp"
#include "../src/metrics/order_count_metrics.hpp"
#include "../src/metrics/notional_metrics.hpp"

using namespace aggregation;

// ============================================================================
// Grouping Key Tests
// ============================================================================

TEST(GlobalKeyTest, AllKeysEqual) {
    GlobalKey k1, k2;
    EXPECT_EQ(k1, k2);
}

TEST(UnderlyerKeyTest, Equality) {
    UnderlyerKey k1{"AAPL"};
    UnderlyerKey k2{"AAPL"};
    UnderlyerKey k3{"MSFT"};

    EXPECT_EQ(k1, k2);
    EXPECT_NE(k1, k3);
}

TEST(InstrumentKeyTest, Equality) {
    InstrumentKey k1{"AAPL230120C150"};
    InstrumentKey k2{"AAPL230120C150"};
    InstrumentKey k3{"AAPL230120P150"};

    EXPECT_EQ(k1, k2);
    EXPECT_NE(k1, k3);
}

TEST(InstrumentSideKeyTest, Equality) {
    InstrumentSideKey k1{"AAPL", 1};
    InstrumentSideKey k2{"AAPL", 1};
    InstrumentSideKey k3{"AAPL", 2};
    InstrumentSideKey k4{"MSFT", 1};

    EXPECT_EQ(k1, k2);
    EXPECT_NE(k1, k3);
    EXPECT_NE(k1, k4);
}

TEST(KeyHashingTest, UnorderedMapUsage) {
    std::unordered_map<UnderlyerKey, int> map;
    map[UnderlyerKey{"AAPL"}] = 1;
    map[UnderlyerKey{"MSFT"}] = 2;
    map[UnderlyerKey{"GOOG"}] = 3;

    EXPECT_EQ(map[UnderlyerKey{"AAPL"}], 1);
    EXPECT_EQ(map[UnderlyerKey{"MSFT"}], 2);
    EXPECT_EQ(map[UnderlyerKey{"GOOG"}], 3);
    EXPECT_EQ(map.size(), 3u);
}

// ============================================================================
// Combiner Tests
// ============================================================================

TEST(SumCombinerTest, DoubleOperations) {
    using Combiner = SumCombiner<double>;
    EXPECT_DOUBLE_EQ(Combiner::identity(), 0.0);
    EXPECT_DOUBLE_EQ(Combiner::combine(10.0, 5.0), 15.0);
    EXPECT_DOUBLE_EQ(Combiner::uncombine(15.0, 5.0), 10.0);
}

TEST(CountCombinerTest, Operations) {
    EXPECT_EQ(CountCombiner::identity(), 0);
    EXPECT_EQ(CountCombiner::combine(10, 3), 13);
    EXPECT_EQ(CountCombiner::uncombine(13, 3), 10);
}

TEST(DeltaCombinerTest, Operations) {
    DeltaValue v1{100.0, 50.0};
    DeltaValue v2{25.0, -10.0};

    auto combined = DeltaCombiner::combine(v1, v2);
    EXPECT_DOUBLE_EQ(combined.gross, 125.0);
    EXPECT_DOUBLE_EQ(combined.net, 40.0);

    auto uncombined = DeltaCombiner::uncombine(combined, v2);
    EXPECT_DOUBLE_EQ(uncombined.gross, 100.0);
    EXPECT_DOUBLE_EQ(uncombined.net, 50.0);
}

// ============================================================================
// AggregationBucket Tests
// ============================================================================

class AggregationBucketTest : public ::testing::Test {
protected:
    AggregationBucket<UnderlyerKey, SumCombiner<double>> double_bucket;
    AggregationBucket<UnderlyerKey, CountCombiner> count_bucket;
};

TEST_F(AggregationBucketTest, AddAndGet) {
    double_bucket.add(UnderlyerKey{"AAPL"}, 100.0);
    double_bucket.add(UnderlyerKey{"MSFT"}, 200.0);
    double_bucket.add(UnderlyerKey{"AAPL"}, 50.0);

    EXPECT_DOUBLE_EQ(double_bucket.get(UnderlyerKey{"AAPL"}), 150.0);
    EXPECT_DOUBLE_EQ(double_bucket.get(UnderlyerKey{"MSFT"}), 200.0);
    EXPECT_DOUBLE_EQ(double_bucket.get(UnderlyerKey{"GOOG"}), 0.0);  // Not present
}

TEST_F(AggregationBucketTest, Remove) {
    double_bucket.add(UnderlyerKey{"AAPL"}, 100.0);
    double_bucket.remove(UnderlyerKey{"AAPL"}, 40.0);

    EXPECT_DOUBLE_EQ(double_bucket.get(UnderlyerKey{"AAPL"}), 60.0);
}

TEST_F(AggregationBucketTest, Update) {
    double_bucket.add(UnderlyerKey{"AAPL"}, 100.0);
    double_bucket.update(UnderlyerKey{"AAPL"}, 100.0, 150.0);

    EXPECT_DOUBLE_EQ(double_bucket.get(UnderlyerKey{"AAPL"}), 150.0);
}

TEST_F(AggregationBucketTest, Contains) {
    count_bucket.add(UnderlyerKey{"AAPL"}, 1);

    EXPECT_TRUE(count_bucket.contains(UnderlyerKey{"AAPL"}));
    EXPECT_FALSE(count_bucket.contains(UnderlyerKey{"MSFT"}));
}

TEST_F(AggregationBucketTest, SizeAndKeys) {
    count_bucket.add(UnderlyerKey{"AAPL"}, 1);
    count_bucket.add(UnderlyerKey{"MSFT"}, 2);
    count_bucket.add(UnderlyerKey{"GOOG"}, 3);

    EXPECT_EQ(count_bucket.size(), 3u);

    auto keys = count_bucket.keys();
    EXPECT_EQ(keys.size(), 3u);
}

TEST_F(AggregationBucketTest, Clear) {
    count_bucket.add(UnderlyerKey{"AAPL"}, 1);
    count_bucket.add(UnderlyerKey{"MSFT"}, 2);
    count_bucket.clear();

    EXPECT_EQ(count_bucket.size(), 0u);
    EXPECT_EQ(count_bucket.get(UnderlyerKey{"AAPL"}), 0);
}

TEST_F(AggregationBucketTest, CleanupOnZero) {
    count_bucket.add(UnderlyerKey{"AAPL"}, 5);
    EXPECT_EQ(count_bucket.size(), 1u);

    count_bucket.remove(UnderlyerKey{"AAPL"}, 5);
    EXPECT_EQ(count_bucket.size(), 0u);  // Key removed when value returns to identity
}

TEST(DeltaBucketTest, DeltaValues) {
    AggregationBucket<GlobalKey, DeltaCombiner> bucket;

    bucket.add(GlobalKey::instance(), DeltaValue{100.0, 50.0});
    bucket.add(GlobalKey::instance(), DeltaValue{50.0, -30.0});

    auto value = bucket.get(GlobalKey::instance());
    EXPECT_DOUBLE_EQ(value.gross, 150.0);
    EXPECT_DOUBLE_EQ(value.net, 20.0);
}

// ============================================================================
// AggregationEngine Tests
// ============================================================================

TEST(AggregationEngineTest, MultipleBuckets) {
    using Engine = AggregationEngine<
        GlobalDeltaBucket,
        UnderlyerDeltaBucket,
        StrategyNotionalBucket
    >;

    Engine engine;

    auto& global = engine.get<GlobalDeltaBucket>();
    auto& underlyer = engine.get<UnderlyerDeltaBucket>();
    auto& strategy = engine.get<StrategyNotionalBucket>();

    global.add(GlobalKey::instance(), DeltaValue{100.0, 50.0});
    underlyer.add(UnderlyerKey{"AAPL"}, DeltaValue{100.0, 50.0});
    strategy.add(StrategyKey{"STRAT1"}, 10000.0);

    EXPECT_DOUBLE_EQ(global.get(GlobalKey::instance()).gross, 100.0);
    EXPECT_DOUBLE_EQ(underlyer.get(UnderlyerKey{"AAPL"}).net, 50.0);
    EXPECT_DOUBLE_EQ(strategy.get(StrategyKey{"STRAT1"}), 10000.0);
}

TEST(AggregationEngineTest, Clear) {
    using Engine = AggregationEngine<GlobalDeltaBucket, StrategyNotionalBucket>;

    Engine engine;
    engine.get<GlobalDeltaBucket>().add(GlobalKey::instance(), DeltaValue{100.0, 50.0});
    engine.get<StrategyNotionalBucket>().add(StrategyKey{"STRAT1"}, 10000.0);

    engine.clear();

    EXPECT_DOUBLE_EQ(engine.get<GlobalDeltaBucket>().get(GlobalKey::instance()).gross, 0.0);
    EXPECT_DOUBLE_EQ(engine.get<StrategyNotionalBucket>().get(StrategyKey{"STRAT1"}), 0.0);
}

// ============================================================================
// Delta Metrics Tests - Parameterized for side variations
// ============================================================================

struct DeltaMetricsAddOrderParam {
    std::string name;
    fix::Side side;
    double expected_gross;
    double expected_net;
};

class DeltaMetricsAddOrderTest : public ::testing::TestWithParam<DeltaMetricsAddOrderParam> {};

TEST_P(DeltaMetricsAddOrderTest, AddsOrderCorrectly) {
    auto param = GetParam();
    metrics::DeltaMetrics metrics;

    metrics.add_order("AAPL", 100.0, param.side);

    EXPECT_DOUBLE_EQ(metrics.global_gross_delta(), param.expected_gross);
    EXPECT_DOUBLE_EQ(metrics.global_net_delta(), param.expected_net);
}

INSTANTIATE_TEST_SUITE_P(
    SideVariations,
    DeltaMetricsAddOrderTest,
    ::testing::Values(
        DeltaMetricsAddOrderParam{"BidOrder", fix::Side::BID, 100.0, 100.0},
        DeltaMetricsAddOrderParam{"AskOrder", fix::Side::ASK, 100.0, -100.0}
    ),
    [](const ::testing::TestParamInfo<DeltaMetricsAddOrderParam>& info) {
        return info.param.name;
    }
);

class DeltaMetricsTest : public ::testing::Test {
protected:
    metrics::DeltaMetrics metrics;
};

TEST_F(DeltaMetricsTest, MultipleOrders) {
    metrics.add_order("AAPL", 100.0, fix::Side::BID);
    metrics.add_order("AAPL", 50.0, fix::Side::ASK);
    metrics.add_order("MSFT", 75.0, fix::Side::BID);

    EXPECT_DOUBLE_EQ(metrics.global_gross_delta(), 225.0);  // 100 + 50 + 75
    EXPECT_DOUBLE_EQ(metrics.global_net_delta(), 125.0);    // 100 - 50 + 75
    EXPECT_DOUBLE_EQ(metrics.underlyer_gross_delta("AAPL"), 150.0);
    EXPECT_DOUBLE_EQ(metrics.underlyer_net_delta("AAPL"), 50.0);  // 100 - 50
    EXPECT_DOUBLE_EQ(metrics.underlyer_gross_delta("MSFT"), 75.0);
}

TEST_F(DeltaMetricsTest, RemoveOrder) {
    metrics.add_order("AAPL", 100.0, fix::Side::BID);
    metrics.remove_order("AAPL", 100.0, fix::Side::BID);

    EXPECT_DOUBLE_EQ(metrics.global_gross_delta(), 0.0);
    EXPECT_DOUBLE_EQ(metrics.global_net_delta(), 0.0);
}

TEST_F(DeltaMetricsTest, UpdateOrder) {
    metrics.add_order("AAPL", 100.0, fix::Side::BID);
    metrics.update_order("AAPL", 100.0, 150.0, fix::Side::BID);

    EXPECT_DOUBLE_EQ(metrics.global_gross_delta(), 150.0);
    EXPECT_DOUBLE_EQ(metrics.global_net_delta(), 150.0);
}

TEST_F(DeltaMetricsTest, PartialFill) {
    metrics.add_order("AAPL", 100.0, fix::Side::BID);
    metrics.partial_fill("AAPL", 40.0, fix::Side::BID);

    EXPECT_DOUBLE_EQ(metrics.global_gross_delta(), 60.0);
    EXPECT_DOUBLE_EQ(metrics.global_net_delta(), 60.0);
}

// ============================================================================
// Order Count Metrics Tests
// ============================================================================

class OrderCountMetricsTest : public ::testing::Test {
protected:
    metrics::OrderCountMetrics metrics;
};

TEST_F(OrderCountMetricsTest, AddOrders) {
    metrics.add_order("AAPL230120C150", "AAPL", fix::Side::BID);
    metrics.add_order("AAPL230120C150", "AAPL", fix::Side::BID);
    metrics.add_order("AAPL230120C150", "AAPL", fix::Side::ASK);

    EXPECT_EQ(metrics.bid_order_count("AAPL230120C150"), 2);
    EXPECT_EQ(metrics.ask_order_count("AAPL230120C150"), 1);
    EXPECT_EQ(metrics.total_order_count("AAPL230120C150"), 3);
}

TEST_F(OrderCountMetricsTest, RemoveOrders) {
    metrics.add_order("AAPL230120C150", "AAPL", fix::Side::BID);
    metrics.add_order("AAPL230120C150", "AAPL", fix::Side::BID);
    metrics.remove_order("AAPL230120C150", "AAPL", fix::Side::BID);

    EXPECT_EQ(metrics.bid_order_count("AAPL230120C150"), 1);
}

TEST_F(OrderCountMetricsTest, QuotedInstrumentsCount) {
    // Add orders for multiple instruments under AAPL
    metrics.add_order("AAPL230120C150", "AAPL", fix::Side::BID);
    metrics.add_order("AAPL230120P150", "AAPL", fix::Side::ASK);
    metrics.add_order("AAPL230217C160", "AAPL", fix::Side::BID);

    // Add orders for MSFT
    metrics.add_order("MSFT230120C250", "MSFT", fix::Side::BID);

    EXPECT_EQ(metrics.quoted_instruments_count("AAPL"), 3);
    EXPECT_EQ(metrics.quoted_instruments_count("MSFT"), 1);
}

TEST_F(OrderCountMetricsTest, QuotedInstrumentsDecrement) {
    // Add two orders for one instrument
    metrics.add_order("AAPL230120C150", "AAPL", fix::Side::BID);
    metrics.add_order("AAPL230120C150", "AAPL", fix::Side::ASK);

    EXPECT_EQ(metrics.quoted_instruments_count("AAPL"), 1);

    // Remove one - instrument still quoted
    metrics.remove_order("AAPL230120C150", "AAPL", fix::Side::BID);
    EXPECT_EQ(metrics.quoted_instruments_count("AAPL"), 1);

    // Remove last - instrument no longer quoted
    metrics.remove_order("AAPL230120C150", "AAPL", fix::Side::ASK);
    EXPECT_EQ(metrics.quoted_instruments_count("AAPL"), 0);
}

// ============================================================================
// Notional Metrics Tests
// ============================================================================

class NotionalMetricsTest : public ::testing::Test {
protected:
    metrics::NotionalMetrics metrics;
};

TEST_F(NotionalMetricsTest, AddOrders) {
    metrics.add_order("STRAT1", "PORT1", 10000.0);
    metrics.add_order("STRAT1", "PORT1", 5000.0);
    metrics.add_order("STRAT2", "PORT1", 8000.0);

    EXPECT_DOUBLE_EQ(metrics.global_notional(), 23000.0);
    EXPECT_DOUBLE_EQ(metrics.strategy_notional("STRAT1"), 15000.0);
    EXPECT_DOUBLE_EQ(metrics.strategy_notional("STRAT2"), 8000.0);
    EXPECT_DOUBLE_EQ(metrics.portfolio_notional("PORT1"), 23000.0);
}

TEST_F(NotionalMetricsTest, RemoveOrders) {
    metrics.add_order("STRAT1", "PORT1", 10000.0);
    metrics.remove_order("STRAT1", "PORT1", 10000.0);

    EXPECT_DOUBLE_EQ(metrics.global_notional(), 0.0);
    EXPECT_DOUBLE_EQ(metrics.strategy_notional("STRAT1"), 0.0);
}

TEST_F(NotionalMetricsTest, UpdateOrder) {
    metrics.add_order("STRAT1", "PORT1", 10000.0);
    metrics.update_order("STRAT1", "PORT1", 10000.0, 15000.0);

    EXPECT_DOUBLE_EQ(metrics.strategy_notional("STRAT1"), 15000.0);
}

TEST_F(NotionalMetricsTest, PartialFill) {
    metrics.add_order("STRAT1", "PORT1", 10000.0);
    metrics.partial_fill("STRAT1", "PORT1", 4000.0);

    EXPECT_DOUBLE_EQ(metrics.strategy_notional("STRAT1"), 6000.0);
}

TEST_F(NotionalMetricsTest, EmptyStrategy) {
    // Order with empty strategy should only update global and portfolio
    metrics.add_order("", "PORT1", 10000.0);

    EXPECT_DOUBLE_EQ(metrics.global_notional(), 10000.0);
    EXPECT_DOUBLE_EQ(metrics.strategy_notional(""), 0.0);  // Empty string strategy not tracked
    EXPECT_DOUBLE_EQ(metrics.portfolio_notional("PORT1"), 10000.0);
}

// ============================================================================
// Generic Engine Template Tests
// ============================================================================

#include "../src/engine/risk_engine.hpp"

TEST(GenericEngineTest, EmptyEngine) {
    // Engine with no metrics
    engine::GenericRiskAggregationEngine<> engine;

    EXPECT_EQ(engine.metric_count(), 0u);
    EXPECT_EQ(engine.active_order_count(), 0u);
}

TEST(GenericEngineTest, DeltaOnlyEngine) {
    engine::DeltaOnlyEngine engine;

    EXPECT_EQ(engine.metric_count(), 1u);
    EXPECT_TRUE(engine.has_metric<metrics::DeltaMetrics>());
    EXPECT_FALSE(engine.has_metric<metrics::OrderCountMetrics>());
    EXPECT_FALSE(engine.has_metric<metrics::NotionalMetrics>());

    // Accessor methods from mixin should be available
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 0.0);
    EXPECT_DOUBLE_EQ(engine.global_net_delta(), 0.0);

    // Process an order
    fix::NewOrderSingle order;
    order.key.cl_ord_id = "ORD001";
    order.symbol = "AAPL";
    order.underlyer = "AAPL";
    order.side = fix::Side::BID;
    order.price = 100.0;
    order.quantity = 10.0;
    order.delta = 0.5;
    order.strategy_id = "STRAT1";
    order.portfolio_id = "PORT1";

    engine.on_new_order_single(order);

    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 5.0);  // 10 * 0.5
    EXPECT_DOUBLE_EQ(engine.global_net_delta(), 5.0);    // BID = positive
    EXPECT_DOUBLE_EQ(engine.underlyer_gross_delta("AAPL"), 5.0);
}

TEST(GenericEngineTest, OrderCountOnlyEngine) {
    engine::OrderCountOnlyEngine engine;

    EXPECT_EQ(engine.metric_count(), 1u);
    EXPECT_FALSE(engine.has_metric<metrics::DeltaMetrics>());
    EXPECT_TRUE(engine.has_metric<metrics::OrderCountMetrics>());
    EXPECT_FALSE(engine.has_metric<metrics::NotionalMetrics>());

    // Accessor methods from mixin should be available
    EXPECT_EQ(engine.bid_order_count("AAPL"), 0);
    EXPECT_EQ(engine.ask_order_count("AAPL"), 0);

    // Process an order
    fix::NewOrderSingle order;
    order.key.cl_ord_id = "ORD001";
    order.symbol = "AAPL";
    order.underlyer = "AAPL";
    order.side = fix::Side::BID;
    order.price = 100.0;
    order.quantity = 10.0;
    order.delta = 0.5;
    order.strategy_id = "STRAT1";
    order.portfolio_id = "PORT1";

    engine.on_new_order_single(order);

    EXPECT_EQ(engine.bid_order_count("AAPL"), 1);
    EXPECT_EQ(engine.ask_order_count("AAPL"), 0);
    EXPECT_EQ(engine.quoted_instruments_count("AAPL"), 1);
}

TEST(GenericEngineTest, NotionalOnlyEngine) {
    engine::NotionalOnlyEngine engine;

    EXPECT_EQ(engine.metric_count(), 1u);
    EXPECT_FALSE(engine.has_metric<metrics::DeltaMetrics>());
    EXPECT_FALSE(engine.has_metric<metrics::OrderCountMetrics>());
    EXPECT_TRUE(engine.has_metric<metrics::NotionalMetrics>());

    // Accessor methods from mixin should be available
    EXPECT_DOUBLE_EQ(engine.global_notional(), 0.0);
    EXPECT_DOUBLE_EQ(engine.strategy_notional("STRAT1"), 0.0);

    // Process an order
    fix::NewOrderSingle order;
    order.key.cl_ord_id = "ORD001";
    order.symbol = "AAPL";
    order.underlyer = "AAPL";
    order.side = fix::Side::BID;
    order.price = 100.0;
    order.quantity = 10.0;
    order.delta = 0.5;
    order.strategy_id = "STRAT1";
    order.portfolio_id = "PORT1";

    engine.on_new_order_single(order);

    EXPECT_DOUBLE_EQ(engine.global_notional(), 1000.0);  // 10 * 100
    EXPECT_DOUBLE_EQ(engine.strategy_notional("STRAT1"), 1000.0);
    EXPECT_DOUBLE_EQ(engine.portfolio_notional("PORT1"), 1000.0);
}

TEST(GenericEngineTest, CustomMetricCombination) {
    // Engine with only Delta and Notional metrics
    using DeltaNotionalEngine = engine::GenericRiskAggregationEngine<
        metrics::DeltaMetrics,
        metrics::NotionalMetrics
    >;

    DeltaNotionalEngine engine;

    EXPECT_EQ(engine.metric_count(), 2u);
    EXPECT_TRUE(engine.has_metric<metrics::DeltaMetrics>());
    EXPECT_FALSE(engine.has_metric<metrics::OrderCountMetrics>());
    EXPECT_TRUE(engine.has_metric<metrics::NotionalMetrics>());

    // Both delta and notional accessors should be available
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 0.0);
    EXPECT_DOUBLE_EQ(engine.global_notional(), 0.0);

    // Process an order
    fix::NewOrderSingle order;
    order.key.cl_ord_id = "ORD001";
    order.symbol = "AAPL";
    order.underlyer = "AAPL";
    order.side = fix::Side::ASK;
    order.price = 50.0;
    order.quantity = 20.0;
    order.delta = 0.3;
    order.strategy_id = "STRAT1";
    order.portfolio_id = "PORT1";

    engine.on_new_order_single(order);

    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 6.0);   // 20 * 0.3
    EXPECT_DOUBLE_EQ(engine.global_net_delta(), -6.0);    // ASK = negative
    EXPECT_DOUBLE_EQ(engine.global_notional(), 1000.0);   // 20 * 50
}

TEST(GenericEngineTest, StandardEngineHasAllAccessors) {
    engine::RiskAggregationEngine engine;

    EXPECT_EQ(engine.metric_count(), 3u);
    EXPECT_TRUE(engine.has_metric<metrics::DeltaMetrics>());
    EXPECT_TRUE(engine.has_metric<metrics::OrderCountMetrics>());
    EXPECT_TRUE(engine.has_metric<metrics::NotionalMetrics>());

    // All accessor methods should be available
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 0.0);
    EXPECT_DOUBLE_EQ(engine.global_net_delta(), 0.0);
    EXPECT_EQ(engine.bid_order_count("AAPL"), 0);
    EXPECT_EQ(engine.ask_order_count("AAPL"), 0);
    EXPECT_DOUBLE_EQ(engine.global_notional(), 0.0);

    // Process an order
    fix::NewOrderSingle order;
    order.key.cl_ord_id = "ORD001";
    order.symbol = "AAPL";
    order.underlyer = "AAPL";
    order.side = fix::Side::BID;
    order.price = 100.0;
    order.quantity = 10.0;
    order.delta = 0.5;
    order.strategy_id = "STRAT1";
    order.portfolio_id = "PORT1";

    engine.on_new_order_single(order);

    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 5.0);
    EXPECT_EQ(engine.bid_order_count("AAPL"), 1);
    EXPECT_DOUBLE_EQ(engine.global_notional(), 1000.0);
}

TEST(GenericEngineTest, GetMetricAccess) {
    engine::RiskAggregationEngine engine;

    // Direct metric access via get_metric
    auto& delta = engine.get_metric<metrics::DeltaMetrics>();
    auto& order_count = engine.get_metric<metrics::OrderCountMetrics>();
    auto& notional = engine.get_metric<metrics::NotionalMetrics>();

    // Process an order
    fix::NewOrderSingle order;
    order.key.cl_ord_id = "ORD001";
    order.symbol = "AAPL";
    order.underlyer = "AAPL";
    order.side = fix::Side::BID;
    order.price = 100.0;
    order.quantity = 10.0;
    order.delta = 0.5;
    order.strategy_id = "STRAT1";
    order.portfolio_id = "PORT1";

    engine.on_new_order_single(order);

    // Access metrics directly
    EXPECT_DOUBLE_EQ(delta.global_gross_delta(), 5.0);
    EXPECT_EQ(order_count.bid_order_count("AAPL"), 1);
    EXPECT_DOUBLE_EQ(notional.global_notional(), 1000.0);
}
