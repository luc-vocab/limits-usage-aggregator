#include <gtest/gtest.h>
#include "../src/engine/risk_engine_with_limits.hpp"
#include "../src/metrics/order_count_metric.hpp"
#include "../src/fix/fix_parser.hpp"

using namespace engine;
using namespace fix;
using namespace aggregation;
using namespace metrics;
using namespace instrument;

// ============================================================================
// Helper functions
// ============================================================================

namespace {

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

}  // namespace

// ============================================================================
// Test: Open and In-Flight Orders by Instrument-Side
// ============================================================================
//
// This test verifies that we can track open and in-flight orders separately
// per instrument-side combination, with a limit of 1 for each.
//
// Metrics used:
//   - OpenOrderCount: OrderCountMetric<InstrumentSideKey, OpenStage>
//   - InFlightOrderCount: OrderCountMetric<InstrumentSideKey, InFlightStage>
//

class OrderCountByInstrumentSideTest : public ::testing::Test {
protected:
    // Define the engine with single-purpose metrics
    // void is used because order count metrics don't need context or instrument data
    using OpenOrderCount = OrderCountMetric<InstrumentSideKey, OpenStage>;
    using InFlightOrderCount = OrderCountMetric<InstrumentSideKey, InFlightStage>;

    using TestEngine = RiskAggregationEngineWithLimits<
        void,
        void,
        OpenOrderCount,
        InFlightOrderCount
    >;

    TestEngine engine;

    // Limits
    static constexpr int64_t MAX_OPEN_PER_SIDE = 1;
    static constexpr int64_t MAX_IN_FLIGHT_PER_SIDE = 1;

    void SetUp() override {
        // Configure the order count limit for both open and in-flight metrics
        engine.set_default_limit<OpenOrderCount>(MAX_OPEN_PER_SIDE);
        engine.set_default_limit<InFlightOrderCount>(MAX_IN_FLIGHT_PER_SIDE);
    }

    // Helper to get counts
    int64_t open_count(const std::string& symbol, Side side) const {
        InstrumentSideKey key{symbol, static_cast<int>(side)};
        return engine.get_metric<OpenOrderCount>().get(key);
    }

    int64_t in_flight_count(const std::string& symbol, Side side) const {
        InstrumentSideKey key{symbol, static_cast<int>(side)};
        return engine.get_metric<InFlightOrderCount>().get(key);
    }
};

TEST_F(OrderCountByInstrumentSideTest, SingleOrderLifecycle) {
    const std::string SYMBOL = "AAPL";

    // Step 1: Send order
    engine.on_new_order_single(create_order("ORD001", SYMBOL, SYMBOL, Side::BID, 150.0, 100));

    // Assert: in-flight=1, open=0
    EXPECT_EQ(in_flight_count(SYMBOL, Side::BID), 1);
    EXPECT_EQ(open_count(SYMBOL, Side::BID), 0);
    EXPECT_EQ(in_flight_count(SYMBOL, Side::ASK), 0);
    EXPECT_EQ(open_count(SYMBOL, Side::ASK), 0);

    // Step 2: Receive ACK
    engine.on_execution_report(create_ack("ORD001", 100));

    // Assert: in-flight=0, open=1
    EXPECT_EQ(in_flight_count(SYMBOL, Side::BID), 0);
    EXPECT_EQ(open_count(SYMBOL, Side::BID), 1);

    // Step 3: Request cancel
    engine.on_order_cancel_request(create_cancel_request("CXL001", "ORD001", SYMBOL, Side::BID));

    // Assert: order moves to in-flight (pending cancel)
    EXPECT_EQ(in_flight_count(SYMBOL, Side::BID), 1);
    EXPECT_EQ(open_count(SYMBOL, Side::BID), 0);

    // Step 4: Cancel ACK
    engine.on_execution_report(create_cancel_ack("CXL001", "ORD001"));

    // Assert: all counts back to 0
    EXPECT_EQ(in_flight_count(SYMBOL, Side::BID), 0);
    EXPECT_EQ(open_count(SYMBOL, Side::BID), 0);
}

TEST_F(OrderCountByInstrumentSideTest, LimitEnforcement) {
    const std::string SYMBOL = "AAPL";

    // Step 1: Check pre-trade for first order (BID)
    auto order1 = create_order("ORD001", SYMBOL, SYMBOL, Side::BID, 150.0, 100);
    auto result1 = engine.pre_trade_check(order1);
    EXPECT_FALSE(result1.would_breach) << result1.to_string();
    engine.on_new_order_single(order1);

    // Assert: would breach limit for new BID order
    auto order2 = create_order("ORD002", SYMBOL, SYMBOL, Side::BID, 150.0, 100);
    auto result2 = engine.pre_trade_check(order2);
    EXPECT_TRUE(result2.would_breach) << "BID should be at limit";
    EXPECT_TRUE(result2.has_breach(LimitType::ORDER_COUNT));

    // ASK still available
    auto ask_order = create_order("ORD003", SYMBOL, SYMBOL, Side::ASK, 151.0, 50);
    auto ask_result = engine.pre_trade_check(ask_order);
    EXPECT_FALSE(ask_result.would_breach) << ask_result.to_string();

    // Step 2: ACK first order
    engine.on_execution_report(create_ack("ORD001", 100));

    // Assert: still at limit (open=1)
    auto result3 = engine.pre_trade_check(order2);
    EXPECT_TRUE(result3.would_breach) << "BID should still be at limit after ACK";

    // Step 3: Send ASK order (should be allowed)
    EXPECT_FALSE(engine.pre_trade_check(ask_order).would_breach);
    engine.on_new_order_single(ask_order);

    // Assert: ASK now at limit
    auto ask_order2 = create_order("ORD004", SYMBOL, SYMBOL, Side::ASK, 151.0, 50);
    EXPECT_TRUE(engine.pre_trade_check(ask_order2).would_breach);
    EXPECT_EQ(in_flight_count(SYMBOL, Side::ASK), 1);
}

TEST_F(OrderCountByInstrumentSideTest, MultipleInstruments) {
    // Each instrument has independent limits
    engine.on_new_order_single(create_order("ORD001", "AAPL", "AAPL", Side::BID, 150.0, 100));
    engine.on_new_order_single(create_order("ORD002", "MSFT", "MSFT", Side::BID, 300.0, 50));
    engine.on_new_order_single(create_order("ORD003", "GOOG", "GOOG", Side::BID, 100.0, 200));

    // Assert: each instrument has 1 in-flight BID
    EXPECT_EQ(in_flight_count("AAPL", Side::BID), 1);
    EXPECT_EQ(in_flight_count("MSFT", Side::BID), 1);
    EXPECT_EQ(in_flight_count("GOOG", Side::BID), 1);

    // Assert: limits are per-instrument
    EXPECT_TRUE(engine.pre_trade_check(create_order("X", "AAPL", "AAPL", Side::BID, 150.0, 100)).would_breach);
    EXPECT_TRUE(engine.pre_trade_check(create_order("X", "MSFT", "MSFT", Side::BID, 300.0, 50)).would_breach);
    EXPECT_TRUE(engine.pre_trade_check(create_order("X", "GOOG", "GOOG", Side::BID, 100.0, 200)).would_breach);
    EXPECT_FALSE(engine.pre_trade_check(create_order("X", "AAPL", "AAPL", Side::ASK, 150.0, 100)).would_breach);
}

TEST_F(OrderCountByInstrumentSideTest, NackFreesCapacity) {
    const std::string SYMBOL = "AAPL";

    // Send order
    auto order = create_order("ORD001", SYMBOL, SYMBOL, Side::BID, 150.0, 100);
    engine.on_new_order_single(order);
    EXPECT_TRUE(engine.pre_trade_check(create_order("X", SYMBOL, SYMBOL, Side::BID, 150.0, 100)).would_breach);

    // Receive NACK
    engine.on_execution_report(create_nack("ORD001"));

    // Assert: capacity freed
    EXPECT_FALSE(engine.pre_trade_check(create_order("X", SYMBOL, SYMBOL, Side::BID, 150.0, 100)).would_breach);
    EXPECT_EQ(in_flight_count(SYMBOL, Side::BID), 0);
}

TEST_F(OrderCountByInstrumentSideTest, FillRemovesFromOpen) {
    const std::string SYMBOL = "AAPL";

    // Send and ACK order
    engine.on_new_order_single(create_order("ORD001", SYMBOL, SYMBOL, Side::BID, 150.0, 100));
    engine.on_execution_report(create_ack("ORD001", 100));

    EXPECT_EQ(open_count(SYMBOL, Side::BID), 1);
    EXPECT_TRUE(engine.pre_trade_check(create_order("X", SYMBOL, SYMBOL, Side::BID, 150.0, 100)).would_breach);

    // Partial fill - order stays in OPEN
    engine.on_execution_report(create_fill("ORD001", 50, 50, 150.0));
    EXPECT_EQ(open_count(SYMBOL, Side::BID), 1);  // Still open
    EXPECT_TRUE(engine.pre_trade_check(create_order("X", SYMBOL, SYMBOL, Side::BID, 150.0, 100)).would_breach);

    // Full fill - order removed
    engine.on_execution_report(create_fill("ORD001", 50, 0, 150.0));
    EXPECT_EQ(open_count(SYMBOL, Side::BID), 0);
    EXPECT_FALSE(engine.pre_trade_check(create_order("X", SYMBOL, SYMBOL, Side::BID, 150.0, 100)).would_breach);
}

TEST_F(OrderCountByInstrumentSideTest, FullOrderFlowWithAssertions) {
    const std::string SYMBOL = "AAPL";

    // Step 1: Send BID order
    engine.on_new_order_single(create_order("ORD001", SYMBOL, SYMBOL, Side::BID, 150.0, 100));
    EXPECT_EQ(in_flight_count(SYMBOL, Side::BID), 1) << "After INSERT BID";
    EXPECT_EQ(open_count(SYMBOL, Side::BID), 0) << "After INSERT BID";

    // Step 2: Send ASK order
    engine.on_new_order_single(create_order("ORD002", SYMBOL, SYMBOL, Side::ASK, 151.0, 100));
    EXPECT_EQ(in_flight_count(SYMBOL, Side::ASK), 1) << "After INSERT ASK";
    EXPECT_EQ(open_count(SYMBOL, Side::ASK), 0) << "After INSERT ASK";

    // Step 3: ACK BID
    engine.on_execution_report(create_ack("ORD001", 100));
    EXPECT_EQ(in_flight_count(SYMBOL, Side::BID), 0) << "After ACK BID";
    EXPECT_EQ(open_count(SYMBOL, Side::BID), 1) << "After ACK BID";

    // Step 4: ACK ASK
    engine.on_execution_report(create_ack("ORD002", 100));
    EXPECT_EQ(in_flight_count(SYMBOL, Side::ASK), 0) << "After ACK ASK";
    EXPECT_EQ(open_count(SYMBOL, Side::ASK), 1) << "After ACK ASK";

    // Both sides at limit
    EXPECT_TRUE(engine.pre_trade_check(create_order("X", SYMBOL, SYMBOL, Side::BID, 150.0, 100)).would_breach)
        << "BID at limit";
    EXPECT_TRUE(engine.pre_trade_check(create_order("X", SYMBOL, SYMBOL, Side::ASK, 151.0, 100)).would_breach)
        << "ASK at limit";

    // Step 5: Full fill on BID
    engine.on_execution_report(create_fill("ORD001", 100, 0, 150.0));
    EXPECT_EQ(open_count(SYMBOL, Side::BID), 0) << "After FILL BID";
    EXPECT_FALSE(engine.pre_trade_check(create_order("X", SYMBOL, SYMBOL, Side::BID, 150.0, 100)).would_breach)
        << "BID capacity freed";

    // Step 6: Cancel ASK
    engine.on_order_cancel_request(create_cancel_request("CXL001", "ORD002", SYMBOL, Side::ASK));
    EXPECT_EQ(in_flight_count(SYMBOL, Side::ASK), 1) << "After CANCEL_REQ ASK";
    EXPECT_EQ(open_count(SYMBOL, Side::ASK), 0) << "After CANCEL_REQ ASK";

    engine.on_execution_report(create_cancel_ack("CXL001", "ORD002"));
    EXPECT_EQ(in_flight_count(SYMBOL, Side::ASK), 0) << "After CANCEL_ACK ASK";
    EXPECT_EQ(open_count(SYMBOL, Side::ASK), 0) << "After CANCEL_ACK ASK";
    EXPECT_FALSE(engine.pre_trade_check(create_order("X", SYMBOL, SYMBOL, Side::ASK, 151.0, 100)).would_breach)
        << "ASK capacity freed";
}

TEST_F(OrderCountByInstrumentSideTest, Clear) {
    engine.on_new_order_single(create_order("ORD001", "AAPL", "AAPL", Side::BID, 150.0, 100));
    engine.on_execution_report(create_ack("ORD001", 100));

    EXPECT_EQ(open_count("AAPL", Side::BID), 1);

    engine.clear();

    EXPECT_EQ(open_count("AAPL", Side::BID), 0);
    EXPECT_EQ(in_flight_count("AAPL", Side::BID), 0);
}

TEST_F(OrderCountByInstrumentSideTest, PreTradeCheckResultToString) {
    const std::string SYMBOL = "AAPL";

    // Send order to hit limit
    engine.on_new_order_single(create_order("ORD001", SYMBOL, SYMBOL, Side::BID, 150.0, 100));

    // Check pre-trade for new order
    auto result = engine.pre_trade_check(create_order("ORD002", SYMBOL, SYMBOL, Side::BID, 150.0, 100));
    EXPECT_TRUE(result.would_breach);
    EXPECT_EQ(result.breaches.size(), 1u);

    // Verify to_string() contains expected information
    std::string result_str = result.to_string();
    EXPECT_NE(result_str.find("ORDER_COUNT"), std::string::npos);
    EXPECT_NE(result_str.find("AAPL"), std::string::npos);
    EXPECT_NE(result_str.find("limit=1"), std::string::npos);

    // Verify breach info to_string()
    std::string breach_str = result.breaches[0].to_string();
    EXPECT_NE(breach_str.find("ORDER_COUNT"), std::string::npos);
    EXPECT_NE(breach_str.find("current=1"), std::string::npos);
    EXPECT_NE(breach_str.find("after_order=2"), std::string::npos);
}
