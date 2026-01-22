#include <gtest/gtest.h>
#include "../src/engine/risk_engine.hpp"
#include "../src/instrument/instrument.hpp"
#include "../src/fix/fix_parser.hpp"

using namespace engine;
using namespace fix;
using namespace instrument;

// ============================================================================
// Helper functions to create test messages
// ============================================================================

namespace {

NewOrderSingle create_new_order(const std::string& cl_ord_id, const std::string& symbol,
                                 const std::string& underlyer, Side side,
                                 double price, int64_t qty,
                                 const std::string& strategy = "STRAT1",
                                 const std::string& portfolio = "PORT1") {
    NewOrderSingle order;
    order.key.cl_ord_id = cl_ord_id;
    order.symbol = symbol;
    order.underlyer = underlyer;
    order.side = side;
    order.price = price;
    order.quantity = qty;
    order.strategy_id = strategy;
    order.portfolio_id = portfolio;
    return order;
}

ExecutionReport create_exec_report(const std::string& cl_ord_id, OrdStatus status,
                                    ExecType exec_type, int64_t leaves_qty, int64_t cum_qty,
                                    int64_t last_qty = 0, double last_px = 0,
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
                                                   Side side, double price, int64_t qty) {
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

// Create a standard test instrument provider
StaticInstrumentProvider create_test_provider() {
    StaticInstrumentProvider provider;

    // AAPL options - delta 0.5, contract size 100, underlyer spot 150
    provider.add_option("AAPL230120C150", "AAPL", 5.0, 150.0, 0.5, 100.0, 1.0);
    provider.add_option("AAPL230120P150", "AAPL", 3.0, 150.0, 0.3, 100.0, 1.0);
    provider.add_option("OPT1", "AAPL", 5.0, 150.0, 0.5, 100.0, 1.0);
    provider.add_option("OPT2", "AAPL", 3.0, 150.0, 0.3, 100.0, 1.0);

    // MSFT options - delta 0.7, contract size 100, underlyer spot 300
    provider.add_option("MSFT230120C300", "MSFT", 8.0, 300.0, 0.7, 100.0, 1.0);
    provider.add_option("OPT3", "MSFT", 10.0, 300.0, 0.8, 100.0, 1.0);

    // GOOG - equity-like (delta=1, contract size=1)
    provider.add_equity("GOOG", 100.0);

    // Simple equities
    provider.add_equity("AAPL", 150.0);
    provider.add_equity("MSFT", 300.0);

    return provider;
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
    EXPECT_EQ(result1->filled_qty, 40);
    EXPECT_FALSE(result1->is_complete);

    auto* tracked = book.get_order(OrderKey{"ORD001"});
    EXPECT_EQ(tracked->leaves_qty, 60);
    EXPECT_EQ(tracked->cum_qty, 40);

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
    StaticInstrumentProvider provider;
    RiskAggregationEngine engine;

    void SetUp() override {
        provider = create_test_provider();
        engine.set_instrument_provider(&provider);
    }
};

TEST_F(RiskEngineTest, NewOrderFlow) {
    // Send new order: 100 contracts of AAPL call option
    // Delta = 100 * 0.5 * 100 (contract size) * 150 (underlyer spot) * 1.0 (fx) = 750,000
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);
    engine.on_new_order_single(order);

    // Verify metrics updated immediately on order send
    // Delta: qty(100) * delta(0.5) * contract_size(100) * underlyer_spot(150) * fx(1.0) = 750000
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 750000.0);
    EXPECT_DOUBLE_EQ(engine.global_net_delta(), 750000.0);  // BID = positive
    EXPECT_EQ(engine.bid_order_count("AAPL230120C150"), 1);
    EXPECT_EQ(engine.quoted_instruments_count("AAPL"), 1);

    // Notional: qty(100) * contract_size(100) * spot_price(5.0) * fx(1.0) = 50000
    EXPECT_DOUBLE_EQ(engine.strategy_notional("STRAT1"), 50000.0);

    // Receive ack
    auto ack = create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0);
    engine.on_execution_report(ack);

    // Metrics should remain unchanged on ack
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 750000.0);
}

TEST_F(RiskEngineTest, OrderRejected) {
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);
    engine.on_new_order_single(order);

    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 750000.0);

    // Receive nack
    auto nack = create_exec_report("ORD001", OrdStatus::REJECTED, ExecType::REJECTED, 0, 0);
    engine.on_execution_report(nack);

    // Metrics should be rolled back
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 0.0);
    EXPECT_EQ(engine.bid_order_count("AAPL230120C150"), 0);
    EXPECT_EQ(engine.quoted_instruments_count("AAPL"), 0);
}

TEST_F(RiskEngineTest, OrderCanceled) {
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);
    engine.on_new_order_single(order);
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));

    // Send cancel request
    auto cancel_req = create_cancel_request("CXL001", "ORD001", "AAPL230120C150", Side::BID);
    engine.on_order_cancel_request(cancel_req);

    // Metrics unchanged until cancel ack
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 750000.0);

    // Receive cancel ack
    auto cancel_ack = create_exec_report("CXL001", OrdStatus::CANCELED, ExecType::CANCELED, 0, 0, 0, 0, "ORD001");
    engine.on_execution_report(cancel_ack);

    // Metrics should be removed
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 0.0);
    EXPECT_EQ(engine.bid_order_count("AAPL230120C150"), 0);
}

TEST_F(RiskEngineTest, CancelRejected) {
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);
    engine.on_new_order_single(order);
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));

    // Send cancel request
    auto cancel_req = create_cancel_request("CXL001", "ORD001", "AAPL230120C150", Side::BID);
    engine.on_order_cancel_request(cancel_req);

    // Receive cancel reject
    auto cancel_reject = create_cancel_reject("CXL001", "ORD001", CxlRejResponseTo::ORDER_CANCEL_REQUEST);
    engine.on_order_cancel_reject(cancel_reject);

    // Metrics should remain unchanged
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 750000.0);
    EXPECT_EQ(engine.bid_order_count("AAPL230120C150"), 1);
}

TEST_F(RiskEngineTest, PartialFill) {
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);
    engine.on_new_order_single(order);
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));

    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 750000.0);
    EXPECT_DOUBLE_EQ(engine.strategy_notional("STRAT1"), 50000.0);

    // Receive partial fill (40 contracts)
    auto partial = create_exec_report("ORD001", OrdStatus::PARTIALLY_FILLED, ExecType::PARTIAL_FILL, 60, 40, 40, 5.0);
    engine.on_execution_report(partial);

    // Delta reduced: 60 * 0.5 * 100 * 150 = 450000
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 450000.0);
    // Notional reduced: 60 * 100 * 5.0 = 30000
    EXPECT_DOUBLE_EQ(engine.strategy_notional("STRAT1"), 30000.0);
    // Order count unchanged on partial fill
    EXPECT_EQ(engine.bid_order_count("AAPL230120C150"), 1);
}

TEST_F(RiskEngineTest, FullFill) {
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);
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
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);
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
    engine.on_new_order_single(create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100));
    engine.on_new_order_single(create_new_order("ORD002", "AAPL230120P150", "AAPL", Side::ASK, 3.0, 50));

    // MSFT orders
    engine.on_new_order_single(create_new_order("ORD003", "MSFT230120C300", "MSFT", Side::BID, 8.0, 200));

    // ORD001: 100 * 0.5 * 100 * 150 = 750,000
    // ORD002: 50 * 0.3 * 100 * 150 = 225,000
    // ORD003: 200 * 0.7 * 100 * 300 = 4,200,000
    double aapl_bid = 750000.0;
    double aapl_ask = 225000.0;
    double msft_bid = 4200000.0;

    // Global metrics
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), aapl_bid + aapl_ask + msft_bid);
    EXPECT_DOUBLE_EQ(engine.global_net_delta(), aapl_bid - aapl_ask + msft_bid);

    // Per-underlyer metrics
    EXPECT_DOUBLE_EQ(engine.underlyer_gross_delta("AAPL"), aapl_bid + aapl_ask);
    EXPECT_DOUBLE_EQ(engine.underlyer_net_delta("AAPL"), aapl_bid - aapl_ask);
    EXPECT_DOUBLE_EQ(engine.underlyer_gross_delta("MSFT"), msft_bid);

    // Quoted instruments
    EXPECT_EQ(engine.quoted_instruments_count("AAPL"), 2);
    EXPECT_EQ(engine.quoted_instruments_count("MSFT"), 1);
}

TEST_F(RiskEngineTest, OrderReplace) {
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);
    engine.on_new_order_single(order);
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));

    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 750000.0);
    EXPECT_DOUBLE_EQ(engine.strategy_notional("STRAT1"), 50000.0);

    // Send replace request (increase quantity to 150)
    auto replace = create_replace_request("ORD002", "ORD001", "AAPL230120C150", Side::BID, 5.0, 150);
    engine.on_order_cancel_replace(replace);

    // Metrics unchanged until ack
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 750000.0);

    // Receive replace ack
    auto replace_ack = create_exec_report("ORD002", OrdStatus::NEW, ExecType::REPLACED, 150, 0, 0, 0, "ORD001");
    engine.on_execution_report(replace_ack);

    // Delta: 150 * 0.5 * 100 * 150 = 1,125,000
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 1125000.0);
    // Notional: 150 * 100 * 5.0 = 75,000
    EXPECT_DOUBLE_EQ(engine.strategy_notional("STRAT1"), 75000.0);
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
    engine.on_new_order_single(create_new_order("ORD001", "AAPL", "AAPL", Side::BID, 150.0, 100, "MOMENTUM", "PORT1"));
    engine.on_new_order_single(create_new_order("ORD002", "MSFT", "MSFT", Side::BID, 300.0, 50, "MOMENTUM", "PORT1"));
    engine.on_new_order_single(create_new_order("ORD003", "GOOG", "GOOG", Side::BID, 100.0, 200, "REVERSION", "PORT2"));

    // AAPL: 100 * 1 * 150 * 1 = 15000
    // MSFT: 50 * 1 * 300 * 1 = 15000
    // GOOG: 200 * 1 * 100 * 1 = 20000
    EXPECT_DOUBLE_EQ(engine.strategy_notional("MOMENTUM"), 30000.0);
    EXPECT_DOUBLE_EQ(engine.strategy_notional("REVERSION"), 20000.0);
    EXPECT_DOUBLE_EQ(engine.portfolio_notional("PORT1"), 30000.0);
    EXPECT_DOUBLE_EQ(engine.portfolio_notional("PORT2"), 20000.0);
    EXPECT_DOUBLE_EQ(engine.global_notional(), 50000.0);
}

TEST_F(RiskEngineTest, Clear) {
    engine.on_new_order_single(create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100));
    engine.on_new_order_single(create_new_order("ORD002", "MSFT230120C300", "MSFT", Side::ASK, 8.0, 200));

    engine.clear();

    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 0.0);
    EXPECT_DOUBLE_EQ(engine.global_net_delta(), 0.0);
    EXPECT_DOUBLE_EQ(engine.global_notional(), 0.0);
    EXPECT_EQ(engine.active_order_count(), 0u);
}

TEST_F(RiskEngineTest, ComplexScenario) {
    // Initial orders
    engine.on_new_order_single(create_new_order("ORD001", "OPT1", "AAPL", Side::BID, 5.0, 100, "STRAT1", "PORT1"));
    engine.on_new_order_single(create_new_order("ORD002", "OPT2", "AAPL", Side::ASK, 3.0, 200, "STRAT1", "PORT1"));
    engine.on_new_order_single(create_new_order("ORD003", "OPT3", "MSFT", Side::BID, 10.0, 50, "STRAT2", "PORT2"));

    // Ack all
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));
    engine.on_execution_report(create_exec_report("ORD002", OrdStatus::NEW, ExecType::NEW, 200, 0));
    engine.on_execution_report(create_exec_report("ORD003", OrdStatus::NEW, ExecType::NEW, 50, 0));

    // OPT1: 100 * 0.5 * 100 * 150 = 750,000 (BID)
    // OPT2: 200 * 0.3 * 100 * 150 = 900,000 (ASK)
    // OPT3: 50 * 0.8 * 100 * 300 = 1,200,000 (BID)
    double opt1_delta = 750000.0;
    double opt2_delta = 900000.0;
    double opt3_delta = 1200000.0;

    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), opt1_delta + opt2_delta + opt3_delta);
    EXPECT_DOUBLE_EQ(engine.global_net_delta(), opt1_delta - opt2_delta + opt3_delta);

    // Partial fill on ORD001 (40 contracts filled, 60 remain)
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::PARTIALLY_FILLED, ExecType::PARTIAL_FILL, 60, 40, 40, 5.0));
    double opt1_after_fill = 60 * 0.5 * 100 * 150;  // 450,000
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), opt1_after_fill + opt2_delta + opt3_delta);

    // Cancel ORD002
    engine.on_order_cancel_request(create_cancel_request("CXL001", "ORD002", "OPT2", Side::ASK));
    engine.on_execution_report(create_exec_report("CXL001", OrdStatus::CANCELED, ExecType::CANCELED, 0, 0, 0, 0, "ORD002"));
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), opt1_after_fill + opt3_delta);
    EXPECT_DOUBLE_EQ(engine.global_net_delta(), opt1_after_fill + opt3_delta);  // Both BID now

    // Complete fill on ORD001
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::FILLED, ExecType::FILL, 0, 100, 60, 5.0));
    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), opt3_delta);

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
    StaticInstrumentProvider provider;
    RiskAggregationEngine engine;

    void SetUp() override {
        provider = create_test_provider();
        engine.set_instrument_provider(&provider);
    }
};

TEST_P(OrderStateTransitionTest, MetricsUpdatedCorrectly) {
    auto param = GetParam();

    // Send and ack order
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);
    engine.on_new_order_single(order);
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));

    EXPECT_DOUBLE_EQ(engine.global_gross_delta(), 750000.0);

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
