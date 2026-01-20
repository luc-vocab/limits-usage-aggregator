#include "test_framework.hpp"
#include "../src/engine/risk_engine.hpp"
#include "../src/fix/fix_parser.hpp"

using namespace test;
using namespace engine;
using namespace fix;

// ============================================================================
// Helper functions to create test messages
// ============================================================================

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

// ============================================================================
// Order State Tests
// ============================================================================

void test_order_book_add_and_get() {
    OrderBook book;
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);

    book.add_order(order);

    auto* tracked = book.get_order(OrderKey{"ORD001"});
    assert_not_null(tracked);
    assert_equal(tracked->symbol, std::string("AAPL230120C150"));
    assert_equal(tracked->state, OrderState::PENDING_NEW);
}

void test_order_book_acknowledge() {
    OrderBook book;
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);

    book.add_order(order);
    book.acknowledge_order(OrderKey{"ORD001"});

    auto* tracked = book.get_order(OrderKey{"ORD001"});
    assert_equal(tracked->state, OrderState::OPEN);
}

void test_order_book_reject() {
    OrderBook book;
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);

    book.add_order(order);
    book.reject_order(OrderKey{"ORD001"});

    auto* tracked = book.get_order(OrderKey{"ORD001"});
    assert_equal(tracked->state, OrderState::REJECTED);
    assert_true(tracked->is_terminal());
}

void test_order_book_cancel_flow() {
    OrderBook book;
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);

    book.add_order(order);
    book.acknowledge_order(OrderKey{"ORD001"});
    book.start_cancel(OrderKey{"ORD001"}, OrderKey{"CXL001"});

    auto* tracked = book.get_order(OrderKey{"ORD001"});
    assert_equal(tracked->state, OrderState::PENDING_CANCEL);

    book.complete_cancel(OrderKey{"ORD001"});
    assert_equal(tracked->state, OrderState::CANCELED);
}

void test_order_book_fill_flow() {
    OrderBook book;
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100);

    book.add_order(order);
    book.acknowledge_order(OrderKey{"ORD001"});

    // Partial fill
    auto result1 = book.apply_fill(OrderKey{"ORD001"}, 40, 5.0);
    assert_true(result1.has_value());
    assert_double_equal(result1->filled_qty, 40.0);
    assert_false(result1->is_complete);

    auto* tracked = book.get_order(OrderKey{"ORD001"});
    assert_double_equal(tracked->leaves_qty, 60.0);
    assert_double_equal(tracked->cum_qty, 40.0);

    // Full fill
    auto result2 = book.apply_fill(OrderKey{"ORD001"}, 60, 5.0);
    assert_true(result2->is_complete);
    assert_equal(tracked->state, OrderState::FILLED);
}

// ============================================================================
// Risk Engine Integration Tests
// ============================================================================

void test_engine_new_order_flow() {
    RiskAggregationEngine engine;

    // Send new order
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5);
    engine.on_new_order_single(order);

    // Verify metrics updated immediately on order send
    assert_double_equal(engine.global_gross_delta(), 50.0);  // 100 * 0.5
    assert_double_equal(engine.global_net_delta(), 50.0);    // BID = positive
    assert_equal(engine.bid_order_count("AAPL230120C150"), int64_t(1));
    assert_equal(engine.quoted_instruments_count("AAPL"), int64_t(1));
    assert_double_equal(engine.strategy_notional("STRAT1"), 500.0);  // 100 * 5.0

    // Receive ack
    auto ack = create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0);
    engine.on_execution_report(ack);

    // Metrics should remain unchanged on ack
    assert_double_equal(engine.global_gross_delta(), 50.0);
}

void test_engine_order_rejected() {
    RiskAggregationEngine engine;

    // Send new order
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5);
    engine.on_new_order_single(order);

    assert_double_equal(engine.global_gross_delta(), 50.0);

    // Receive nack
    auto nack = create_exec_report("ORD001", OrdStatus::REJECTED, ExecType::REJECTED, 0, 0);
    engine.on_execution_report(nack);

    // Metrics should be rolled back
    assert_double_equal(engine.global_gross_delta(), 0.0);
    assert_equal(engine.bid_order_count("AAPL230120C150"), int64_t(0));
    assert_equal(engine.quoted_instruments_count("AAPL"), int64_t(0));
}

void test_engine_order_canceled() {
    RiskAggregationEngine engine;

    // Send and ack order
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5);
    engine.on_new_order_single(order);
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));

    // Send cancel request
    auto cancel_req = create_cancel_request("CXL001", "ORD001", "AAPL230120C150", Side::BID);
    engine.on_order_cancel_request(cancel_req);

    // Metrics unchanged until cancel ack
    assert_double_equal(engine.global_gross_delta(), 50.0);

    // Receive cancel ack
    auto cancel_ack = create_exec_report("CXL001", OrdStatus::CANCELED, ExecType::CANCELED, 0, 0, 0, 0, "ORD001");
    engine.on_execution_report(cancel_ack);

    // Metrics should be removed
    assert_double_equal(engine.global_gross_delta(), 0.0);
    assert_equal(engine.bid_order_count("AAPL230120C150"), int64_t(0));
}

void test_engine_cancel_rejected() {
    RiskAggregationEngine engine;

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
    assert_double_equal(engine.global_gross_delta(), 50.0);
    assert_equal(engine.bid_order_count("AAPL230120C150"), int64_t(1));
}

void test_engine_partial_fill() {
    RiskAggregationEngine engine;

    // Send and ack order
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5);
    engine.on_new_order_single(order);
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));

    assert_double_equal(engine.global_gross_delta(), 50.0);
    assert_double_equal(engine.strategy_notional("STRAT1"), 500.0);

    // Receive partial fill (40 contracts)
    auto partial = create_exec_report("ORD001", OrdStatus::PARTIALLY_FILLED, ExecType::PARTIAL_FILL, 60, 40, 40, 5.0);
    engine.on_execution_report(partial);

    // Delta reduced by filled amount
    assert_double_equal(engine.global_gross_delta(), 30.0);  // 60 * 0.5
    assert_double_equal(engine.strategy_notional("STRAT1"), 300.0);  // 60 * 5.0
    // Order count unchanged on partial fill
    assert_equal(engine.bid_order_count("AAPL230120C150"), int64_t(1));
}

void test_engine_full_fill() {
    RiskAggregationEngine engine;

    // Send and ack order
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5);
    engine.on_new_order_single(order);
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));

    // Receive full fill
    auto fill = create_exec_report("ORD001", OrdStatus::FILLED, ExecType::FILL, 0, 100, 100, 5.0);
    engine.on_execution_report(fill);

    // All metrics removed
    assert_double_equal(engine.global_gross_delta(), 0.0);
    assert_equal(engine.bid_order_count("AAPL230120C150"), int64_t(0));
    assert_equal(engine.quoted_instruments_count("AAPL"), int64_t(0));
}

void test_engine_unsolicited_cancel() {
    RiskAggregationEngine engine;

    // Send and ack order
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5);
    engine.on_new_order_single(order);
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));

    // Receive unsolicited cancel
    auto unsolicited = create_exec_report("ORD001", OrdStatus::CANCELED, ExecType::CANCELED, 0, 0, 0, 0, "", true);
    engine.on_execution_report(unsolicited);

    // Metrics removed
    assert_double_equal(engine.global_gross_delta(), 0.0);
    assert_equal(engine.bid_order_count("AAPL230120C150"), int64_t(0));
}

void test_engine_multiple_orders_different_underlyers() {
    RiskAggregationEngine engine;

    // AAPL orders
    engine.on_new_order_single(create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5));
    engine.on_new_order_single(create_new_order("ORD002", "AAPL230120P150", "AAPL", Side::ASK, 3.0, 50, 0.3));

    // MSFT orders
    engine.on_new_order_single(create_new_order("ORD003", "MSFT230120C300", "MSFT", Side::BID, 8.0, 200, 0.7));

    // Global metrics
    assert_double_equal(engine.global_gross_delta(), 50.0 + 15.0 + 140.0);  // All absolute
    assert_double_equal(engine.global_net_delta(), 50.0 - 15.0 + 140.0);    // BID pos, ASK neg

    // Per-underlyer metrics
    assert_double_equal(engine.underlyer_gross_delta("AAPL"), 65.0);
    assert_double_equal(engine.underlyer_net_delta("AAPL"), 35.0);  // 50 - 15
    assert_double_equal(engine.underlyer_gross_delta("MSFT"), 140.0);

    // Quoted instruments
    assert_equal(engine.quoted_instruments_count("AAPL"), int64_t(2));
    assert_equal(engine.quoted_instruments_count("MSFT"), int64_t(1));
}

void test_engine_order_replace() {
    RiskAggregationEngine engine;

    // Send and ack order
    auto order = create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5);
    engine.on_new_order_single(order);
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));

    assert_double_equal(engine.global_gross_delta(), 50.0);
    assert_double_equal(engine.strategy_notional("STRAT1"), 500.0);

    // Send replace request (increase quantity to 150)
    auto replace = create_replace_request("ORD002", "ORD001", "AAPL230120C150", Side::BID, 5.0, 150);
    engine.on_order_cancel_replace(replace);

    // Metrics unchanged until ack
    assert_double_equal(engine.global_gross_delta(), 50.0);

    // Receive replace ack
    auto replace_ack = create_exec_report("ORD002", OrdStatus::NEW, ExecType::REPLACED, 150, 0, 0, 0, "ORD001");
    engine.on_execution_report(replace_ack);

    // Metrics updated to new values
    assert_double_equal(engine.global_gross_delta(), 75.0);  // 150 * 0.5
    assert_double_equal(engine.strategy_notional("STRAT1"), 750.0);  // 150 * 5.0
}

void test_engine_bid_ask_order_counts() {
    RiskAggregationEngine engine;

    // Multiple bid and ask orders for same instrument
    engine.on_new_order_single(create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100));
    engine.on_new_order_single(create_new_order("ORD002", "AAPL230120C150", "AAPL", Side::BID, 4.9, 50));
    engine.on_new_order_single(create_new_order("ORD003", "AAPL230120C150", "AAPL", Side::ASK, 5.1, 75));

    assert_equal(engine.bid_order_count("AAPL230120C150"), int64_t(2));
    assert_equal(engine.ask_order_count("AAPL230120C150"), int64_t(1));

    // Cancel one bid
    engine.on_order_cancel_request(create_cancel_request("CXL001", "ORD001", "AAPL230120C150", Side::BID));
    engine.on_execution_report(create_exec_report("CXL001", OrdStatus::CANCELED, ExecType::CANCELED, 0, 0, 0, 0, "ORD001"));

    assert_equal(engine.bid_order_count("AAPL230120C150"), int64_t(1));
    assert_equal(engine.ask_order_count("AAPL230120C150"), int64_t(1));
}

void test_engine_multiple_strategies() {
    RiskAggregationEngine engine;

    engine.on_new_order_single(create_new_order("ORD001", "AAPL", "AAPL", Side::BID, 150.0, 100, 1.0, "MOMENTUM", "PORT1"));
    engine.on_new_order_single(create_new_order("ORD002", "MSFT", "MSFT", Side::BID, 300.0, 50, 1.0, "MOMENTUM", "PORT1"));
    engine.on_new_order_single(create_new_order("ORD003", "GOOG", "GOOG", Side::BID, 100.0, 200, 1.0, "REVERSION", "PORT2"));

    assert_double_equal(engine.strategy_notional("MOMENTUM"), 15000.0 + 15000.0);  // 100*150 + 50*300
    assert_double_equal(engine.strategy_notional("REVERSION"), 20000.0);  // 200*100
    assert_double_equal(engine.portfolio_notional("PORT1"), 30000.0);
    assert_double_equal(engine.portfolio_notional("PORT2"), 20000.0);
    assert_double_equal(engine.global_notional(), 50000.0);
}

void test_engine_clear() {
    RiskAggregationEngine engine;

    engine.on_new_order_single(create_new_order("ORD001", "AAPL230120C150", "AAPL", Side::BID, 5.0, 100, 0.5));
    engine.on_new_order_single(create_new_order("ORD002", "MSFT230120C300", "MSFT", Side::ASK, 8.0, 200, 0.7));

    engine.clear();

    assert_double_equal(engine.global_gross_delta(), 0.0);
    assert_double_equal(engine.global_net_delta(), 0.0);
    assert_double_equal(engine.global_notional(), 0.0);
    assert_equal(engine.active_order_count(), size_t(0));
}

void test_engine_complex_scenario() {
    RiskAggregationEngine engine;

    // Initial orders
    engine.on_new_order_single(create_new_order("ORD001", "OPT1", "AAPL", Side::BID, 5.0, 100, 0.5, "STRAT1", "PORT1"));
    engine.on_new_order_single(create_new_order("ORD002", "OPT2", "AAPL", Side::ASK, 3.0, 200, 0.3, "STRAT1", "PORT1"));
    engine.on_new_order_single(create_new_order("ORD003", "OPT3", "MSFT", Side::BID, 10.0, 50, 0.8, "STRAT2", "PORT2"));

    // Ack all
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::NEW, ExecType::NEW, 100, 0));
    engine.on_execution_report(create_exec_report("ORD002", OrdStatus::NEW, ExecType::NEW, 200, 0));
    engine.on_execution_report(create_exec_report("ORD003", OrdStatus::NEW, ExecType::NEW, 50, 0));

    // Initial state check
    assert_double_equal(engine.global_gross_delta(), 50.0 + 60.0 + 40.0);  // 150
    assert_double_equal(engine.global_net_delta(), 50.0 - 60.0 + 40.0);    // 30 (BID-ASK+BID)

    // Partial fill on ORD001
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::PARTIALLY_FILLED, ExecType::PARTIAL_FILL, 60, 40, 40, 5.0));
    assert_double_equal(engine.global_gross_delta(), 30.0 + 60.0 + 40.0);  // 130

    // Cancel ORD002
    engine.on_order_cancel_request(create_cancel_request("CXL001", "ORD002", "OPT2", Side::ASK));
    engine.on_execution_report(create_exec_report("CXL001", OrdStatus::CANCELED, ExecType::CANCELED, 0, 0, 0, 0, "ORD002"));
    assert_double_equal(engine.global_gross_delta(), 30.0 + 40.0);  // 70
    assert_double_equal(engine.global_net_delta(), 30.0 + 40.0);    // 70 (both BID now)

    // Complete fill on ORD001
    engine.on_execution_report(create_exec_report("ORD001", OrdStatus::FILLED, ExecType::FILL, 0, 100, 60, 5.0));
    assert_double_equal(engine.global_gross_delta(), 40.0);

    // Only ORD003 remains
    assert_equal(engine.active_order_count(), size_t(1));
    assert_equal(engine.quoted_instruments_count("AAPL"), int64_t(0));
    assert_equal(engine.quoted_instruments_count("MSFT"), int64_t(1));
}

// ============================================================================
// Run all integration tests
// ============================================================================

TestSuite run_integration_tests() {
    TestSuite suite("Integration Tests");

    // Order book tests
    suite.run_test("OrderBook - add and get", test_order_book_add_and_get);
    suite.run_test("OrderBook - acknowledge", test_order_book_acknowledge);
    suite.run_test("OrderBook - reject", test_order_book_reject);
    suite.run_test("OrderBook - cancel flow", test_order_book_cancel_flow);
    suite.run_test("OrderBook - fill flow", test_order_book_fill_flow);

    // Risk engine tests
    suite.run_test("Engine - new order flow", test_engine_new_order_flow);
    suite.run_test("Engine - order rejected", test_engine_order_rejected);
    suite.run_test("Engine - order canceled", test_engine_order_canceled);
    suite.run_test("Engine - cancel rejected", test_engine_cancel_rejected);
    suite.run_test("Engine - partial fill", test_engine_partial_fill);
    suite.run_test("Engine - full fill", test_engine_full_fill);
    suite.run_test("Engine - unsolicited cancel", test_engine_unsolicited_cancel);
    suite.run_test("Engine - multiple underlyers", test_engine_multiple_orders_different_underlyers);
    suite.run_test("Engine - order replace", test_engine_order_replace);
    suite.run_test("Engine - bid/ask order counts", test_engine_bid_ask_order_counts);
    suite.run_test("Engine - multiple strategies", test_engine_multiple_strategies);
    suite.run_test("Engine - clear", test_engine_clear);
    suite.run_test("Engine - complex scenario", test_engine_complex_scenario);

    return suite;
}
