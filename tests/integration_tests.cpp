#include <gtest/gtest.h>
#include "../src/engine/risk_engine.hpp"
#include "../src/fix/fix_parser.hpp"

using namespace engine;
using namespace fix;

// ============================================================================
// Helper functions to create test messages
// ============================================================================

namespace {

NewOrderSingle create_new_order(const std::string& cl_ord_id, const std::string& symbol,
                                 const std::string& underlyer, Side side,
                                 double price, double qty, double delta = 1.0,
                                 const std::string& strategy = "STRAT1",
                                 const std::string& portfolio = "PORT1") {
    NewOrderSingle order;
    order.key.cl_ord_id = cl_ord_id;
    order.symbol = symbol;
    order.underlyer = underlyer;
    order.side = side;
    order.price = price;
    order.quantity = qty;
    order.delta = delta;
    order.strategy_id = strategy;
    order.portfolio_id = portfolio;
    return order;
}

ExecutionReport create_exec_report(const std::string& cl_ord_id, OrdStatus status,
                                    ExecType exec_type, double leaves_qty, double cum_qty,
                                    double last_qty = 0, double last_px = 0,
                                    const std::string& orig_cl_ord_id = "",
                                    bool is_unsolicited = false) {
    ExecutionReport report;
    report.key.cl_ord_id = cl_ord_id;
    report.order_id = "EX" + cl_ord_id;
    report.ord_status = status;
    report.exec_type = exec_type;
    report.leaves_qty = leaves_qty;
    report.cum_qty = cum_qty;
    report.last_qty = last_qty;
    report.last_px = last_px;
    report.is_unsolicited = is_unsolicited;
    if (!orig_cl_ord_id.empty()) {
        report.orig_key = OrderKey{orig_cl_ord_id};
    }
    return report;
}

OrderCancelReplaceRequest create_replace_request(const std::string& new_cl_ord_id,
                                                   const std::string& orig_cl_ord_id,
                                                   const std::string& symbol,
                                                   Side side, double price, double qty) {
    OrderCancelReplaceRequest req;
    req.key.cl_ord_id = new_cl_ord_id;
    req.orig_key.cl_ord_id = orig_cl_ord_id;
    req.symbol = symbol;
    req.side = side;
    req.price = price;
    req.quantity = qty;
    return req;
}

OrderCancelRequest create_cancel_request(const std::string& cancel_cl_ord_id,
                                          const std::string& orig_cl_ord_id,
                                          const std::string& symbol, Side side) {
    OrderCancelRequest req;
    req.key.cl_ord_id = cancel_cl_ord_id;
    req.orig_key.cl_ord_id = orig_cl_ord_id;
    req.symbol = symbol;
    req.side = side;
    return req;
}

OrderCancelReject create_cancel_reject(const std::string& cancel_cl_ord_id,
                                        const std::string& orig_cl_ord_id,
                                        CxlRejResponseTo response_to) {
    OrderCancelReject reject;
    reject.key.cl_ord_id = cancel_cl_ord_id;
    reject.orig_key.cl_ord_id = orig_cl_ord_id;
    reject.order_id = "EX" + orig_cl_ord_id;
    reject.ord_status = OrdStatus::NEW;
    reject.response_to = response_to;
    reject.cxl_rej_reason = 0;
    return reject;
}

}  // namespace

// ============================================================================
// Order Book Tests
// ============================================================================

class OrderBookTest : public ::testing::Test {
protected:
    OrderBook book;
};

TEST_F(OrderBookTest, AddAndGet) {
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);

    book.add_order(order);

    auto* tracked = book.get_order(OrderKey{"ORD001"});
    ASSERT_NE(tracked, nullptr);
    EXPECT_EQ(tracked->symbol, "AAPL230120C150");
    EXPECT_EQ(tracked->state, OrderState::PENDING_NEW);
}

TEST_F(OrderBookTest, Acknowledge) {
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);

    book.add_order(order);
    book.acknowledge_order(OrderKey{"ORD001"});

    auto* tracked = book.get_order(OrderKey{"ORD001"});
    EXPECT_EQ(tracked->state, OrderState::OPEN);
}

TEST_F(OrderBookTest, Reject) {
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);

    book.add_order(order);
    book.reject_order(OrderKey{"ORD001"});

    auto* tracked = book.get_order(OrderKey{"ORD001"});
    EXPECT_EQ(tracked->state, OrderState::REJECTED);
    EXPECT_TRUE(tracked->is_terminal());
}

TEST_F(OrderBookTest, CancelFlow) {
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);

    book.add_order(order);
    book.acknowledge_order(OrderKey{"ORD001"});
    book.start_cancel(OrderKey{"ORD001"}, OrderKey{"CXL001"});

    auto* tracked = book.get_order(OrderKey{"ORD001"});
    EXPECT_EQ(tracked->state, OrderState::PENDING_CANCEL);

    book.complete_cancel(OrderKey{"ORD001"});
    EXPECT_EQ(tracked->state, OrderState::CANCELED);
}

TEST_F(OrderBookTest, FillFlow) {
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);

    book.add_order(order);
    book.acknowledge_order(OrderKey{"ORD001"});

    // Partial fill
    auto result1 = book.apply_fill(OrderKey{"ORD001"}, 40, 5.0);
    ASSERT_TRUE(result1.has_value());
    EXPECT_DOUBLE_EQ(result1->filled_qty, 40.0);
    EXPECT_FALSE(result1->is_complete);

    auto* tracked = book.get_order(OrderKey{"ORD001"});
    EXPECT_DOUBLE_EQ(tracked->leaves_qty, 60.0);
    EXPECT_DOUBLE_EQ(tracked->cum_qty, 40.0);

    // Full fill
    auto result2 = book.apply_fill(OrderKey{"ORD001"}, 60, 5.0);
    EXPECT_TRUE(result2->is_complete);
    EXPECT_EQ(tracked->state, OrderState::FILLED);
}

// ============================================================================
// Risk Engine Integration Tests
// ============================================================================

class RiskEngineTest : public ::testing::Test {
protected:
    RiskAggregationEngine engine;
};

TEST_F(RiskEngineTest, NewOrderFlow) {
    // Send new order
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5);
    engine.on_new_order_single(order);

    // Verify metrics updated immediately on order send
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 50.0);  // 100 * 0.5
    EXPECT_DOUBLE_EQ(engine.global_net_delta(), 50.0);    // BID = positive
    EXPECT_EQ(engine.bid_order_count("AAPL230120C150"), 1);
    EXPECT_EQ(engine.quoted_instruments_count("AAPL"), 1);
    EXPECT_DOUBLE_EQ(engine.strategy_notional("STRAT1"), 500.0);  // 100 * 5.0

    // Receive ack
    auto ack = create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0);
    engine.on_execution_report(ack);

    // Metrics should remain unchanged on ack
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 50.0);
}

TEST_F(RiskEngineTest, OrderRejected) {
    // Send new order
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5);
    engine.on_new_order_single(order);

    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 50.0);

    // Receive nack
    auto nack = create_exec_report("ORD001", OrdStatus::REJECTED, ExecType::REJECTED, 0, 0);
    engine.on_execution_report(nack);

    // Metrics should be rolled back
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 0.0);
    EXPECT_EQ(engine.bid_order_count("AAPL230120C150"), 0);
    EXPECT_EQ(engine.quoted_instruments_count("AAPL"), 0);
}

TEST_F(RiskEngineTest, OrderCanceled) {
    // Send and ack order
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5);
    engine.on_new_order_single(order);
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));

    // Send cancel request
    auto cancel_req = create_cancel_request("CXL001", "ORD001", "AAPL230120C150", Side::BID);
    engine.on_order_cancel_request(cancel_req);

    // Metrics unchanged until cancel ack
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 50.0);

    // Receive cancel ack
    auto cancel_ack = create_exec_report("CXL001", OrdStatus::CANCELED, ExecType::CANCELED, 0, 0, 0, 0, "ORD001");
    engine.on_execution_report(cancel_ack);

    // Metrics should be removed
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 0.0);
    EXPECT_EQ(engine.bid_order_count("AAPL230120C150"), 0);
}

TEST_F(RiskEngineTest, CancelRejected) {
    // Send and ack order
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5);
    engine.on_new_order_single(order);
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));

    // Send cancel request
    auto cancel_req = create_cancel_request("CXL001", "ORD001", "AAPL230120C150", Side::BID);
    engine.on_order_cancel_request(cancel_req);

    // Receive cancel reject
    auto cancel_reject = create_cancel_reject("CXL001", "ORD001", CxlRejResponseTo::ORDER_CANCEL_REQUEST);
    engine.on_order_cancel_reject(cancel_reject);

    // Metrics should remain unchanged
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 50.0);
    EXPECT_EQ(engine.bid_order_count("AAPL230120C150"), 1);
}

TEST_F(RiskEngineTest, PartialFill) {
    // Send and ack order
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5);
    engine.on_new_order_single(order);
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));

    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 50.0);
    EXPECT_DOUBLE_EQ(engine.strategy_notional("STRAT1"), 500.0);

    // Receive partial fill (40 contracts)
    auto partial = create_exec_report("ORD001", OrdStatus::PARTIALLY_FILLED, ExecType::PARTIAL_FILL, 60, 40, 40, 5.0);
    engine.on_execution_report(partial);

    // Delta reduced by filled amount
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 30.0);  // 60 * 0.5
    EXPECT_DOUBLE_EQ(engine.strategy_notional("STRAT1"), 300.0);  // 60 * 5.0
    // Order count unchanged on partial fill
    EXPECT_EQ(engine.bid_order_count("AAPL230120C150"), 1);
}

TEST_F(RiskEngineTest, FullFill) {
    // Send and ack order
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5);
    engine.on_new_order_single(order);
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));

    // Receive full fill
    auto fill = create_exec_report("ORD001", OrdStatus::FILLED, ExecType::FILL, 0, 100, 100, 5.0);
    engine.on_execution_report(fill);

    // All metrics removed
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 0.0);
    EXPECT_EQ(engine.bid_order_count("AAPL230120C150"), 0);
    EXPECT_EQ(engine.quoted_instruments_count("AAPL"), 0);
}

TEST_F(RiskEngineTest, UnsolicitedCancel) {
    // Send and ack order
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5);
    engine.on_new_order_single(order);
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));

    // Receive unsolicited cancel
    auto unsolicited = create_exec_report("ORD001", OrdStatus::CANCELED, ExecType::CANCELED, 0, 0, 0, 0, "", true);
    engine.on_execution_report(unsolicited);

    // Metrics removed
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 0.0);
    EXPECT_EQ(engine.bid_order_count("AAPL230120C150"), 0);
}

TEST_F(RiskEngineTest, MultipleOrdersDifferentUnderlyers) {
    // AAPL orders
    engine.on_new_order_single(create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5));
    engine.on_new_order_single(create_new_order("ORD002", "AAPL230120P150", "AAPL", Side::ASK, 3.0, 50, 0.3));

    // MSFT orders
    engine.on_new_order_single(create_new_order("ORD003", "MSFT230120C300", "MSFT", Side::BID, 8.0, 200, 0.7));

    // Global metrics
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 50.0 + 15.0 + 140.0);  // All absolute
    EXPECT_DOUBLE_EQ(engine.global_net_delta(), 50.0 - 15.0 + 140.0);    // BID pos, ASK neg

    // Per-underlyer metrics
    EXPECT_DOUBLE_EQ(engine.underlyer_gross_delta("AAPL"), 65.0);
    EXPECT_DOUBLE_EQ(engine.underlyer_net_delta("AAPL"), 35.0);  // 50 - 15
    EXPECT_DOUBLE_EQ(engine.underlyer_gross_delta("MSFT"), 140.0);

    // Quoted instruments
    EXPECT_EQ(engine.quoted_instruments_count("AAPL"), 2);
    EXPECT_EQ(engine.quoted_instruments_count("MSFT"), 1);
}

TEST_F(RiskEngineTest, OrderReplace) {
    // Send and ack order
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5);
    engine.on_new_order_single(order);
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));

    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 50.0);
    EXPECT_DOUBLE_EQ(engine.strategy_notional("STRAT1"), 500.0);

    // Send replace request (increase quantity to 150)
    auto replace = create_replace_request("ORD002", "ORD001", "AAPL230120C150", Side::BID, 5.0, 150);
    engine.on_order_cancel_replace(replace);

    // Metrics unchanged until ack
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 50.0);

    // Receive replace ack
    auto replace_ack = create_exec_report("ORD002", OrdStatus::NEW, ExecType::REPLACED, 150, 0, 0, 0, "ORD001");
    engine.on_execution_report(replace_ack);

    // Metrics updated to new values
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 75.0);  // 150 * 0.5
    EXPECT_DOUBLE_EQ(engine.strategy_notional("STRAT1"), 750.0);  // 150 * 5.0
}

TEST_F(RiskEngineTest, BidAskOrderCounts) {
    // Multiple bid and ask orders for same instrument
    engine.on_new_order_single(create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100));
    engine.on_new_order_single(create_new_order("ORD002", "AAPL230120C150", "AAPL", Side::BID, 4.9, 50));
    engine.on_new_order_single(create_new_order("ORD003", "AAPL230120C150", "AAPL", Side::ASK, 5.1, 75));

    EXPECT_EQ(engine.bid_order_count("AAPL230120C150"), 2);
    EXPECT_EQ(engine.ask_order_count("AAPL230120C150"), 1);

    // Cancel one bid
    engine.on_order_cancel_request(create_cancel_request("CXL001", "ORD001", "AAPL230120C150", Side::BID));
    engine.on_execution_report(create_exec_report("CXL001", OrdStatus::CANCELED, ExecType::CANCELED, 0, 0, 0, 0, "ORD001"));

    EXPECT_EQ(engine.bid_order_count("AAPL230120C150"), 1);
    EXPECT_EQ(engine.ask_order_count("AAPL230120C150"), 1);
}

TEST_F(RiskEngineTest, MultipleStrategies) {
    engine.on_new_order_single(create_new_order("ORD001", "AAPL", "AAPL", Side::BID, 150.0, 100, 1.0, "MOMENTUM", "PORT1"));
    engine.on_new_order_single(create_new_order("ORD002", "MSFT", "MSFT", Side::BID, 300.0, 50, 1.0, "MOMENTUM", "PORT1"));
    engine.on_new_order_single(create_new_order("ORD003", "GOOG", "GOOG", Side::BID, 100.0, 200, 1.0, "REVERSION", "PORT2"));

    EXPECT_DOUBLE_EQ(engine.strategy_notional("MOMENTUM"), 15000.0 + 15000.0);  // 100*150 + 50*300
    EXPECT_DOUBLE_EQ(engine.strategy_notional("REVERSION"), 20000.0);  // 200*100
    EXPECT_DOUBLE_EQ(engine.portfolio_notional("PORT1"), 30000.0);
    EXPECT_DOUBLE_EQ(engine.portfolio_notional("PORT2"), 20000.0);
    EXPECT_DOUBLE_EQ(engine.global_notional(), 50000.0);
}

TEST_F(RiskEngineTest, Clear) {
    engine.on_new_order_single(create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5));
    engine.on_new_order_single(create_new_order("ORD002", "MSFT230120C300", "MSFT", Side::ASK, 8.0, 200, 0.7));

    engine.clear();

    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 0.0);
    EXPECT_DOUBLE_EQ(engine.global_net_delta(), 0.0);
    EXPECT_DOUBLE_EQ(engine.global_notional(), 0.0);
    EXPECT_EQ(engine.active_order_count(), 0u);
}

TEST_F(RiskEngineTest, ComplexScenario) {
    // Initial orders
    engine.on_new_order_single(create_new_order("ORD001", "OPT1", "AAPL", Side::BID, 5.0, 100, 0.5, "STRAT1", "PORT1"));
    engine.on_new_order_single(create_new_order("ORD002", "OPT2", "AAPL", Side::ASK, 3.0, 200, 0.3, "STRAT1", "PORT1"));
    engine.on_new_order_single(create_new_order("ORD003", "OPT3", "MSFT", Side::BID, 10.0, 50, 0.8, "STRAT2", "PORT2"));

    // Ack all
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));
    engine.on_execution_report(create_exec_report("ORD002", OrdStatus::NEW, ExecType::NEW, 200, 0));
    engine.on_execution_report(create_exec_report("ORD003", OrdStatus::NEW, ExecType::NEW, 50, 0));

    // Initial state check
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 50.0 + 60.0 + 40.0);  // 150
    EXPECT_DOUBLE_EQ(engine.global_net_delta(), 50.0 - 60.0 + 40.0);    // 30 (BID-ASK+BID)

    // Partial fill on ORD001
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::PARTIALLY_FILLED, ExecType::PARTIAL_FILL, 60, 40, 40, 5.0));
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 30.0 + 60.0 + 40.0);  // 130

    // Cancel ORD002
    engine.on_order_cancel_request(create_cancel_request("CXL001", "ORD002", "OPT2", Side::ASK));
    engine.on_execution_report(create_exec_report("CXL001", OrdStatus::CANCELED, ExecType::CANCELED, 0, 0, 0, 0, "ORD002"));
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 30.0 + 40.0);  // 70
    EXPECT_DOUBLE_EQ(engine.global_net_delta(), 30.0 + 40.0);    // 70 (both BID now)

    // Complete fill on ORD001
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::FILLED, ExecType::FILL, 0, 100, 60, 5.0));
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 40.0);

    // Only ORD003 remains
    EXPECT_EQ(engine.active_order_count(), 1u);
    EXPECT_EQ(engine.quoted_instruments_count("AAPL"), 0);
    EXPECT_EQ(engine.quoted_instruments_count("MSFT"), 1);
}

// ============================================================================
// Parameterized Tests for Order State Transitions
// ============================================================================

struct OrderStateTestParam {
    std::string name;
    OrdStatus status;
    ExecType exec_type;
    bool expect_metrics_cleared;
};

class OrderStateTransitionTest : public ::testing::TestWithParam<OrderStateTestParam> {
protected:
    RiskAggregationEngine engine;
};

TEST_P(OrderStateTransitionTest, MetricsUpdatedCorrectly) {
    auto param = GetParam();

    // Send and ack order
    auto order = create_new_order("ORD001", "AAPL", "AAPL", Side::BID, 5.0, 100, 0.5);
    engine.on_new_order_single(order);
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));

    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 50.0);

    // Apply state transition
    auto report = create_exec_report("ORD001", param.status, param.exec_type,
                                      param.status == OrdStatus::PARTIALLY_FILLED ? 50 : 0,
                                      param.status == OrdStatus::PARTIALLY_FILLED ? 50 : 100,
                                      param.status == OrdStatus::PARTIALLY_FILLED ? 50 : 100, 5.0);
    engine.on_execution_report(report);

    if (param.expect_metrics_cleared) {
        EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 0.0);
    } else {
        EXPECT_GT(engine.global_gross_delta(), 0.0);
    }
}

INSTANTIATE_TEST_SUITE_P(
    StateTransitions,
    OrderStateTransitionTest,
    ::testing::Values(
        OrderStateTestParam{"PartialFill", OrdStatus::PARTIALLY_FILLED, ExecType::PARTIAL_FILL, false},
        OrderStateTestParam{"FullFill", OrdStatus::FILLED, ExecType::FILL, true},
        OrderStateTestParam{"Canceled", OrdStatus::CANCELED, ExecType::CANCELED, true}
    ),
    [](const ::testing::TestParamInfo<OrderStateTestParam>& info) {
        return info.param.name;
    }
);
