#include <gtest/gtest.h>
#include "../src/engine/risk_engine_with_limits.hpp"
#include "../src/metrics/delta_metric.hpp"
#include "../src/metrics/vega_metric.hpp"
#include "../src/instrument/instrument.hpp"
#include "../src/fix/fix_parser.hpp"

using namespace engine;
using namespace fix;
using namespace aggregation;
using namespace metrics;
using namespace instrument;

// ============================================================================
// Integration Test: Combined Vega and Delta Metrics at Underlyer Level
// ============================================================================
//
// This test verifies tracking of both delta and vega exposure at the underlyer
// level using POSITION STAGE ONLY (tracks filled orders).
//
// Key behaviors tested:
//
// 1. Stocks have delta=1 and vega=0 (stocks contribute to delta but not vega)
// 2. Options have both delta and vega exposure
// 3. Gross delta/vega = sum of |exposure|
// 4. Net delta/vega = signed sum (BID = +, ASK = -)
// 5. Pre-trade checks work for both delta and vega limits
// 6. Metrics only update after fills (not on insert/ack)
//
// Exposure formulas:
//   Delta exposure = quantity * delta * contract_size * underlyer_spot * fx_rate
//   Vega exposure = quantity * vega * contract_size * underlyer_spot * fx_rate
//

namespace {

NewOrderSingle create_order(const std::string& cl_ord_id, const std::string& symbol,
                             const std::string& underlyer, Side side,
                             double price, int64_t qty,
                             const std::string& strategy = "STRAT1") {
    NewOrderSingle order;
    order.key.cl_ord_id = cl_ord_id;
    order.symbol = symbol;
    order.underlyer = underlyer;
    order.side = side;
    order.price = price;
    order.quantity = qty;
    order.strategy_id = strategy;
    order.portfolio_id = "PORT1";
    return order;
}

ExecutionReport create_ack(const std::string& cl_ord_id, int64_t leaves_qty) {
    ExecutionReport report;
    report.key.cl_ord_id = cl_ord_id;
    report.order_id = "EX" + cl_ord_id;
    report.ord_status = OrdStatus::NEW;
    report.exec_type = ExecType::NEW;
    report.leaves_qty = leaves_qty;
    report.cum_qty = 0;
    report.is_unsolicited = false;
    return report;
}

ExecutionReport create_fill(const std::string& cl_ord_id, int64_t fill_qty, int64_t leaves_qty, double price) {
    ExecutionReport report;
    report.key.cl_ord_id = cl_ord_id;
    report.order_id = "EX" + cl_ord_id;
    report.ord_status = leaves_qty > 0 ? OrdStatus::PARTIALLY_FILLED : OrdStatus::FILLED;
    report.exec_type = leaves_qty > 0 ? ExecType::PARTIAL_FILL : ExecType::FILL;
    report.leaves_qty = leaves_qty;
    report.cum_qty = fill_qty;
    report.last_qty = fill_qty;
    report.last_px = price;
    report.is_unsolicited = false;
    return report;
}

// Create provider with stocks and options
StaticInstrumentProvider create_vega_delta_provider() {
    StaticInstrumentProvider provider;

    // AAPL stock: delta=1, vega=0, contract_size=1
    provider.add_equity("AAPL", 150.0);  // spot=$150

    // AAPL options with vega
    // AAPL_C150: Call, spot=$5.00, underlyer_spot=$150, delta=0.5, vega=0.25, contract_size=100
    provider.add_option("AAPL_C150", "AAPL", 5.0, 150.0, 0.5, 0.25, 100.0, 1.0);
    // AAPL_P150: Put, spot=$3.00, underlyer_spot=$150, delta=-0.4, vega=0.20, contract_size=100
    provider.add_option("AAPL_P150", "AAPL", 3.0, 150.0, -0.4, 0.20, 100.0, 1.0);

    // MSFT stock: delta=1, vega=0, contract_size=1
    provider.add_equity("MSFT", 300.0);  // spot=$300

    // MSFT options with vega
    // MSFT_C300: Call, spot=$8.00, underlyer_spot=$300, delta=0.6, vega=0.30, contract_size=100
    provider.add_option("MSFT_C300", "MSFT", 8.0, 300.0, 0.6, 0.30, 100.0, 1.0);

    return provider;
}

}  // namespace

// ============================================================================
// Test Fixture: Combined Vega and Delta Tracking
// ============================================================================

class VegaDeltaCombinedTest : public ::testing::Test {
protected:
    // Track only at position stage (filled orders)
    using UnderlyerGrossDelta = UnderlyerGrossDeltaMetric<StaticInstrumentProvider, PositionStage>;
    using UnderlyerNetDelta = UnderlyerNetDeltaMetric<StaticInstrumentProvider, PositionStage>;
    using UnderlyerGrossVega = UnderlyerGrossVegaMetric<StaticInstrumentProvider, PositionStage>;
    using UnderlyerNetVega = UnderlyerNetVegaMetric<StaticInstrumentProvider, PositionStage>;

    using TestEngine = RiskAggregationEngineWithLimits<
        StaticInstrumentProvider,
        UnderlyerGrossDelta,
        UnderlyerNetDelta,
        UnderlyerGrossVega,
        UnderlyerNetVega
    >;

    StaticInstrumentProvider provider;
    TestEngine engine;

    // Default limits
    static constexpr double MAX_GROSS_DELTA = 100000.0;
    static constexpr double MAX_NET_DELTA = 50000.0;
    static constexpr double MAX_GROSS_VEGA = 50000.0;
    static constexpr double MAX_NET_VEGA = 25000.0;

    void SetUp() override {
        provider = create_vega_delta_provider();
        engine.set_instrument_provider(&provider);

        // Set limits for AAPL underlyer
        engine.set_limit<UnderlyerGrossDelta>(UnderlyerKey{"AAPL"}, MAX_GROSS_DELTA);
        engine.set_limit<UnderlyerNetDelta>(UnderlyerKey{"AAPL"}, MAX_NET_DELTA);
        engine.set_limit<UnderlyerGrossVega>(UnderlyerKey{"AAPL"}, MAX_GROSS_VEGA);
        engine.set_limit<UnderlyerNetVega>(UnderlyerKey{"AAPL"}, MAX_NET_VEGA);

        // Set limits for MSFT underlyer
        engine.set_limit<UnderlyerGrossDelta>(UnderlyerKey{"MSFT"}, MAX_GROSS_DELTA);
        engine.set_limit<UnderlyerNetDelta>(UnderlyerKey{"MSFT"}, MAX_NET_DELTA);
        engine.set_limit<UnderlyerGrossVega>(UnderlyerKey{"MSFT"}, MAX_GROSS_VEGA);
        engine.set_limit<UnderlyerNetVega>(UnderlyerKey{"MSFT"}, MAX_NET_VEGA);
    }

    // Accessors
    double gross_delta(const std::string& underlyer) const {
        return engine.get_metric<UnderlyerGrossDelta>().get(UnderlyerKey{underlyer});
    }

    double net_delta(const std::string& underlyer) const {
        return engine.get_metric<UnderlyerNetDelta>().get(UnderlyerKey{underlyer});
    }

    double gross_vega(const std::string& underlyer) const {
        return engine.get_metric<UnderlyerGrossVega>().get(UnderlyerKey{underlyer});
    }

    double net_vega(const std::string& underlyer) const {
        return engine.get_metric<UnderlyerNetVega>().get(UnderlyerKey{underlyer});
    }

    // Helper: compute expected delta exposure
    // delta_exposure = quantity * delta * contract_size * underlyer_spot * fx_rate
    double expected_delta_exposure(const std::string& symbol, int64_t qty) const {
        return provider.compute_delta_exposure(symbol, qty);
    }

    // Helper: compute expected vega exposure
    // vega_exposure = quantity * vega * contract_size * underlyer_spot * fx_rate
    double expected_vega_exposure(const std::string& symbol, int64_t qty) const {
        return provider.compute_vega_exposure(symbol, qty);
    }
};

// ============================================================================
// Test: Stock has delta=1 and vega=0
// ============================================================================

TEST_F(VegaDeltaCombinedTest, StockHasZeroVega) {
    // AAPL stock: qty=100, delta=1, vega=0, contract_size=1, underlyer_spot=$150
    // delta_exposure = 100 * 1 * 1 * 150 * 1 = 15,000
    // vega_exposure = 100 * 0 * 1 * 150 * 1 = 0

    EXPECT_DOUBLE_EQ(gross_delta("AAPL"), 0.0) << "Initial: gross_delta=0";
    EXPECT_DOUBLE_EQ(gross_vega("AAPL"), 0.0) << "Initial: gross_vega=0";

    engine.on_new_order_single(create_order("ORD001", "AAPL", "AAPL", Side::BID, 150.0, 100));
    engine.on_execution_report(create_ack("ORD001", 100));
    engine.on_execution_report(create_fill("ORD001", 100, 0, 150.0));

    // Stock contributes to delta but not vega
    EXPECT_DOUBLE_EQ(gross_delta("AAPL"), 15000.0) << "Stock has delta exposure";
    EXPECT_DOUBLE_EQ(net_delta("AAPL"), 15000.0) << "BID = positive net delta";
    EXPECT_DOUBLE_EQ(gross_vega("AAPL"), 0.0) << "Stock has ZERO vega exposure";
    EXPECT_DOUBLE_EQ(net_vega("AAPL"), 0.0) << "Stock has ZERO net vega";
}

// ============================================================================
// Test: Option has both delta and vega exposure
// ============================================================================

TEST_F(VegaDeltaCombinedTest, OptionHasDeltaAndVega) {
    // AAPL_C150: qty=10, delta=0.5, vega=0.25, contract_size=100, underlyer_spot=$150
    // delta_exposure = 10 * 0.5 * 100 * 150 * 1 = 75,000
    // vega_exposure = 10 * 0.25 * 100 * 150 * 1 = 37,500

    engine.on_new_order_single(create_order("ORD001", "AAPL_C150", "AAPL", Side::BID, 5.0, 10));
    engine.on_execution_report(create_ack("ORD001", 10));
    engine.on_execution_report(create_fill("ORD001", 10, 0, 5.0));

    // Option contributes to both delta and vega
    double expected_delta = expected_delta_exposure("AAPL_C150", 10);  // 75,000
    double expected_vega = expected_vega_exposure("AAPL_C150", 10);    // 37,500

    EXPECT_DOUBLE_EQ(expected_delta, 75000.0) << "Expected delta = 10 * 0.5 * 100 * 150 = 75000";
    EXPECT_DOUBLE_EQ(expected_vega, 37500.0) << "Expected vega = 10 * 0.25 * 100 * 150 = 37500";

    EXPECT_DOUBLE_EQ(gross_delta("AAPL"), 75000.0) << "Option has delta exposure";
    EXPECT_DOUBLE_EQ(net_delta("AAPL"), 75000.0) << "BID = positive net delta";
    EXPECT_DOUBLE_EQ(gross_vega("AAPL"), 37500.0) << "Option has vega exposure";
    EXPECT_DOUBLE_EQ(net_vega("AAPL"), 37500.0) << "BID = positive net vega";
}

// ============================================================================
// Test: Combined stock and option - only option contributes vega
// ============================================================================

TEST_F(VegaDeltaCombinedTest, CombinedStockAndOption) {
    // AAPL stock: qty=100, delta_exposure=15,000, vega_exposure=0
    // AAPL_C150: qty=10, delta_exposure=75,000, vega_exposure=37,500
    //
    // Combined:
    //   gross_delta = 15,000 + 75,000 = 90,000
    //   gross_vega = 0 + 37,500 = 37,500 (only option contributes)

    // Stock order - fill it
    engine.on_new_order_single(create_order("ORD001", "AAPL", "AAPL", Side::BID, 150.0, 100));
    engine.on_execution_report(create_ack("ORD001", 100));
    engine.on_execution_report(create_fill("ORD001", 100, 0, 150.0));

    EXPECT_DOUBLE_EQ(gross_delta("AAPL"), 15000.0);
    EXPECT_DOUBLE_EQ(gross_vega("AAPL"), 0.0);

    // Option order - fill it
    engine.on_new_order_single(create_order("ORD002", "AAPL_C150", "AAPL", Side::BID, 5.0, 10));
    engine.on_execution_report(create_ack("ORD002", 10));
    engine.on_execution_report(create_fill("ORD002", 10, 0, 5.0));

    EXPECT_DOUBLE_EQ(gross_delta("AAPL"), 90000.0) << "Stock + option delta";
    EXPECT_DOUBLE_EQ(gross_vega("AAPL"), 37500.0) << "Only option contributes vega";
    EXPECT_DOUBLE_EQ(net_delta("AAPL"), 90000.0) << "Both BID = positive net";
    EXPECT_DOUBLE_EQ(net_vega("AAPL"), 37500.0) << "BID = positive net vega";
}

// ============================================================================
// Test: Net vega with mixed sides (BID and ASK)
// ============================================================================

TEST_F(VegaDeltaCombinedTest, NetVegaWithMixedSides) {
    // AAPL_C150 BID qty=10: vega_exposure = +37,500
    // AAPL_C150 ASK qty=6: vega_exposure = -22,500
    //
    // gross_vega = 37,500 + 22,500 = 60,000
    // net_vega = 37,500 - 22,500 = 15,000

    // BID order - fill it
    engine.on_new_order_single(create_order("ORD001", "AAPL_C150", "AAPL", Side::BID, 5.0, 10));
    engine.on_execution_report(create_ack("ORD001", 10));
    engine.on_execution_report(create_fill("ORD001", 10, 0, 5.0));

    EXPECT_DOUBLE_EQ(gross_vega("AAPL"), 37500.0);
    EXPECT_DOUBLE_EQ(net_vega("AAPL"), 37500.0);

    // ASK order - fill it
    engine.on_new_order_single(create_order("ORD002", "AAPL_C150", "AAPL", Side::ASK, 5.0, 6));
    engine.on_execution_report(create_ack("ORD002", 6));
    engine.on_execution_report(create_fill("ORD002", 6, 0, 5.0));

    double ask_vega = expected_vega_exposure("AAPL_C150", 6);  // 22,500
    EXPECT_DOUBLE_EQ(ask_vega, 22500.0);

    EXPECT_DOUBLE_EQ(gross_vega("AAPL"), 60000.0) << "Gross = |BID| + |ASK|";
    EXPECT_DOUBLE_EQ(net_vega("AAPL"), 15000.0) << "Net = BID - ASK = 37500 - 22500 = 15000";
}

// ============================================================================
// Test: Vega limit breach check
// ============================================================================

TEST_F(VegaDeltaCombinedTest, VegaLimitBreachCheck) {
    // Set a lower gross vega limit for testing
    engine.set_limit<UnderlyerGrossVega>(UnderlyerKey{"AAPL"}, 50000.0);

    // AAPL_C150 BID qty=10: vega_exposure = 37,500 (under limit) - fill it
    engine.on_new_order_single(create_order("ORD001", "AAPL_C150", "AAPL", Side::BID, 5.0, 10));
    engine.on_execution_report(create_ack("ORD001", 10));
    engine.on_execution_report(create_fill("ORD001", 10, 0, 5.0));

    EXPECT_DOUBLE_EQ(gross_vega("AAPL"), 37500.0);

    // Pre-trade check: AAPL_C150 BID qty=5 would add 18,750 vega
    // Total would be 37,500 + 18,750 = 56,250 > 50,000 limit
    auto order = create_order("ORD002", "AAPL_C150", "AAPL", Side::BID, 5.0, 5);
    auto result = engine.pre_trade_check(order);

    EXPECT_TRUE(result.would_breach) << "Should breach gross vega limit";
    EXPECT_TRUE(result.has_breach(LimitType::GROSS_VEGA));

    const auto* breach = result.get_breach(LimitType::GROSS_VEGA);
    ASSERT_NE(breach, nullptr);
    EXPECT_DOUBLE_EQ(breach->current_usage, 37500.0);
    EXPECT_DOUBLE_EQ(breach->hypothetical_usage, 56250.0);
    EXPECT_DOUBLE_EQ(breach->limit_value, 50000.0);
}

// ============================================================================
// Test: Stock order passes vega limit (zero vega contribution)
// ============================================================================

TEST_F(VegaDeltaCombinedTest, StockOrderPassesVegaLimit) {
    // Set a very low gross vega limit
    engine.set_limit<UnderlyerGrossVega>(UnderlyerKey{"AAPL"}, 1000.0);

    // AAPL stock order: qty=1000 (huge position)
    // vega_exposure = 1000 * 0 * 1 * 150 * 1 = 0 (stocks have zero vega)
    // delta_exposure = 1000 * 1 * 1 * 150 * 1 = 150,000

    auto order = create_order("ORD001", "AAPL", "AAPL", Side::BID, 150.0, 1000);
    auto result = engine.pre_trade_check(order);

    // Stock has zero vega, so it should NOT breach vega limit
    EXPECT_FALSE(result.has_breach(LimitType::GROSS_VEGA))
        << "Stock has zero vega, should not breach vega limit";

    // But it may breach delta limit (if we had set one lower)
    engine.set_limit<UnderlyerGrossDelta>(UnderlyerKey{"AAPL"}, 100000.0);
    result = engine.pre_trade_check(order);
    EXPECT_TRUE(result.has_breach(LimitType::GROSS_DELTA))
        << "Stock DOES breach delta limit";
}

// ============================================================================
// Test: Net vega limit check with opposing positions
// ============================================================================

TEST_F(VegaDeltaCombinedTest, NetVegaLimitWithOpposingPositions) {
    // Set net vega limit
    engine.set_limit<UnderlyerNetVega>(UnderlyerKey{"AAPL"}, 20000.0);

    // AAPL_C150 BID qty=10: net_vega = +37,500 (breaches limit)
    auto order1 = create_order("ORD001", "AAPL_C150", "AAPL", Side::BID, 5.0, 10);
    auto result1 = engine.pre_trade_check(order1);
    EXPECT_TRUE(result1.has_breach(LimitType::NET_VEGA)) << "BID order alone breaches net vega limit";

    // But if we have existing ASK position, the new BID might be OK
    // First establish ASK position: qty=6 -> net_vega = -22,500 - fill it
    engine.on_new_order_single(create_order("ORD002", "AAPL_C150", "AAPL", Side::ASK, 5.0, 6));
    engine.on_execution_report(create_ack("ORD002", 6));
    engine.on_execution_report(create_fill("ORD002", 6, 0, 5.0));

    EXPECT_DOUBLE_EQ(net_vega("AAPL"), -22500.0);

    // Now check smaller BID order: qty=5 -> +18,750
    // Net would be -22,500 + 18,750 = -3,750 (within limit of 20,000)
    auto order2 = create_order("ORD003", "AAPL_C150", "AAPL", Side::BID, 5.0, 5);
    auto result2 = engine.pre_trade_check(order2);
    EXPECT_FALSE(result2.has_breach(LimitType::NET_VEGA))
        << "Net vega -3750 is within limit of 20000";
}

// ============================================================================
// Test: Put option with negative delta and positive vega
// ============================================================================

TEST_F(VegaDeltaCombinedTest, PutOptionNegativeDeltaPositiveVega) {
    // AAPL_P150: delta=-0.4, vega=0.20, contract_size=100, underlyer_spot=$150
    // For BID qty=10:
    //   delta_exposure = 10 * (-0.4) * 100 * 150 * 1 = -60,000
    //   gross_delta = |-60,000| = 60,000
    //   net_delta (BID) = -60,000 (negative because delta is negative)
    //   vega_exposure = 10 * 0.20 * 100 * 150 * 1 = 30,000
    //   gross_vega = 30,000
    //   net_vega (BID) = +30,000

    engine.on_new_order_single(create_order("ORD001", "AAPL_P150", "AAPL", Side::BID, 3.0, 10));
    engine.on_execution_report(create_ack("ORD001", 10));
    engine.on_execution_report(create_fill("ORD001", 10, 0, 3.0));

    double expected_delta = expected_delta_exposure("AAPL_P150", 10);  // -60,000
    double expected_vega = expected_vega_exposure("AAPL_P150", 10);    // 30,000

    EXPECT_DOUBLE_EQ(expected_delta, -60000.0) << "Put has negative delta";
    EXPECT_DOUBLE_EQ(expected_vega, 30000.0) << "Put has positive vega";

    EXPECT_DOUBLE_EQ(gross_delta("AAPL"), 60000.0) << "Gross delta = |delta_exposure|";
    EXPECT_DOUBLE_EQ(net_delta("AAPL"), -60000.0) << "Net delta is negative (put BID)";
    EXPECT_DOUBLE_EQ(gross_vega("AAPL"), 30000.0) << "Gross vega = vega_exposure";
    EXPECT_DOUBLE_EQ(net_vega("AAPL"), 30000.0) << "Net vega positive (BID)";
}

// ============================================================================
// Test: Multiple underlyers are independent
// ============================================================================

TEST_F(VegaDeltaCombinedTest, MultipleUnderlyersIndependent) {
    // AAPL option order - fill it
    engine.on_new_order_single(create_order("ORD001", "AAPL_C150", "AAPL", Side::BID, 5.0, 10));
    engine.on_execution_report(create_ack("ORD001", 10));
    engine.on_execution_report(create_fill("ORD001", 10, 0, 5.0));

    // MSFT option order - fill it
    // MSFT_C300: delta=0.6, vega=0.30, contract_size=100, underlyer_spot=$300
    // delta_exposure = 10 * 0.6 * 100 * 300 = 180,000
    // vega_exposure = 10 * 0.30 * 100 * 300 = 90,000
    engine.on_new_order_single(create_order("ORD002", "MSFT_C300", "MSFT", Side::BID, 8.0, 10));
    engine.on_execution_report(create_ack("ORD002", 10));
    engine.on_execution_report(create_fill("ORD002", 10, 0, 8.0));

    // AAPL metrics
    EXPECT_DOUBLE_EQ(gross_delta("AAPL"), 75000.0);
    EXPECT_DOUBLE_EQ(gross_vega("AAPL"), 37500.0);

    // MSFT metrics (independent)
    EXPECT_DOUBLE_EQ(gross_delta("MSFT"), 180000.0);
    EXPECT_DOUBLE_EQ(gross_vega("MSFT"), 90000.0);

    // Verify AAPL unchanged after MSFT order
    EXPECT_DOUBLE_EQ(gross_delta("AAPL"), 75000.0);
    EXPECT_DOUBLE_EQ(gross_vega("AAPL"), 37500.0);
}

// ============================================================================
// Test: Partial and full fills accumulate in position stage
// ============================================================================

TEST_F(VegaDeltaCombinedTest, PartialAndFullFillsAccumulateInPosition) {
    // Insert order
    engine.on_new_order_single(create_order("ORD001", "AAPL_C150", "AAPL", Side::BID, 5.0, 10));

    // Before any fills, position is 0 (we only track position stage)
    EXPECT_DOUBLE_EQ(gross_delta("AAPL"), 0.0);
    EXPECT_DOUBLE_EQ(gross_vega("AAPL"), 0.0);

    // Ack moves to open (still no position)
    engine.on_execution_report(create_ack("ORD001", 10));

    EXPECT_DOUBLE_EQ(gross_delta("AAPL"), 0.0);
    EXPECT_DOUBLE_EQ(gross_vega("AAPL"), 0.0);

    // Partial fill: 5 contracts filled
    // Position: 5 * 0.5 * 100 * 150 = 37,500 delta
    // Position: 5 * 0.25 * 100 * 150 = 18,750 vega
    engine.on_execution_report(create_fill("ORD001", 5, 5, 5.0));

    EXPECT_DOUBLE_EQ(gross_delta("AAPL"), 37500.0) << "Partial fill: 5 contracts";
    EXPECT_DOUBLE_EQ(gross_vega("AAPL"), 18750.0) << "Partial fill: 5 contracts";

    // Full fill (remaining 5)
    // Total position: 10 contracts
    engine.on_execution_report(create_fill("ORD001", 5, 0, 5.0));

    // Order fully filled, all 10 contracts in position stage
    EXPECT_DOUBLE_EQ(gross_delta("AAPL"), 75000.0);
    EXPECT_DOUBLE_EQ(gross_vega("AAPL"), 37500.0);
}

// ============================================================================
// Test: Clear resets all metrics
// ============================================================================

TEST_F(VegaDeltaCombinedTest, ClearResetsAllMetrics) {
    // Create some positions - fill them
    engine.on_new_order_single(create_order("ORD001", "AAPL_C150", "AAPL", Side::BID, 5.0, 10));
    engine.on_execution_report(create_ack("ORD001", 10));
    engine.on_execution_report(create_fill("ORD001", 10, 0, 5.0));
    engine.on_new_order_single(create_order("ORD002", "AAPL", "AAPL", Side::BID, 150.0, 100));
    engine.on_execution_report(create_ack("ORD002", 100));
    engine.on_execution_report(create_fill("ORD002", 100, 0, 150.0));

    EXPECT_GT(gross_delta("AAPL"), 0.0);
    EXPECT_GT(gross_vega("AAPL"), 0.0);

    engine.clear();

    EXPECT_DOUBLE_EQ(gross_delta("AAPL"), 0.0);
    EXPECT_DOUBLE_EQ(net_delta("AAPL"), 0.0);
    EXPECT_DOUBLE_EQ(gross_vega("AAPL"), 0.0);
    EXPECT_DOUBLE_EQ(net_vega("AAPL"), 0.0);
}

// ============================================================================
// Test: Pre-trade check can breach both delta and vega limits
// ============================================================================

TEST_F(VegaDeltaCombinedTest, PreTradeCheckBreachesBothDeltaAndVega) {
    // Set very low limits
    engine.set_limit<UnderlyerGrossDelta>(UnderlyerKey{"AAPL"}, 10000.0);
    engine.set_limit<UnderlyerGrossVega>(UnderlyerKey{"AAPL"}, 5000.0);

    // AAPL_C150 BID qty=10: delta=75,000, vega=37,500
    // Both exceed limits
    auto order = create_order("ORD001", "AAPL_C150", "AAPL", Side::BID, 5.0, 10);
    auto result = engine.pre_trade_check(order);

    EXPECT_TRUE(result.would_breach);
    EXPECT_TRUE(result.has_breach(LimitType::GROSS_DELTA))
        << "Breaches gross delta limit: 75000 > 10000";
    EXPECT_TRUE(result.has_breach(LimitType::GROSS_VEGA))
        << "Breaches gross vega limit: 37500 > 5000";

    // Verify breach details
    const auto* delta_breach = result.get_breach(LimitType::GROSS_DELTA);
    ASSERT_NE(delta_breach, nullptr);
    EXPECT_DOUBLE_EQ(delta_breach->hypothetical_usage, 75000.0);
    EXPECT_DOUBLE_EQ(delta_breach->limit_value, 10000.0);

    const auto* vega_breach = result.get_breach(LimitType::GROSS_VEGA);
    ASSERT_NE(vega_breach, nullptr);
    EXPECT_DOUBLE_EQ(vega_breach->hypothetical_usage, 37500.0);
    EXPECT_DOUBLE_EQ(vega_breach->limit_value, 5000.0);
}

// ============================================================================
// Test: Verify vega computation formula matches expectations
// ============================================================================

TEST_F(VegaDeltaCombinedTest, VerifyVegaComputationFormula) {
    // Verify: vega_exposure = quantity * vega * contract_size * underlyer_spot * fx_rate

    // AAPL_C150: vega=0.25, contract_size=100, underlyer_spot=150, fx_rate=1.0
    // qty=1: vega_exposure = 1 * 0.25 * 100 * 150 * 1.0 = 3,750
    double single_contract = expected_vega_exposure("AAPL_C150", 1);
    EXPECT_DOUBLE_EQ(single_contract, 3750.0);

    // qty=10: vega_exposure = 10 * 0.25 * 100 * 150 * 1.0 = 37,500
    double ten_contracts = expected_vega_exposure("AAPL_C150", 10);
    EXPECT_DOUBLE_EQ(ten_contracts, 37500.0);

    // Verify linear scaling
    EXPECT_DOUBLE_EQ(ten_contracts, 10.0 * single_contract);

    // Stock has zero vega
    double stock_vega = expected_vega_exposure("AAPL", 1000);
    EXPECT_DOUBLE_EQ(stock_vega, 0.0) << "Stock vega = 0 regardless of quantity";
}
