#include <gtest/gtest.h>
#include "../src/aggregation/grouping.hpp"
#include "../src/aggregation/aggregation_traits.hpp"
#include "../src/aggregation/aggregation_core.hpp"
#include "../src/instrument/instrument.hpp"
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
// Delta Metrics Tests - Quantity-based tracking with InstrumentProvider
// ============================================================================

// Test fixture with InstrumentProvider for delta computation
class DeltaMetricsTest : public ::testing::Test {
protected:
    metrics::DeltaMetrics metrics;
    instrument::StaticInstrumentProvider provider;

    void SetUp() override {
        // Add test instruments with delta=1.0 for simple math
        // Using add_equity: spot=1.0 so delta_exp = qty * delta(1.0) * contract_size(1) * spot(1) * fx(1) = qty
        provider.add_equity("AAPL", 1.0);
        provider.add_equity("MSFT", 1.0);
        metrics.set_instrument_provider(&provider);
    }
};

TEST_F(DeltaMetricsTest, AddBidOrder) {
    metrics.add_order("AAPL", "AAPL", 100, fix::Side::BID);
    EXPECT_DOUBLE_EQ(metrics.global_gross_delta(), 100.0);
    EXPECT_DOUBLE_EQ(metrics.global_net_delta(), 100.0);
}

TEST_F(DeltaMetricsTest, AddAskOrder) {
    metrics.add_order("AAPL", "AAPL", 100, fix::Side::ASK);
    EXPECT_DOUBLE_EQ(metrics.global_gross_delta(), 100.0);
    EXPECT_DOUBLE_EQ(metrics.global_net_delta(), -100.0);
}

TEST_F(DeltaMetricsTest, MultipleOrders) {
    metrics.add_order("AAPL", "AAPL", 100, fix::Side::BID);
    metrics.add_order("AAPL", "AAPL", 50, fix::Side::ASK);
    metrics.add_order("MSFT", "MSFT", 75, fix::Side::BID);

    EXPECT_DOUBLE_EQ(metrics.global_gross_delta(), 225.0);  // 100 + 50 + 75
    EXPECT_DOUBLE_EQ(metrics.global_net_delta(), 125.0);    // 100 - 50 + 75
    EXPECT_DOUBLE_EQ(metrics.underlyer_gross_delta("AAPL"), 150.0);
    EXPECT_DOUBLE_EQ(metrics.underlyer_net_delta("AAPL"), 50.0);  // 100 - 50
    EXPECT_DOUBLE_EQ(metrics.underlyer_gross_delta("MSFT"), 75.0);
}

TEST_F(DeltaMetricsTest, RemoveOrder) {
    metrics.add_order("AAPL", "AAPL", 100, fix::Side::BID);
    metrics.remove_order("AAPL", "AAPL", 100, fix::Side::BID);

    EXPECT_DOUBLE_EQ(metrics.global_gross_delta(), 0.0);
    EXPECT_DOUBLE_EQ(metrics.global_net_delta(), 0.0);
}

TEST_F(DeltaMetricsTest, UpdateOrder) {
    metrics.add_order("AAPL", "AAPL", 100, fix::Side::BID);
    metrics.update_order("AAPL", "AAPL", 100, 150, fix::Side::BID);

    EXPECT_DOUBLE_EQ(metrics.global_gross_delta(), 150.0);
    EXPECT_DOUBLE_EQ(metrics.global_net_delta(), 150.0);
}

TEST_F(DeltaMetricsTest, PartialFill) {
    metrics.add_order("AAPL", "AAPL", 100, fix::Side::BID);
    metrics.partial_fill("AAPL", "AAPL", 40, fix::Side::BID);

    EXPECT_DOUBLE_EQ(metrics.global_gross_delta(), 60.0);
    EXPECT_DOUBLE_EQ(metrics.global_net_delta(), 60.0);
}

TEST_F(DeltaMetricsTest, QuantityAccessors) {
    metrics.add_order("AAPL", "AAPL", 100, fix::Side::BID);
    metrics.add_order("AAPL", "AAPL", 50, fix::Side::ASK);

    EXPECT_EQ(metrics.global_bid_quantity(), 100);
    EXPECT_EQ(metrics.global_ask_quantity(), 50);
    EXPECT_EQ(metrics.global_quantity(), 150);
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
// Notional Metrics Tests - Quantity-based tracking with InstrumentProvider
// ============================================================================

class NotionalMetricsTest : public ::testing::Test {
protected:
    metrics::NotionalMetrics metrics;
    instrument::StaticInstrumentProvider provider;

    void SetUp() override {
        // Add test instruments
        // For notional: qty * contract_size * spot * fx
        // Using add_option: spot=100.0, underlyer_spot=100.0, delta=0.5, contract_size=1.0 (override default 100)
        // So notional = qty * 1.0 * 100.0 * 1.0 = qty * 100
        provider.add_option("AAPL_OPT1", "AAPL", 100.0, 100.0, 0.5, 1.0, 1.0);
        provider.add_option("AAPL_OPT2", "AAPL", 100.0, 100.0, 0.3, 1.0, 1.0);
        provider.add_option("MSFT_OPT1", "MSFT", 80.0, 80.0, 0.4, 1.0, 1.0);
        metrics.set_instrument_provider(&provider);
    }
};

TEST_F(NotionalMetricsTest, AddOrders) {
    // notional = quantity * contract_size * spot * fx_rate = quantity * 100
    metrics.add_order("AAPL_OPT1", "STRAT1", "PORT1", 100);  // 100 * 100 = 10000
    metrics.add_order("AAPL_OPT1", "STRAT1", "PORT1", 50);   // 50 * 100 = 5000
    metrics.add_order("MSFT_OPT1", "STRAT2", "PORT1", 100);  // 100 * 80 = 8000

    EXPECT_DOUBLE_EQ(metrics.global_notional(), 23000.0);
    EXPECT_DOUBLE_EQ(metrics.strategy_notional("STRAT1"), 15000.0);
    EXPECT_DOUBLE_EQ(metrics.strategy_notional("STRAT2"), 8000.0);
    EXPECT_DOUBLE_EQ(metrics.portfolio_notional("PORT1"), 23000.0);
}

TEST_F(NotionalMetricsTest, RemoveOrders) {
    metrics.add_order("AAPL_OPT1", "STRAT1", "PORT1", 100);
    metrics.remove_order("AAPL_OPT1", "STRAT1", "PORT1", 100);

    EXPECT_DOUBLE_EQ(metrics.global_notional(), 0.0);
    EXPECT_DOUBLE_EQ(metrics.strategy_notional("STRAT1"), 0.0);
}

TEST_F(NotionalMetricsTest, UpdateOrder) {
    metrics.add_order("AAPL_OPT1", "STRAT1", "PORT1", 100);  // 10000 notional
    metrics.update_order("AAPL_OPT1", "STRAT1", "PORT1", 100, 150);  // now 15000 notional

    EXPECT_DOUBLE_EQ(metrics.strategy_notional("STRAT1"), 15000.0);
}

TEST_F(NotionalMetricsTest, PartialFill) {
    metrics.add_order("AAPL_OPT1", "STRAT1", "PORT1", 100);  // 10000 notional
    metrics.partial_fill("AAPL_OPT1", "STRAT1", "PORT1", 40);  // remove 40 -> 60 * 100 = 6000

    EXPECT_DOUBLE_EQ(metrics.strategy_notional("STRAT1"), 6000.0);
}

TEST_F(NotionalMetricsTest, EmptyStrategy) {
    // Order with empty strategy should only update global and portfolio
    metrics.add_order("AAPL_OPT1", "", "PORT1", 100);  // 10000 notional

    EXPECT_DOUBLE_EQ(metrics.global_notional(), 10000.0);
    EXPECT_DOUBLE_EQ(metrics.strategy_notional(""), 0.0);  // Empty string strategy not tracked
    EXPECT_DOUBLE_EQ(metrics.portfolio_notional("PORT1"), 10000.0);
}

TEST_F(NotionalMetricsTest, QuantityAccessors) {
    metrics.add_order("AAPL_OPT1", "STRAT1", "PORT1", 100);
    metrics.add_order("AAPL_OPT1", "STRAT1", "PORT1", 50);

    EXPECT_EQ(metrics.global_quantity(), 150);
    EXPECT_EQ(metrics.instrument_quantity("AAPL_OPT1"), 150);
}

// ============================================================================
// Generic Engine Template Tests
// ============================================================================

#include "../src/engine/risk_engine.hpp"

// Test fixture with InstrumentProvider for engine tests
class GenericEngineTestFixture : public ::testing::Test {
protected:
    instrument::StaticInstrumentProvider provider;

    void SetUp() override {
        // AAPL: spot=100, contract_size=1, fx=1, delta=0.5
        // notional = qty * contract_size(1) * spot(100) * fx(1) = qty * 100
        // delta_exp = qty * delta(0.5) * contract_size(1) * underlyer_spot(100) * fx(1) = qty * 50
        provider.add_option("AAPL", "AAPL", 100.0, 100.0, 0.5, 1.0, 1.0);
    }
};

TEST(GenericEngineTest, EmptyEngine) {
    // Engine with no metrics
    engine::GenericRiskAggregationEngine<> engine;

    EXPECT_EQ(engine.metric_count(), 0u);
    EXPECT_EQ(engine.active_order_count(), 0u);
}

TEST_F(GenericEngineTestFixture, DeltaOnlyEngine) {
    engine::DeltaOnlyEngine engine(&provider);

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
    order.quantity = 10;
    order.strategy_id = "STRAT1";
    order.portfolio_id = "PORT1";

    engine.on_new_order_single(order);

    // delta_exp = qty(10) * delta(0.5) * contract_size(1) * underlyer_spot(100) * fx(1) = 500
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 500.0);
    EXPECT_DOUBLE_EQ(engine.global_net_delta(), 500.0);    // BID = positive
    EXPECT_DOUBLE_EQ(engine.underlyer_gross_delta("AAPL"), 500.0);
}

TEST_F(GenericEngineTestFixture, OrderCountOnlyEngine) {
    engine::OrderCountOnlyEngine engine(&provider);

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
    order.quantity = 10;
    order.strategy_id = "STRAT1";
    order.portfolio_id = "PORT1";

    engine.on_new_order_single(order);

    EXPECT_EQ(engine.bid_order_count("AAPL"), 1);
    EXPECT_EQ(engine.ask_order_count("AAPL"), 0);
    EXPECT_EQ(engine.quoted_instruments_count("AAPL"), 1);
}

TEST_F(GenericEngineTestFixture, NotionalOnlyEngine) {
    engine::NotionalOnlyEngine engine(&provider);

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
    order.quantity = 10;
    order.strategy_id = "STRAT1";
    order.portfolio_id = "PORT1";

    engine.on_new_order_single(order);

    // notional = qty(10) * contract_size(1) * spot(100) * fx(1) = 1000
    EXPECT_DOUBLE_EQ(engine.global_notional(), 1000.0);
    EXPECT_DOUBLE_EQ(engine.strategy_notional("STRAT1"), 1000.0);
    EXPECT_DOUBLE_EQ(engine.portfolio_notional("PORT1"), 1000.0);
}

TEST_F(GenericEngineTestFixture, CustomMetricCombination) {
    // Engine with only Delta and Notional metrics
    using DeltaNotionalEngine = engine::GenericRiskAggregationEngine<
        metrics::DeltaMetrics,
        metrics::NotionalMetrics
    >;

    DeltaNotionalEngine engine(&provider);

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
    order.quantity = 20;
    order.strategy_id = "STRAT1";
    order.portfolio_id = "PORT1";

    engine.on_new_order_single(order);

    // delta_exp = qty(20) * delta(0.5) * contract_size(1) * underlyer_spot(100) * fx(1) = 1000
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 1000.0);
    EXPECT_DOUBLE_EQ(engine.global_net_delta(), -1000.0);    // ASK = negative
    // notional = qty(20) * contract_size(1) * spot(100) * fx(1) = 2000
    EXPECT_DOUBLE_EQ(engine.global_notional(), 2000.0);
}

TEST_F(GenericEngineTestFixture, StandardEngineHasAllAccessors) {
    engine::RiskAggregationEngine engine(&provider);

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
    order.quantity = 10;
    order.strategy_id = "STRAT1";
    order.portfolio_id = "PORT1";

    engine.on_new_order_single(order);

    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 500.0);  // 10 * 0.5 * 100
    EXPECT_EQ(engine.bid_order_count("AAPL"), 1);
    EXPECT_DOUBLE_EQ(engine.global_notional(), 1000.0);    // 10 * 100
}

TEST_F(GenericEngineTestFixture, GetMetricAccess) {
    engine::RiskAggregationEngine engine(&provider);

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
    order.quantity = 10;
    order.strategy_id = "STRAT1";
    order.portfolio_id = "PORT1";

    engine.on_new_order_single(order);

    // Access metrics directly
    EXPECT_DOUBLE_EQ(delta.global_gross_delta(), 500.0);
    EXPECT_EQ(order_count.bid_order_count("AAPL"), 1);
    EXPECT_DOUBLE_EQ(notional.global_notional(), 1000.0);
}

// ============================================================================
// MultiGroupAggregator Tests
// ============================================================================

#include "../src/aggregation/multi_group_aggregator.hpp"

// Helper to create a TrackedOrder for testing
engine::TrackedOrder make_test_order(
    const std::string& symbol,
    const std::string& underlyer,
    const std::string& strategy_id,
    const std::string& portfolio_id,
    fix::Side side,
    double price,
    int64_t quantity
) {
    engine::TrackedOrder order;
    order.key.cl_ord_id = "TEST001";
    order.symbol = symbol;
    order.underlyer = underlyer;
    order.strategy_id = strategy_id;
    order.portfolio_id = portfolio_id;
    order.side = side;
    order.price = price;
    order.quantity = quantity;
    order.leaves_qty = quantity;
    order.cum_qty = 0;
    order.state = engine::OrderState::OPEN;
    return order;
}

TEST(KeyExtractorTest, GlobalKeyAlwaysApplicable) {
    auto order = make_test_order("AAPL230120C150", "AAPL", "STRAT1", "PORT1",
                                  fix::Side::BID, 100.0, 10);

    EXPECT_TRUE(KeyExtractor<GlobalKey>::is_applicable(order));
    EXPECT_EQ(KeyExtractor<GlobalKey>::extract(order), GlobalKey::instance());
}

TEST(KeyExtractorTest, UnderlyerKeyExtraction) {
    auto order = make_test_order("AAPL230120C150", "AAPL", "STRAT1", "PORT1",
                                  fix::Side::BID, 100.0, 10);

    EXPECT_TRUE(KeyExtractor<UnderlyerKey>::is_applicable(order));
    EXPECT_EQ(KeyExtractor<UnderlyerKey>::extract(order), UnderlyerKey{"AAPL"});
}

TEST(KeyExtractorTest, StrategyKeyApplicability) {
    auto order_with_strategy = make_test_order("AAPL", "AAPL", "STRAT1", "PORT1",
                                                fix::Side::BID, 100.0, 10);
    auto order_without_strategy = make_test_order("AAPL", "AAPL", "", "PORT1",
                                                   fix::Side::BID, 100.0, 10);

    EXPECT_TRUE(KeyExtractor<StrategyKey>::is_applicable(order_with_strategy));
    EXPECT_FALSE(KeyExtractor<StrategyKey>::is_applicable(order_without_strategy));

    EXPECT_EQ(KeyExtractor<StrategyKey>::extract(order_with_strategy), StrategyKey{"STRAT1"});
}

TEST(KeyExtractorTest, PortfolioKeyApplicability) {
    auto order_with_portfolio = make_test_order("AAPL", "AAPL", "STRAT1", "PORT1",
                                                 fix::Side::BID, 100.0, 10);
    auto order_without_portfolio = make_test_order("AAPL", "AAPL", "STRAT1", "",
                                                    fix::Side::BID, 100.0, 10);

    EXPECT_TRUE(KeyExtractor<PortfolioKey>::is_applicable(order_with_portfolio));
    EXPECT_FALSE(KeyExtractor<PortfolioKey>::is_applicable(order_without_portfolio));

    EXPECT_EQ(KeyExtractor<PortfolioKey>::extract(order_with_portfolio), PortfolioKey{"PORT1"});
}

TEST(KeyExtractorTest, InstrumentSideKeyExtraction) {
    auto bid_order = make_test_order("AAPL230120C150", "AAPL", "STRAT1", "PORT1",
                                      fix::Side::BID, 100.0, 10);
    auto ask_order = make_test_order("AAPL230120C150", "AAPL", "STRAT1", "PORT1",
                                      fix::Side::ASK, 100.0, 10);

    EXPECT_TRUE(KeyExtractor<InstrumentSideKey>::is_applicable(bid_order));

    auto bid_key = KeyExtractor<InstrumentSideKey>::extract(bid_order);
    auto ask_key = KeyExtractor<InstrumentSideKey>::extract(ask_order);

    EXPECT_EQ(bid_key.symbol, "AAPL230120C150");
    EXPECT_EQ(bid_key.side, static_cast<int>(fix::Side::BID));
    EXPECT_EQ(ask_key.side, static_cast<int>(fix::Side::ASK));
}

class MultiGroupAggregatorTest : public ::testing::Test {
protected:
    using DeltaAggregator = MultiGroupAggregator<DeltaCombiner, GlobalKey, UnderlyerKey>;
    using NotionalAggregator = MultiGroupAggregator<SumCombiner<double>, GlobalKey, StrategyKey, PortfolioKey>;

    DeltaAggregator delta_agg;
    NotionalAggregator notional_agg;
};

TEST_F(MultiGroupAggregatorTest, AddToAllApplicableBuckets) {
    auto order = make_test_order("AAPL230120C150", "AAPL", "STRAT1", "PORT1",
                                  fix::Side::BID, 100.0, 10);

    DeltaValue dv{50.0, 25.0};
    delta_agg.add(order, dv);

    // Check global
    auto global_val = delta_agg.get(GlobalKey::instance());
    EXPECT_DOUBLE_EQ(global_val.gross, 50.0);
    EXPECT_DOUBLE_EQ(global_val.net, 25.0);

    // Check underlyer
    auto underlyer_val = delta_agg.get(UnderlyerKey{"AAPL"});
    EXPECT_DOUBLE_EQ(underlyer_val.gross, 50.0);
    EXPECT_DOUBLE_EQ(underlyer_val.net, 25.0);
}

TEST_F(MultiGroupAggregatorTest, RemoveFromAllApplicableBuckets) {
    auto order = make_test_order("AAPL230120C150", "AAPL", "STRAT1", "PORT1",
                                  fix::Side::BID, 100.0, 10);

    DeltaValue dv{50.0, 25.0};
    delta_agg.add(order, dv);
    delta_agg.remove(order, dv);

    // All values should be back to identity
    auto global_val = delta_agg.get(GlobalKey::instance());
    EXPECT_DOUBLE_EQ(global_val.gross, 0.0);
    EXPECT_DOUBLE_EQ(global_val.net, 0.0);

    auto underlyer_val = delta_agg.get(UnderlyerKey{"AAPL"});
    EXPECT_DOUBLE_EQ(underlyer_val.gross, 0.0);
    EXPECT_DOUBLE_EQ(underlyer_val.net, 0.0);
}

TEST_F(MultiGroupAggregatorTest, UpdateInAllApplicableBuckets) {
    auto order = make_test_order("AAPL230120C150", "AAPL", "STRAT1", "PORT1",
                                  fix::Side::BID, 100.0, 10);

    DeltaValue old_dv{50.0, 25.0};
    DeltaValue new_dv{75.0, 40.0};

    delta_agg.add(order, old_dv);
    delta_agg.update(order, old_dv, new_dv);

    auto global_val = delta_agg.get(GlobalKey::instance());
    EXPECT_DOUBLE_EQ(global_val.gross, 75.0);
    EXPECT_DOUBLE_EQ(global_val.net, 40.0);
}

TEST_F(MultiGroupAggregatorTest, SkipsNonApplicableBuckets) {
    // Order with empty strategy_id
    auto order = make_test_order("AAPL230120C150", "AAPL", "", "PORT1",
                                  fix::Side::BID, 100.0, 10);

    notional_agg.add(order, 1000.0);

    // Global and portfolio should be updated
    EXPECT_DOUBLE_EQ(notional_agg.get(GlobalKey::instance()), 1000.0);
    EXPECT_DOUBLE_EQ(notional_agg.get(PortfolioKey{"PORT1"}), 1000.0);

    // Strategy should NOT be updated (empty strategy_id)
    EXPECT_DOUBLE_EQ(notional_agg.get(StrategyKey{""}), 0.0);
}

TEST_F(MultiGroupAggregatorTest, MultipleUnderlyers) {
    auto aapl_order = make_test_order("AAPL230120C150", "AAPL", "STRAT1", "PORT1",
                                       fix::Side::BID, 100.0, 10);
    auto msft_order = make_test_order("MSFT230120C250", "MSFT", "STRAT1", "PORT1",
                                       fix::Side::ASK, 100.0, 10);

    delta_agg.add(aapl_order, DeltaValue{100.0, 50.0});
    delta_agg.add(msft_order, DeltaValue{75.0, -30.0});

    // Global should have sum
    auto global_val = delta_agg.get(GlobalKey::instance());
    EXPECT_DOUBLE_EQ(global_val.gross, 175.0);
    EXPECT_DOUBLE_EQ(global_val.net, 20.0);

    // Underlyers should be separate
    auto aapl_val = delta_agg.get(UnderlyerKey{"AAPL"});
    EXPECT_DOUBLE_EQ(aapl_val.gross, 100.0);
    EXPECT_DOUBLE_EQ(aapl_val.net, 50.0);

    auto msft_val = delta_agg.get(UnderlyerKey{"MSFT"});
    EXPECT_DOUBLE_EQ(msft_val.gross, 75.0);
    EXPECT_DOUBLE_EQ(msft_val.net, -30.0);
}

TEST_F(MultiGroupAggregatorTest, ClearRemovesAll) {
    auto order = make_test_order("AAPL230120C150", "AAPL", "STRAT1", "PORT1",
                                  fix::Side::BID, 100.0, 10);

    delta_agg.add(order, DeltaValue{50.0, 25.0});
    delta_agg.clear();

    auto global_val = delta_agg.get(GlobalKey::instance());
    EXPECT_DOUBLE_EQ(global_val.gross, 0.0);
    EXPECT_DOUBLE_EQ(global_val.net, 0.0);
}

TEST_F(MultiGroupAggregatorTest, HasKeyCompileTimeCheck) {
    EXPECT_TRUE((DeltaAggregator::has_key<GlobalKey>()));
    EXPECT_TRUE((DeltaAggregator::has_key<UnderlyerKey>()));
    EXPECT_FALSE((DeltaAggregator::has_key<StrategyKey>()));

    EXPECT_TRUE((NotionalAggregator::has_key<GlobalKey>()));
    EXPECT_TRUE((NotionalAggregator::has_key<StrategyKey>()));
    EXPECT_TRUE((NotionalAggregator::has_key<PortfolioKey>()));
    EXPECT_FALSE((NotionalAggregator::has_key<UnderlyerKey>()));
}

TEST_F(MultiGroupAggregatorTest, KeyCount) {
    EXPECT_EQ(DeltaAggregator::key_count(), 2u);
    EXPECT_EQ(NotionalAggregator::key_count(), 3u);
}

TEST_F(MultiGroupAggregatorTest, DirectBucketAccess) {
    auto order = make_test_order("AAPL230120C150", "AAPL", "STRAT1", "PORT1",
                                  fix::Side::BID, 100.0, 10);

    // Add directly to bucket
    delta_agg.bucket<GlobalKey>().add(GlobalKey::instance(), DeltaValue{100.0, 50.0});

    auto global_val = delta_agg.get(GlobalKey::instance());
    EXPECT_DOUBLE_EQ(global_val.gross, 100.0);

    // Underlyer bucket should still be empty
    auto underlyer_val = delta_agg.get(UnderlyerKey{"AAPL"});
    EXPECT_DOUBLE_EQ(underlyer_val.gross, 0.0);
}

TEST_F(MultiGroupAggregatorTest, KeysRetrieval) {
    auto aapl_order = make_test_order("AAPL230120C150", "AAPL", "STRAT1", "PORT1",
                                       fix::Side::BID, 100.0, 10);
    auto msft_order = make_test_order("MSFT230120C250", "MSFT", "STRAT2", "PORT1",
                                       fix::Side::ASK, 100.0, 10);

    delta_agg.add(aapl_order, DeltaValue{100.0, 50.0});
    delta_agg.add(msft_order, DeltaValue{75.0, -30.0});

    auto underlyer_keys = delta_agg.keys<UnderlyerKey>();
    EXPECT_EQ(underlyer_keys.size(), 2u);
}
