#include "test_framework.hpp"
#include "../src/aggregation/grouping.hpp"
#include "../src/aggregation/aggregation_traits.hpp"
#include "../src/aggregation/aggregation_core.hpp"
#include "../src/metrics/delta_metrics.hpp"
#include "../src/metrics/order_count_metrics.hpp"
#include "../src/metrics/notional_metrics.hpp"

using namespace test;
using namespace aggregation;

// ============================================================================
// Grouping Key Tests
// ============================================================================

void test_global_key_equality() {
    GlobalKey k1, k2;
    assert_true(k1 == k2, "All GlobalKeys should be equal");
}

void test_underlyer_key_equality() {
    UnderlyerKey k1{"AAPL"};
    UnderlyerKey k2{"AAPL"};
    UnderlyerKey k3{"MSFT"};

    assert_true(k1 == k2);
    assert_false(k1 == k3);
}

void test_instrument_key_equality() {
    InstrumentKey k1{"AAPL230120C150"};
    InstrumentKey k2{"AAPL230120C150"};
    InstrumentKey k3{"AAPL230120P150"};

    assert_true(k1 == k2);
    assert_false(k1 == k3);
}

void test_instrument_side_key_equality() {
    InstrumentSideKey k1{"AAPL", 1};
    InstrumentSideKey k2{"AAPL", 1};
    InstrumentSideKey k3{"AAPL", 2};
    InstrumentSideKey k4{"MSFT", 1};

    assert_true(k1 == k2);
    assert_false(k1 == k3);
    assert_false(k1 == k4);
}

void test_key_hashing() {
    std::unordered_map<UnderlyerKey, int> map;
    map[UnderlyerKey{"AAPL"}] = 1;
    map[UnderlyerKey{"MSFT"}] = 2;
    map[UnderlyerKey{"GOOG"}] = 3;

    assert_equal(map[UnderlyerKey{"AAPL"}], 1);
    assert_equal(map[UnderlyerKey{"MSFT"}], 2);
    assert_equal(map[UnderlyerKey{"GOOG"}], 3);
    assert_equal(map.size(), size_t(3));
}

// ============================================================================
// Combiner Tests
// ============================================================================

void test_sum_combiner_double() {
    using Combiner = SumCombiner<double>;
    assert_double_equal(Combiner::identity(), 0.0);
    assert_double_equal(Combiner::combine(10.0, 5.0), 15.0);
    assert_double_equal(Combiner::uncombine(15.0, 5.0), 10.0);
}

void test_count_combiner() {
    assert_equal(CountCombiner::identity(), int64_t(0));
    assert_equal(CountCombiner::combine(10, 3), int64_t(13));
    assert_equal(CountCombiner::uncombine(13, 3), int64_t(10));
}

void test_delta_combiner() {
    DeltaValue v1{100.0, 50.0};
    DeltaValue v2{25.0, -10.0};

    auto combined = DeltaCombiner::combine(v1, v2);
    assert_double_equal(combined.gross, 125.0);
    assert_double_equal(combined.net, 40.0);

    auto uncombined = DeltaCombiner::uncombine(combined, v2);
    assert_double_equal(uncombined.gross, 100.0);
    assert_double_equal(uncombined.net, 50.0);
}

// ============================================================================
// AggregationBucket Tests
// ============================================================================

void test_bucket_add_and_get() {
    AggregationBucket<UnderlyerKey, SumCombiner<double>> bucket;

    bucket.add(UnderlyerKey{"AAPL"}, 100.0);
    bucket.add(UnderlyerKey{"MSFT"}, 200.0);
    bucket.add(UnderlyerKey{"AAPL"}, 50.0);

    assert_double_equal(bucket.get(UnderlyerKey{"AAPL"}), 150.0);
    assert_double_equal(bucket.get(UnderlyerKey{"MSFT"}), 200.0);
    assert_double_equal(bucket.get(UnderlyerKey{"GOOG"}), 0.0);  // Not present, returns identity
}

void test_bucket_remove() {
    AggregationBucket<UnderlyerKey, SumCombiner<double>> bucket;

    bucket.add(UnderlyerKey{"AAPL"}, 100.0);
    bucket.remove(UnderlyerKey{"AAPL"}, 40.0);

    assert_double_equal(bucket.get(UnderlyerKey{"AAPL"}), 60.0);
}

void test_bucket_update() {
    AggregationBucket<UnderlyerKey, SumCombiner<double>> bucket;

    bucket.add(UnderlyerKey{"AAPL"}, 100.0);
    bucket.update(UnderlyerKey{"AAPL"}, 100.0, 150.0);

    assert_double_equal(bucket.get(UnderlyerKey{"AAPL"}), 150.0);
}

void test_bucket_contains() {
    AggregationBucket<UnderlyerKey, CountCombiner> bucket;

    bucket.add(UnderlyerKey{"AAPL"}, 1);

    assert_true(bucket.contains(UnderlyerKey{"AAPL"}));
    assert_false(bucket.contains(UnderlyerKey{"MSFT"}));
}

void test_bucket_size_and_keys() {
    AggregationBucket<UnderlyerKey, CountCombiner> bucket;

    bucket.add(UnderlyerKey{"AAPL"}, 1);
    bucket.add(UnderlyerKey{"MSFT"}, 2);
    bucket.add(UnderlyerKey{"GOOG"}, 3);

    assert_equal(bucket.size(), size_t(3));

    auto keys = bucket.keys();
    assert_equal(keys.size(), size_t(3));
}

void test_bucket_clear() {
    AggregationBucket<UnderlyerKey, CountCombiner> bucket;

    bucket.add(UnderlyerKey{"AAPL"}, 1);
    bucket.add(UnderlyerKey{"MSFT"}, 2);
    bucket.clear();

    assert_equal(bucket.size(), size_t(0));
    assert_equal(bucket.get(UnderlyerKey{"AAPL"}), int64_t(0));
}

void test_bucket_cleanup_on_zero() {
    AggregationBucket<UnderlyerKey, CountCombiner> bucket;

    bucket.add(UnderlyerKey{"AAPL"}, 5);
    assert_equal(bucket.size(), size_t(1));

    bucket.remove(UnderlyerKey{"AAPL"}, 5);
    assert_equal(bucket.size(), size_t(0));  // Key should be removed when value returns to identity
}

void test_bucket_delta_values() {
    AggregationBucket<GlobalKey, DeltaCombiner> bucket;

    bucket.add(GlobalKey::instance(), DeltaValue{100.0, 50.0});
    bucket.add(GlobalKey::instance(), DeltaValue{50.0, -30.0});

    auto value = bucket.get(GlobalKey::instance());
    assert_double_equal(value.gross, 150.0);
    assert_double_equal(value.net, 20.0);
}

// ============================================================================
// AggregationEngine Tests
// ============================================================================

void test_aggregation_engine_multiple_buckets() {
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

    assert_double_equal(global.get(GlobalKey::instance()).gross, 100.0);
    assert_double_equal(underlyer.get(UnderlyerKey{"AAPL"}).net, 50.0);
    assert_double_equal(strategy.get(StrategyKey{"STRAT1"}), 10000.0);
}

void test_aggregation_engine_clear() {
    using Engine = AggregationEngine<GlobalDeltaBucket, StrategyNotionalBucket>;

    Engine engine;
    engine.get<GlobalDeltaBucket>().add(GlobalKey::instance(), DeltaValue{100.0, 50.0});
    engine.get<StrategyNotionalBucket>().add(StrategyKey{"STRAT1"}, 10000.0);

    engine.clear();

    assert_double_equal(engine.get<GlobalDeltaBucket>().get(GlobalKey::instance()).gross, 0.0);
    assert_double_equal(engine.get<StrategyNotionalBucket>().get(StrategyKey{"STRAT1"}), 0.0);
}

// ============================================================================
// Delta Metrics Tests
// ============================================================================

void test_delta_metrics_add_bid_order() {
    metrics::DeltaMetrics metrics;

    metrics.add_order("AAPL", 100.0, fix::Side::BID);

    assert_double_equal(metrics.global_gross_delta(), 100.0);
    assert_double_equal(metrics.global_net_delta(), 100.0);  // Positive for bid
    assert_double_equal(metrics.underlyer_gross_delta("AAPL"), 100.0);
    assert_double_equal(metrics.underlyer_net_delta("AAPL"), 100.0);
}

void test_delta_metrics_add_ask_order() {
    metrics::DeltaMetrics metrics;

    metrics.add_order("AAPL", 100.0, fix::Side::ASK);

    assert_double_equal(metrics.global_gross_delta(), 100.0);
    assert_double_equal(metrics.global_net_delta(), -100.0);  // Negative for ask
}

void test_delta_metrics_multiple_orders() {
    metrics::DeltaMetrics metrics;

    metrics.add_order("AAPL", 100.0, fix::Side::BID);
    metrics.add_order("AAPL", 50.0, fix::Side::ASK);
    metrics.add_order("MSFT", 75.0, fix::Side::BID);

    assert_double_equal(metrics.global_gross_delta(), 225.0);  // 100 + 50 + 75
    assert_double_equal(metrics.global_net_delta(), 125.0);    // 100 - 50 + 75
    assert_double_equal(metrics.underlyer_gross_delta("AAPL"), 150.0);
    assert_double_equal(metrics.underlyer_net_delta("AAPL"), 50.0);  // 100 - 50
    assert_double_equal(metrics.underlyer_gross_delta("MSFT"), 75.0);
}

void test_delta_metrics_remove_order() {
    metrics::DeltaMetrics metrics;

    metrics.add_order("AAPL", 100.0, fix::Side::BID);
    metrics.remove_order("AAPL", 100.0, fix::Side::BID);

    assert_double_equal(metrics.global_gross_delta(), 0.0);
    assert_double_equal(metrics.global_net_delta(), 0.0);
}

void test_delta_metrics_update_order() {
    metrics::DeltaMetrics metrics;

    metrics.add_order("AAPL", 100.0, fix::Side::BID);
    metrics.update_order("AAPL", 100.0, 150.0, fix::Side::BID);

    assert_double_equal(metrics.global_gross_delta(), 150.0);
    assert_double_equal(metrics.global_net_delta(), 150.0);
}

void test_delta_metrics_partial_fill() {
    metrics::DeltaMetrics metrics;

    metrics.add_order("AAPL", 100.0, fix::Side::BID);
    metrics.partial_fill("AAPL", 40.0, fix::Side::BID);

    assert_double_equal(metrics.global_gross_delta(), 60.0);
    assert_double_equal(metrics.global_net_delta(), 60.0);
}

// ============================================================================
// Order Count Metrics Tests
// ============================================================================

void test_order_count_add_orders() {
    metrics::OrderCountMetrics metrics;

    metrics.add_order("AAPL230120C150", "AAPL", fix::Side::BID);
    metrics.add_order("AAPL230120C150", "AAPL", fix::Side::BID);
    metrics.add_order("AAPL230120C150", "AAPL", fix::Side::ASK);

    assert_equal(metrics.bid_order_count("AAPL230120C150"), int64_t(2));
    assert_equal(metrics.ask_order_count("AAPL230120C150"), int64_t(1));
    assert_equal(metrics.total_order_count("AAPL230120C150"), int64_t(3));
}

void test_order_count_remove_orders() {
    metrics::OrderCountMetrics metrics;

    metrics.add_order("AAPL230120C150", "AAPL", fix::Side::BID);
    metrics.add_order("AAPL230120C150", "AAPL", fix::Side::BID);
    metrics.remove_order("AAPL230120C150", "AAPL", fix::Side::BID);

    assert_equal(metrics.bid_order_count("AAPL230120C150"), int64_t(1));
}

void test_quoted_instruments_count() {
    metrics::OrderCountMetrics metrics;

    // Add orders for multiple instruments under AAPL
    metrics.add_order("AAPL230120C150", "AAPL", fix::Side::BID);
    metrics.add_order("AAPL230120P150", "AAPL", fix::Side::ASK);
    metrics.add_order("AAPL230217C160", "AAPL", fix::Side::BID);

    // Add orders for MSFT
    metrics.add_order("MSFT230120C250", "MSFT", fix::Side::BID);

    assert_equal(metrics.quoted_instruments_count("AAPL"), int64_t(3));
    assert_equal(metrics.quoted_instruments_count("MSFT"), int64_t(1));
}

void test_quoted_instruments_decrement() {
    metrics::OrderCountMetrics metrics;

    // Add two orders for one instrument
    metrics.add_order("AAPL230120C150", "AAPL", fix::Side::BID);
    metrics.add_order("AAPL230120C150", "AAPL", fix::Side::ASK);

    assert_equal(metrics.quoted_instruments_count("AAPL"), int64_t(1));

    // Remove one - instrument still quoted
    metrics.remove_order("AAPL230120C150", "AAPL", fix::Side::BID);
    assert_equal(metrics.quoted_instruments_count("AAPL"), int64_t(1));

    // Remove last - instrument no longer quoted
    metrics.remove_order("AAPL230120C150", "AAPL", fix::Side::ASK);
    assert_equal(metrics.quoted_instruments_count("AAPL"), int64_t(0));
}

// ============================================================================
// Notional Metrics Tests
// ============================================================================

void test_notional_add_orders() {
    metrics::NotionalMetrics metrics;

    metrics.add_order("STRAT1", "PORT1", 10000.0);
    metrics.add_order("STRAT1", "PORT1", 5000.0);
    metrics.add_order("STRAT2", "PORT1", 8000.0);

    assert_double_equal(metrics.global_notional(), 23000.0);
    assert_double_equal(metrics.strategy_notional("STRAT1"), 15000.0);
    assert_double_equal(metrics.strategy_notional("STRAT2"), 8000.0);
    assert_double_equal(metrics.portfolio_notional("PORT1"), 23000.0);
}

void test_notional_remove_orders() {
    metrics::NotionalMetrics metrics;

    metrics.add_order("STRAT1", "PORT1", 10000.0);
    metrics.remove_order("STRAT1", "PORT1", 10000.0);

    assert_double_equal(metrics.global_notional(), 0.0);
    assert_double_equal(metrics.strategy_notional("STRAT1"), 0.0);
}

void test_notional_update_order() {
    metrics::NotionalMetrics metrics;

    metrics.add_order("STRAT1", "PORT1", 10000.0);
    metrics.update_order("STRAT1", "PORT1", 10000.0, 15000.0);

    assert_double_equal(metrics.strategy_notional("STRAT1"), 15000.0);
}

void test_notional_partial_fill() {
    metrics::NotionalMetrics metrics;

    metrics.add_order("STRAT1", "PORT1", 10000.0);
    metrics.partial_fill("STRAT1", "PORT1", 4000.0);

    assert_double_equal(metrics.strategy_notional("STRAT1"), 6000.0);
}

void test_notional_empty_strategy() {
    metrics::NotionalMetrics metrics;

    // Order with empty strategy should only update global and portfolio
    metrics.add_order("", "PORT1", 10000.0);

    assert_double_equal(metrics.global_notional(), 10000.0);
    assert_double_equal(metrics.strategy_notional(""), 0.0);  // Empty string strategy not tracked
    assert_double_equal(metrics.portfolio_notional("PORT1"), 10000.0);
}

// ============================================================================
// Run all aggregation tests
// ============================================================================

TestSuite run_aggregation_tests() {
    TestSuite suite("Aggregation Tests");

    // Grouping key tests
    suite.run_test("GlobalKey equality", test_global_key_equality);
    suite.run_test("UnderlyerKey equality", test_underlyer_key_equality);
    suite.run_test("InstrumentKey equality", test_instrument_key_equality);
    suite.run_test("InstrumentSideKey equality", test_instrument_side_key_equality);
    suite.run_test("Key hashing", test_key_hashing);

    // Combiner tests
    suite.run_test("SumCombiner<double>", test_sum_combiner_double);
    suite.run_test("CountCombiner", test_count_combiner);
    suite.run_test("DeltaCombiner", test_delta_combiner);

    // AggregationBucket tests
    suite.run_test("Bucket add and get", test_bucket_add_and_get);
    suite.run_test("Bucket remove", test_bucket_remove);
    suite.run_test("Bucket update", test_bucket_update);
    suite.run_test("Bucket contains", test_bucket_contains);
    suite.run_test("Bucket size and keys", test_bucket_size_and_keys);
    suite.run_test("Bucket clear", test_bucket_clear);
    suite.run_test("Bucket cleanup on zero", test_bucket_cleanup_on_zero);
    suite.run_test("Bucket delta values", test_bucket_delta_values);

    // AggregationEngine tests
    suite.run_test("Engine multiple buckets", test_aggregation_engine_multiple_buckets);
    suite.run_test("Engine clear", test_aggregation_engine_clear);

    // Delta metrics tests
    suite.run_test("Delta metrics - add bid order", test_delta_metrics_add_bid_order);
    suite.run_test("Delta metrics - add ask order", test_delta_metrics_add_ask_order);
    suite.run_test("Delta metrics - multiple orders", test_delta_metrics_multiple_orders);
    suite.run_test("Delta metrics - remove order", test_delta_metrics_remove_order);
    suite.run_test("Delta metrics - update order", test_delta_metrics_update_order);
    suite.run_test("Delta metrics - partial fill", test_delta_metrics_partial_fill);

    // Order count metrics tests
    suite.run_test("Order count - add orders", test_order_count_add_orders);
    suite.run_test("Order count - remove orders", test_order_count_remove_orders);
    suite.run_test("Quoted instruments count", test_quoted_instruments_count);
    suite.run_test("Quoted instruments decrement", test_quoted_instruments_decrement);

    // Notional metrics tests
    suite.run_test("Notional - add orders", test_notional_add_orders);
    suite.run_test("Notional - remove orders", test_notional_remove_orders);
    suite.run_test("Notional - update order", test_notional_update_order);
    suite.run_test("Notional - partial fill", test_notional_partial_fill);
    suite.run_test("Notional - empty strategy", test_notional_empty_strategy);

    return suite;
}
