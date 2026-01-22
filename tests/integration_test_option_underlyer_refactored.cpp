#include <gtest/gtest.h>
#include "../src/engine/risk_engine_with_limits.hpp"
#include "../src/metrics/order_count_metric.hpp"
#include "../src/fix/fix_parser.hpp"

using namespace engine;
using namespace fix;
using namespace aggregation;
using namespace metrics;

// ============================================================================
// Refactored Option Underlyer Order Count Test
// ============================================================================
//
// This test uses 3 separate single-purpose metrics:
//   1. OpenOrdersPerInstrumentSide: Orders per instrument-side (open stage only), limit=1
//   2. InFlightOrdersPerInstrumentSide: Orders per instrument-side (in-flight stage only)
//   3. OpenOrdersPerUnderlyer: Quoted instruments per underlyer (open stage only)
//
// After every test step, we explicitly assert the current state of all 3 metrics.
//

namespace {

// Helper functions
NewOrderSingle create_order(const std::string& cl_ord_id, const std::string& symbol,
                             const std::string& underlyer, Side side,
                             double price, int64_t qty) {
    NewOrderSingle order;
    order.key.cl_ord_id = cl_ord_id;
    order.symbol = symbol;
    order.underlyer = underlyer;
    order.side = side;
    order.price = price;
    order.quantity = qty;
    order.strategy_id = "STRAT1";
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

ExecutionReport create_nack(const std::string& cl_ord_id) {
    ExecutionReport report;
    report.key.cl_ord_id = cl_ord_id;
    report.order_id = "EX" + cl_ord_id;
    report.ord_status = OrdStatus::REJECTED;
    report.exec_type = ExecType::REJECTED;
    report.leaves_qty = 0;
    report.cum_qty = 0;
    report.is_unsolicited = false;
    return report;
}

ExecutionReport create_cancel_ack(const std::string& cancel_id, const std::string& orig_id) {
    ExecutionReport report;
    report.key.cl_ord_id = cancel_id;
    report.order_id = "EX" + orig_id;
    report.ord_status = OrdStatus::CANCELED;
    report.exec_type = ExecType::CANCELED;
    report.leaves_qty = 0;
    report.cum_qty = 0;
    report.is_unsolicited = false;
    report.orig_key = OrderKey{orig_id};
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

OrderCancelRequest create_cancel_request(const std::string& cancel_id, const std::string& orig_id,
                                          const std::string& symbol, Side side) {
    OrderCancelRequest req;
    req.key.cl_ord_id = cancel_id;
    req.orig_key.cl_ord_id = orig_id;
    req.symbol = symbol;
    req.side = side;
    return req;
}

ExecutionReport create_unsolicited_cancel(const std::string& cl_ord_id) {
    ExecutionReport report;
    report.key.cl_ord_id = cl_ord_id;
    report.order_id = "EX" + cl_ord_id;
    report.ord_status = OrdStatus::CANCELED;
    report.exec_type = ExecType::CANCELED;
    report.leaves_qty = 0;
    report.cum_qty = 0;
    report.is_unsolicited = true;
    return report;
}

}  // namespace

class OptionUnderlyerRefactoredTest : public ::testing::Test {
protected:
    // Define single-purpose metrics
    using OpenOrdersPerSide = OrderCountMetric<InstrumentSideKey, OpenStage>;
    using InFlightOrdersPerSide = OrderCountMetric<InstrumentSideKey, InFlightStage>;
    using OpenQuotedInstruments = QuotedInstrumentCountMetric<OpenStage>;

    using TestEngine = RiskAggregationEngineWithLimits<
        void,  // No provider needed
        OpenOrdersPerSide,
        InFlightOrdersPerSide,
        OpenQuotedInstruments
    >;

    TestEngine engine;

    // Limits
    static constexpr int64_t MAX_OPEN_PER_SIDE = 1;
    static constexpr int64_t MAX_QUOTED_INSTRUMENTS = 2;

    void SetUp() override {
        // Configure limits
        engine.set_default_order_count_limit(MAX_OPEN_PER_SIDE);
        engine.set_default_quoted_instruments_limit(MAX_QUOTED_INSTRUMENTS);
    }

    // Accessors
    int64_t open_orders(const std::string& symbol, Side side) const {
        return engine.get_metric<OpenOrdersPerSide>().get(
            InstrumentSideKey{symbol, static_cast<int>(side)});
    }

    int64_t in_flight_orders(const std::string& symbol, Side side) const {
        return engine.get_metric<InFlightOrdersPerSide>().get(
            InstrumentSideKey{symbol, static_cast<int>(side)});
    }

    int64_t quoted_instruments(const std::string& underlyer) const {
        return engine.get_metric<OpenQuotedInstruments>().get(UnderlyerKey{underlyer});
    }

    // Helper to assert all metrics at once
    struct MetricState {
        std::string symbol;
        std::string underlyer;
        int64_t open_bid = 0;
        int64_t open_ask = 0;
        int64_t in_flight_bid = 0;
        int64_t in_flight_ask = 0;
        int64_t quoted_count = 0;
    };

    void assert_state(const MetricState& expected, const std::string& step_name) const {
        EXPECT_EQ(open_orders(expected.symbol, Side::BID), expected.open_bid)
            << step_name << ": open_bid for " << expected.symbol;
        EXPECT_EQ(open_orders(expected.symbol, Side::ASK), expected.open_ask)
            << step_name << ": open_ask for " << expected.symbol;
        EXPECT_EQ(in_flight_orders(expected.symbol, Side::BID), expected.in_flight_bid)
            << step_name << ": in_flight_bid for " << expected.symbol;
        EXPECT_EQ(in_flight_orders(expected.symbol, Side::ASK), expected.in_flight_ask)
            << step_name << ": in_flight_ask for " << expected.symbol;
        EXPECT_EQ(quoted_instruments(expected.underlyer), expected.quoted_count)
            << step_name << ": quoted_instruments for " << expected.underlyer;
    }
};

// ============================================================================
// Test: Full order lifecycle with explicit assertions after every step
// ============================================================================

TEST_F(OptionUnderlyerRefactoredTest, FullLifecycleWithExplicitAssertions) {
    const std::string OPT1 = "AAPL_OPT1";
    const std::string OPT2 = "AAPL_OPT2";
    const std::string AAPL = "AAPL";

    // Initial state
    assert_state({OPT1, AAPL, 0, 0, 0, 0, 0}, "Initial");

    // Step 1: INSERT ORD001 (OPT1, BID)
    engine.on_new_order_single(create_order("ORD001", OPT1, AAPL, Side::BID, 5.0, 100));
    // in_flight_bid=1, quoted=0 (only counts open stage)
    assert_state({OPT1, AAPL, 0, 0, 1, 0, 0}, "Step 1: INSERT ORD001");

    // Step 2: ACK ORD001
    engine.on_execution_report(create_ack("ORD001", 100));
    // open_bid=1, in_flight_bid=0, quoted=1
    assert_state({OPT1, AAPL, 1, 0, 0, 0, 1}, "Step 2: ACK ORD001");

    // Step 3: INSERT ORD002 (OPT1, ASK)
    engine.on_new_order_single(create_order("ORD002", OPT1, AAPL, Side::ASK, 5.5, 50));
    // in_flight_ask=1, quoted still 1
    assert_state({OPT1, AAPL, 1, 0, 0, 1, 1}, "Step 3: INSERT ORD002");

    // Step 4: ACK ORD002
    engine.on_execution_report(create_ack("ORD002", 50));
    // open_ask=1, in_flight_ask=0, quoted still 1 (same instrument)
    assert_state({OPT1, AAPL, 1, 1, 0, 0, 1}, "Step 4: ACK ORD002");

    // Step 5: INSERT ORD003 (OPT2, BID) - new instrument
    engine.on_new_order_single(create_order("ORD003", OPT2, AAPL, Side::BID, 3.0, 75));
    // OPT2: in_flight_bid=1, quoted still 1 (only counts open)
    EXPECT_EQ(in_flight_orders(OPT2, Side::BID), 1) << "Step 5: in_flight_bid for OPT2";
    EXPECT_EQ(quoted_instruments(AAPL), 1) << "Step 5: quoted_instruments for AAPL";

    // Step 6: ACK ORD003
    engine.on_execution_report(create_ack("ORD003", 75));
    // OPT2: open_bid=1, quoted now 2
    EXPECT_EQ(open_orders(OPT2, Side::BID), 1) << "Step 6: open_bid for OPT2";
    EXPECT_EQ(quoted_instruments(AAPL), 2) << "Step 6: quoted_instruments for AAPL";

    // Step 7: PARTIAL_FILL ORD001 (doesn't change counts)
    engine.on_execution_report(create_fill("ORD001", 50, 50, 5.0));
    assert_state({OPT1, AAPL, 1, 1, 0, 0, 2}, "Step 7: PARTIAL_FILL ORD001");

    // Step 8: FULL_FILL ORD001
    engine.on_execution_report(create_fill("ORD001", 50, 0, 5.0));
    // OPT1: open_bid=0, quoted still 2 (OPT1 still has ASK, OPT2 has BID)
    assert_state({OPT1, AAPL, 0, 1, 0, 0, 2}, "Step 8: FULL_FILL ORD001");

    // Step 9: CANCEL_REQUEST ORD002
    engine.on_order_cancel_request(create_cancel_request("CXL001", "ORD002", OPT1, Side::ASK));
    // OPT1: open_ask=0, in_flight_ask=1, quoted drops to 1 (OPT1 has no open orders, only OPT2 open)
    assert_state({OPT1, AAPL, 0, 0, 0, 1, 1}, "Step 9: CANCEL_REQUEST ORD002");

    // Step 10: CANCEL_ACK ORD002
    engine.on_execution_report(create_cancel_ack("CXL001", "ORD002"));
    // OPT1: all zeros, quoted now 1 (only OPT2 remains)
    assert_state({OPT1, AAPL, 0, 0, 0, 0, 1}, "Step 10: CANCEL_ACK ORD002");

    // Step 11: UNSOLICITED_CANCEL ORD003
    engine.on_execution_report(create_unsolicited_cancel("ORD003"));
    // All orders gone, quoted=0
    EXPECT_EQ(open_orders(OPT2, Side::BID), 0) << "Step 11: open_bid for OPT2";
    EXPECT_EQ(quoted_instruments(AAPL), 0) << "Step 11: quoted_instruments for AAPL";
}

// ============================================================================
// Test: Limit enforcement with 3 metrics
// ============================================================================

TEST_F(OptionUnderlyerRefactoredTest, LimitEnforcement) {
    const std::string OPT1 = "AAPL_OPT1";
    const std::string OPT2 = "AAPL_OPT2";
    const std::string OPT3 = "AAPL_OPT3";
    const std::string AAPL = "AAPL";

    // Step 1: INSERT & ACK OPT1 BID
    auto order1 = create_order("ORD001", OPT1, AAPL, Side::BID, 5.0, 100);
    engine.on_new_order_single(order1);
    // After INSERT, order is in-flight, which counts towards the order count limit
    auto check1 = engine.pre_trade_check(create_order("X", OPT1, AAPL, Side::BID, 5.0, 100));
    EXPECT_TRUE(check1.would_breach) << "After INSERT, in-flight counts towards limit";
    EXPECT_TRUE(check1.has_breach(LimitType::ORDER_COUNT));

    engine.on_execution_report(create_ack("ORD001", 100));

    // OPT1 BID now at limit (1 open)
    EXPECT_TRUE(engine.pre_trade_check(create_order("X", OPT1, AAPL, Side::BID, 5.0, 100)).would_breach)
        << "OPT1 BID at limit";
    EXPECT_FALSE(engine.pre_trade_check(create_order("X", OPT1, AAPL, Side::ASK, 5.0, 100)).would_breach)
        << "OPT1 ASK not at limit";
    EXPECT_FALSE(engine.pre_trade_check(create_order("X", OPT2, AAPL, Side::BID, 6.0, 50)).would_breach)
        << "OPT2 BID not at limit";

    // Step 2: INSERT & ACK OPT2 ASK
    engine.on_new_order_single(create_order("ORD002", OPT2, AAPL, Side::ASK, 6.0, 50));
    engine.on_execution_report(create_ack("ORD002", 50));

    // OPT2 ASK now at limit, quoted=2
    EXPECT_TRUE(engine.pre_trade_check(create_order("X", OPT2, AAPL, Side::ASK, 6.0, 50)).would_breach)
        << "OPT2 ASK at limit";
    EXPECT_EQ(quoted_instruments(AAPL), 2) << "Quoted instruments = 2";

    // Step 3: Try to INSERT OPT3 - would breach quoted instruments limit
    auto opt3_order = create_order("ORD003", OPT3, AAPL, Side::BID, 4.0, 100);
    auto check3 = engine.pre_trade_check(opt3_order);
    // OPT3 doesn't have an order count breach (per-side is free)
    // But it would breach quoted instruments limit
    EXPECT_TRUE(check3.would_breach) << "OPT3 should breach quoted instruments limit";
    EXPECT_TRUE(check3.has_breach(LimitType::QUOTED_INSTRUMENTS));
}

// ============================================================================
// Test: NACK releases resources
// ============================================================================

TEST_F(OptionUnderlyerRefactoredTest, NackReleasesResources) {
    const std::string OPT1 = "AAPL_OPT1";
    const std::string AAPL = "AAPL";

    // INSERT
    engine.on_new_order_single(create_order("ORD001", OPT1, AAPL, Side::BID, 5.0, 100));
    EXPECT_EQ(in_flight_orders(OPT1, Side::BID), 1) << "After INSERT: in_flight=1";
    EXPECT_EQ(quoted_instruments(AAPL), 0) << "After INSERT: quoted=0 (not open yet)";

    // NACK
    engine.on_execution_report(create_nack("ORD001"));
    EXPECT_EQ(in_flight_orders(OPT1, Side::BID), 0) << "After NACK: in_flight=0";
    EXPECT_EQ(open_orders(OPT1, Side::BID), 0) << "After NACK: open=0";
    EXPECT_EQ(quoted_instruments(AAPL), 0) << "After NACK: quoted=0";
}

// ============================================================================
// Test: Multiple underlyers are independent
// ============================================================================

TEST_F(OptionUnderlyerRefactoredTest, MultipleUnderlyersIndependent) {
    const std::string AAPL_OPT = "AAPL_OPT1";
    const std::string MSFT_OPT = "MSFT_OPT1";
    const std::string AAPL = "AAPL";
    const std::string MSFT = "MSFT";

    // AAPL order
    engine.on_new_order_single(create_order("ORD001", AAPL_OPT, AAPL, Side::BID, 5.0, 100));
    engine.on_execution_report(create_ack("ORD001", 100));

    EXPECT_EQ(open_orders(AAPL_OPT, Side::BID), 1);
    EXPECT_EQ(quoted_instruments(AAPL), 1);
    EXPECT_EQ(quoted_instruments(MSFT), 0);

    // MSFT order
    engine.on_new_order_single(create_order("ORD002", MSFT_OPT, MSFT, Side::ASK, 10.0, 50));
    engine.on_execution_report(create_ack("ORD002", 50));

    EXPECT_EQ(open_orders(MSFT_OPT, Side::ASK), 1);
    EXPECT_EQ(quoted_instruments(AAPL), 1);
    EXPECT_EQ(quoted_instruments(MSFT), 1);

    // Cancel AAPL - doesn't affect MSFT
    engine.on_order_cancel_request(create_cancel_request("CXL001", "ORD001", AAPL_OPT, Side::BID));
    engine.on_execution_report(create_cancel_ack("CXL001", "ORD001"));

    EXPECT_EQ(quoted_instruments(AAPL), 0);
    EXPECT_EQ(quoted_instruments(MSFT), 1);
}

// ============================================================================
// Test: Same instrument, multiple orders (only counts unique instruments)
// ============================================================================

TEST_F(OptionUnderlyerRefactoredTest, SameInstrumentMultipleOrders) {
    const std::string OPT1 = "AAPL_OPT1";
    const std::string AAPL = "AAPL";

    // First order on OPT1
    engine.on_new_order_single(create_order("ORD001", OPT1, AAPL, Side::BID, 5.0, 100));
    engine.on_execution_report(create_ack("ORD001", 100));

    EXPECT_EQ(open_orders(OPT1, Side::BID), 1);
    EXPECT_EQ(quoted_instruments(AAPL), 1);

    // Second order on SAME instrument (ASK side)
    engine.on_new_order_single(create_order("ORD002", OPT1, AAPL, Side::ASK, 5.5, 50));
    engine.on_execution_report(create_ack("ORD002", 50));

    EXPECT_EQ(open_orders(OPT1, Side::BID), 1);
    EXPECT_EQ(open_orders(OPT1, Side::ASK), 1);
    EXPECT_EQ(quoted_instruments(AAPL), 1) << "Still 1 - same instrument";

    // Fill BID order
    engine.on_execution_report(create_fill("ORD001", 100, 0, 5.0));

    EXPECT_EQ(open_orders(OPT1, Side::BID), 0);
    EXPECT_EQ(open_orders(OPT1, Side::ASK), 1);
    EXPECT_EQ(quoted_instruments(AAPL), 1) << "Still 1 - ASK still open";

    // Fill ASK order
    engine.on_execution_report(create_fill("ORD002", 50, 0, 5.5));

    EXPECT_EQ(open_orders(OPT1, Side::BID), 0);
    EXPECT_EQ(open_orders(OPT1, Side::ASK), 0);
    EXPECT_EQ(quoted_instruments(AAPL), 0) << "Now 0 - no more orders";
}

// ============================================================================
// Test: Clear resets all metrics
// ============================================================================

TEST_F(OptionUnderlyerRefactoredTest, ClearResetsAll) {
    const std::string OPT1 = "AAPL_OPT1";
    const std::string OPT2 = "AAPL_OPT2";
    const std::string AAPL = "AAPL";

    // Add some orders
    engine.on_new_order_single(create_order("ORD001", OPT1, AAPL, Side::BID, 5.0, 100));
    engine.on_execution_report(create_ack("ORD001", 100));
    engine.on_new_order_single(create_order("ORD002", OPT2, AAPL, Side::ASK, 6.0, 50));

    EXPECT_EQ(open_orders(OPT1, Side::BID), 1);
    EXPECT_EQ(in_flight_orders(OPT2, Side::ASK), 1);
    EXPECT_EQ(quoted_instruments(AAPL), 1);

    // Clear
    engine.clear();

    EXPECT_EQ(open_orders(OPT1, Side::BID), 0);
    EXPECT_EQ(in_flight_orders(OPT2, Side::ASK), 0);
    EXPECT_EQ(quoted_instruments(AAPL), 0);
}

// ============================================================================
// Test: Pre-trade check result contains useful information
// ============================================================================

TEST_F(OptionUnderlyerRefactoredTest, PreTradeCheckResultDetails) {
    const std::string OPT1 = "AAPL_OPT1";
    const std::string AAPL = "AAPL";

    // Send order to hit limit
    engine.on_new_order_single(create_order("ORD001", OPT1, AAPL, Side::BID, 5.0, 100));
    engine.on_execution_report(create_ack("ORD001", 100));

    // Check pre-trade for new order on same instrument-side
    auto result = engine.pre_trade_check(create_order("ORD002", OPT1, AAPL, Side::BID, 5.0, 100));
    EXPECT_TRUE(result.would_breach);
    EXPECT_FALSE(result);  // operator bool returns !would_breach

    // Verify breach details
    const auto* breach = result.get_breach(LimitType::ORDER_COUNT);
    ASSERT_NE(breach, nullptr);
    EXPECT_DOUBLE_EQ(breach->current_usage, 1.0);
    EXPECT_DOUBLE_EQ(breach->hypothetical_usage, 2.0);
    EXPECT_DOUBLE_EQ(breach->limit_value, 1.0);

    // Verify to_string()
    std::string str = result.to_string();
    EXPECT_NE(str.find("ORDER_COUNT"), std::string::npos);
    EXPECT_NE(str.find("FAILED"), std::string::npos);
}
