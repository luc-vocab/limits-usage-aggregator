#include "test_framework.hpp"
#include "../src/fix/fix_types.hpp"
#include "../src/fix/fix_messages.hpp"
#include "../src/fix/fix_parser.hpp"

using namespace test;
using namespace fix;

// ============================================================================
// FIX Types Tests
// ============================================================================

void test_side_enum() {
    assert_equal(static_cast<uint8_t>(Side::BID), static_cast<uint8_t>(1), "BID should be 1");
    assert_equal(static_cast<uint8_t>(Side::ASK), static_cast<uint8_t>(2), "ASK should be 2");
}

void test_ord_status_enum() {
    assert_equal(static_cast<uint8_t>(OrdStatus::NEW), static_cast<uint8_t>(0));
    assert_equal(static_cast<uint8_t>(OrdStatus::PARTIALLY_FILLED), static_cast<uint8_t>(1));
    assert_equal(static_cast<uint8_t>(OrdStatus::FILLED), static_cast<uint8_t>(2));
    assert_equal(static_cast<uint8_t>(OrdStatus::CANCELED), static_cast<uint8_t>(4));
    assert_equal(static_cast<uint8_t>(OrdStatus::REJECTED), static_cast<uint8_t>(8));
}

void test_exec_type_enum() {
    assert_equal(static_cast<uint8_t>(ExecType::NEW), static_cast<uint8_t>(0));
    assert_equal(static_cast<uint8_t>(ExecType::PARTIAL_FILL), static_cast<uint8_t>(1));
    assert_equal(static_cast<uint8_t>(ExecType::FILL), static_cast<uint8_t>(2));
    assert_equal(static_cast<uint8_t>(ExecType::CANCELED), static_cast<uint8_t>(4));
    assert_equal(static_cast<uint8_t>(ExecType::REPLACED), static_cast<uint8_t>(5));
    assert_equal(static_cast<uint8_t>(ExecType::REJECTED), static_cast<uint8_t>(8));
}

void test_order_key_equality() {
    OrderKey k1{"ORD001"};
    OrderKey k2{"ORD001"};
    OrderKey k3{"ORD002"};

    assert_true(k1 == k2, "Same ClOrdID should be equal");
    assert_false(k1 == k3, "Different ClOrdID should not be equal");
}

void test_order_key_hash() {
    std::unordered_map<OrderKey, int> map;
    map[OrderKey{"ORD001"}] = 1;
    map[OrderKey{"ORD002"}] = 2;

    assert_equal(map[OrderKey{"ORD001"}], 1);
    assert_equal(map[OrderKey{"ORD002"}], 2);
}

// ============================================================================
// FIX Parser Tests
// ============================================================================

void test_parse_fix_fields() {
    std::string msg = "35=D\x01" "11=ORD001\x01" "55=AAPL\x01" "54=1\x01" "38=100\x01" "44=150.50\x01";
    auto fields = parse_fix_fields(msg);

    assert_equal(fields[tags::MSG_TYPE], std::string("D"));
    assert_equal(fields[tags::CL_ORD_ID], std::string("ORD001"));
    assert_equal(fields[tags::SYMBOL], std::string("AAPL"));
    assert_equal(fields[tags::SIDE], std::string("1"));
    assert_equal(fields[tags::ORDER_QTY], std::string("100"));
    assert_equal(fields[tags::PRICE], std::string("150.50"));
}

void test_parse_side() {
    assert_equal(parse_side("1"), Side::BID);
    assert_equal(parse_side("2"), Side::ASK);
    assert_throws<ParseError>([]() { parse_side("3"); }, "Invalid side should throw");
}

void test_parse_ord_status() {
    assert_equal(parse_ord_status("0"), OrdStatus::NEW);
    assert_equal(parse_ord_status("1"), OrdStatus::PARTIALLY_FILLED);
    assert_equal(parse_ord_status("2"), OrdStatus::FILLED);
    assert_equal(parse_ord_status("4"), OrdStatus::CANCELED);
    assert_equal(parse_ord_status("8"), OrdStatus::REJECTED);
}

void test_parse_exec_type() {
    assert_equal(parse_exec_type("0"), ExecType::NEW);
    assert_equal(parse_exec_type("1"), ExecType::PARTIAL_FILL);
    assert_equal(parse_exec_type("2"), ExecType::FILL);
    assert_equal(parse_exec_type("4"), ExecType::CANCELED);
    assert_equal(parse_exec_type("5"), ExecType::REPLACED);
    assert_equal(parse_exec_type("8"), ExecType::REJECTED);
}

// ============================================================================
// NewOrderSingle Tests
// ============================================================================

void test_parse_new_order_single() {
    std::string msg = "35=D\x01" "11=ORD001\x01" "55=AAPL\x01" "311=AAPL\x01"
                      "54=1\x01" "38=100\x01" "44=150.50\x01"
                      "7001=STRAT1\x01" "7002=PORT1\x01" "7003=0.5\x01";
    auto fields = parse_fix_fields(msg);
    auto order = parse_new_order_single(fields);

    assert_equal(order.key.cl_ord_id, std::string("ORD001"));
    assert_equal(order.symbol, std::string("AAPL"));
    assert_equal(order.underlyer, std::string("AAPL"));
    assert_equal(order.side, Side::BID);
    assert_double_equal(order.quantity, 100.0);
    assert_double_equal(order.price, 150.50);
    assert_equal(order.strategy_id, std::string("STRAT1"));
    assert_equal(order.portfolio_id, std::string("PORT1"));
    assert_double_equal(order.delta, 0.5);
}

void test_new_order_single_notional() {
    NewOrderSingle order;
    order.price = 100.0;
    order.quantity = 50.0;
    assert_double_equal(order.notional(), 5000.0);
}

void test_new_order_single_delta_exposure() {
    NewOrderSingle order;
    order.delta = 0.5;
    order.quantity = 100.0;
    assert_double_equal(order.delta_exposure(), 50.0);
}

void test_serialize_new_order_single() {
    NewOrderSingle order;
    order.key.cl_ord_id = "ORD001";
    order.symbol = "AAPL";
    order.underlyer = "AAPL";
    order.side = Side::BID;
    order.quantity = 100;
    order.price = 150.50;
    order.strategy_id = "STRAT1";
    order.portfolio_id = "PORT1";
    order.delta = 1.0;

    std::string serialized = serialize_new_order_single(order);
    auto fields = parse_fix_fields(serialized);

    assert_equal(fields[tags::MSG_TYPE], std::string("D"));
    assert_equal(fields[tags::CL_ORD_ID], std::string("ORD001"));
    assert_equal(fields[tags::SYMBOL], std::string("AAPL"));
}

// ============================================================================
// OrderCancelReplaceRequest Tests
// ============================================================================

void test_parse_order_cancel_replace() {
    std::string msg = "35=G\x01" "11=ORD002\x01" "41=ORD001\x01"
                      "55=AAPL\x01" "54=1\x01" "38=150\x01" "44=155.00\x01";
    auto fields = parse_fix_fields(msg);
    auto req = parse_order_cancel_replace(fields);

    assert_equal(req.key.cl_ord_id, std::string("ORD002"));
    assert_equal(req.orig_key.cl_ord_id, std::string("ORD001"));
    assert_equal(req.symbol, std::string("AAPL"));
    assert_equal(req.side, Side::BID);
    assert_double_equal(req.quantity, 150.0);
    assert_double_equal(req.price, 155.00);
}

void test_serialize_order_cancel_replace() {
    OrderCancelReplaceRequest req;
    req.key.cl_ord_id = "ORD002";
    req.orig_key.cl_ord_id = "ORD001";
    req.symbol = "AAPL";
    req.side = Side::BID;
    req.quantity = 150;
    req.price = 155.00;

    std::string serialized = serialize_order_cancel_replace(req);
    auto fields = parse_fix_fields(serialized);

    assert_equal(fields[tags::MSG_TYPE], std::string("G"));
    assert_equal(fields[tags::CL_ORD_ID], std::string("ORD002"));
    assert_equal(fields[tags::ORIG_CL_ORD_ID], std::string("ORD001"));
}

// ============================================================================
// OrderCancelRequest Tests
// ============================================================================

void test_parse_order_cancel_request() {
    std::string msg = "35=F\x01" "11=CXLORD001\x01" "41=ORD001\x01"
                      "55=AAPL\x01" "54=1\x01";
    auto fields = parse_fix_fields(msg);
    auto req = parse_order_cancel_request(fields);

    assert_equal(req.key.cl_ord_id, std::string("CXLORD001"));
    assert_equal(req.orig_key.cl_ord_id, std::string("ORD001"));
    assert_equal(req.symbol, std::string("AAPL"));
    assert_equal(req.side, Side::BID);
}

void test_serialize_order_cancel_request() {
    OrderCancelRequest req;
    req.key.cl_ord_id = "CXLORD001";
    req.orig_key.cl_ord_id = "ORD001";
    req.symbol = "AAPL";
    req.side = Side::ASK;

    std::string serialized = serialize_order_cancel_request(req);
    auto fields = parse_fix_fields(serialized);

    assert_equal(fields[tags::MSG_TYPE], std::string("F"));
    assert_equal(fields[tags::CL_ORD_ID], std::string("CXLORD001"));
    assert_equal(fields[tags::ORIG_CL_ORD_ID], std::string("ORD001"));
}

// ============================================================================
// ExecutionReport Tests
// ============================================================================

void test_parse_execution_report_insert_ack() {
    std::string msg = "35=8\x01" "11=ORD001\x01" "37=EX001\x01"
                      "39=0\x01" "150=0\x01" "151=100\x01" "14=0\x01";
    auto fields = parse_fix_fields(msg);
    auto report = parse_execution_report(fields);

    assert_equal(report.key.cl_ord_id, std::string("ORD001"));
    assert_equal(report.order_id, std::string("EX001"));
    assert_equal(report.ord_status, OrdStatus::NEW);
    assert_equal(report.exec_type, ExecType::NEW);
    assert_double_equal(report.leaves_qty, 100.0);
    assert_double_equal(report.cum_qty, 0.0);
    assert_equal(report.report_type(), ExecutionReportType::INSERT_ACK);
}

void test_parse_execution_report_insert_nack() {
    std::string msg = "35=8\x01" "11=ORD001\x01" "37=EX001\x01"
                      "39=8\x01" "150=8\x01" "151=0\x01" "14=0\x01"
                      "58=Insufficient margin\x01";
    auto fields = parse_fix_fields(msg);
    auto report = parse_execution_report(fields);

    assert_equal(report.ord_status, OrdStatus::REJECTED);
    assert_equal(report.exec_type, ExecType::REJECTED);
    assert_true(report.text.has_value());
    assert_equal(report.text.value(), std::string("Insufficient margin"));
    assert_equal(report.report_type(), ExecutionReportType::INSERT_NACK);
}

void test_parse_execution_report_partial_fill() {
    std::string msg = "35=8\x01" "11=ORD001\x01" "37=EX001\x01"
                      "39=1\x01" "150=1\x01" "151=50\x01" "14=50\x01"
                      "32=50\x01" "31=150.25\x01";
    auto fields = parse_fix_fields(msg);
    auto report = parse_execution_report(fields);

    assert_equal(report.ord_status, OrdStatus::PARTIALLY_FILLED);
    assert_equal(report.exec_type, ExecType::PARTIAL_FILL);
    assert_double_equal(report.leaves_qty, 50.0);
    assert_double_equal(report.cum_qty, 50.0);
    assert_double_equal(report.last_qty, 50.0);
    assert_double_equal(report.last_px, 150.25);
    assert_equal(report.report_type(), ExecutionReportType::PARTIAL_FILL);
}

void test_parse_execution_report_full_fill() {
    std::string msg = "35=8\x01" "11=ORD001\x01" "37=EX001\x01"
                      "39=2\x01" "150=2\x01" "151=0\x01" "14=100\x01"
                      "32=100\x01" "31=150.50\x01";
    auto fields = parse_fix_fields(msg);
    auto report = parse_execution_report(fields);

    assert_equal(report.ord_status, OrdStatus::FILLED);
    assert_equal(report.exec_type, ExecType::FILL);
    assert_double_equal(report.leaves_qty, 0.0);
    assert_double_equal(report.cum_qty, 100.0);
    assert_equal(report.report_type(), ExecutionReportType::FULL_FILL);
}

void test_parse_execution_report_cancel_ack() {
    std::string msg = "35=8\x01" "11=CXLORD001\x01" "41=ORD001\x01" "37=EX001\x01"
                      "39=4\x01" "150=4\x01" "151=0\x01" "14=0\x01";
    auto fields = parse_fix_fields(msg);
    auto report = parse_execution_report(fields);

    assert_equal(report.ord_status, OrdStatus::CANCELED);
    assert_equal(report.exec_type, ExecType::CANCELED);
    assert_true(report.orig_key.has_value());
    assert_equal(report.orig_key->cl_ord_id, std::string("ORD001"));
    assert_equal(report.report_type(), ExecutionReportType::CANCEL_ACK);
}

void test_parse_execution_report_unsolicited_cancel() {
    std::string msg = "35=8\x01" "11=ORD001\x01" "37=EX001\x01"
                      "39=4\x01" "150=4\x01" "151=0\x01" "14=0\x01"
                      "58=Market closed\x01";
    auto fields = parse_fix_fields(msg);
    auto report = parse_execution_report(fields, true);  // unsolicited

    assert_equal(report.ord_status, OrdStatus::CANCELED);
    assert_true(report.is_unsolicited);
    assert_equal(report.report_type(), ExecutionReportType::UNSOLICITED_CANCEL);
}

void test_parse_execution_report_update_ack() {
    std::string msg = "35=8\x01" "11=ORD002\x01" "41=ORD001\x01" "37=EX001\x01"
                      "39=0\x01" "150=5\x01" "151=150\x01" "14=0\x01";
    auto fields = parse_fix_fields(msg);
    auto report = parse_execution_report(fields);

    assert_equal(report.exec_type, ExecType::REPLACED);
    assert_true(report.orig_key.has_value());
    assert_equal(report.report_type(), ExecutionReportType::UPDATE_ACK);
}

// ============================================================================
// OrderCancelReject Tests
// ============================================================================

void test_parse_order_cancel_reject_cancel_nack() {
    std::string msg = "35=9\x01" "11=CXLORD001\x01" "41=ORD001\x01" "37=EX001\x01"
                      "39=0\x01" "434=1\x01" "102=0\x01" "58=Too late to cancel\x01";
    auto fields = parse_fix_fields(msg);
    auto reject = parse_order_cancel_reject(fields);

    assert_equal(reject.key.cl_ord_id, std::string("CXLORD001"));
    assert_equal(reject.orig_key.cl_ord_id, std::string("ORD001"));
    assert_equal(reject.response_to, CxlRejResponseTo::ORDER_CANCEL_REQUEST);
    assert_true(reject.text.has_value());
    assert_equal(reject.report_type(), ExecutionReportType::CANCEL_NACK);
}

void test_parse_order_cancel_reject_update_nack() {
    std::string msg = "35=9\x01" "11=ORD002\x01" "41=ORD001\x01" "37=EX001\x01"
                      "39=0\x01" "434=2\x01" "102=1\x01" "58=Unknown order\x01";
    auto fields = parse_fix_fields(msg);
    auto reject = parse_order_cancel_reject(fields);

    assert_equal(reject.response_to, CxlRejResponseTo::ORDER_CANCEL_REPLACE_REQUEST);
    assert_equal(reject.report_type(), ExecutionReportType::UPDATE_NACK);
}

void test_serialize_execution_report() {
    ExecutionReport report;
    report.key.cl_ord_id = "ORD001";
    report.order_id = "EX001";
    report.ord_status = OrdStatus::NEW;
    report.exec_type = ExecType::NEW;
    report.leaves_qty = 100;
    report.cum_qty = 0;
    report.is_unsolicited = false;

    std::string serialized = serialize_execution_report(report);
    auto fields = parse_fix_fields(serialized);

    assert_equal(fields[tags::MSG_TYPE], std::string("8"));
    assert_equal(fields[tags::CL_ORD_ID], std::string("ORD001"));
    assert_equal(fields[tags::ORDER_ID], std::string("EX001"));
}

void test_serialize_order_cancel_reject() {
    OrderCancelReject reject;
    reject.key.cl_ord_id = "CXLORD001";
    reject.orig_key.cl_ord_id = "ORD001";
    reject.order_id = "EX001";
    reject.ord_status = OrdStatus::NEW;
    reject.response_to = CxlRejResponseTo::ORDER_CANCEL_REQUEST;
    reject.cxl_rej_reason = 0;
    reject.text = "Too late to cancel";

    std::string serialized = serialize_order_cancel_reject(reject);
    auto fields = parse_fix_fields(serialized);

    assert_equal(fields[tags::MSG_TYPE], std::string("9"));
    assert_equal(fields[tags::CL_ORD_ID], std::string("CXLORD001"));
}

// ============================================================================
// Run all FIX message tests
// ============================================================================

TestSuite run_fix_message_tests() {
    TestSuite suite("FIX Message Tests");

    // Types tests
    suite.run_test("Side enum values", test_side_enum);
    suite.run_test("OrdStatus enum values", test_ord_status_enum);
    suite.run_test("ExecType enum values", test_exec_type_enum);
    suite.run_test("OrderKey equality", test_order_key_equality);
    suite.run_test("OrderKey hash map usage", test_order_key_hash);

    // Parser tests
    suite.run_test("Parse FIX fields", test_parse_fix_fields);
    suite.run_test("Parse Side", test_parse_side);
    suite.run_test("Parse OrdStatus", test_parse_ord_status);
    suite.run_test("Parse ExecType", test_parse_exec_type);

    // NewOrderSingle tests
    suite.run_test("Parse NewOrderSingle", test_parse_new_order_single);
    suite.run_test("NewOrderSingle notional calculation", test_new_order_single_notional);
    suite.run_test("NewOrderSingle delta exposure calculation", test_new_order_single_delta_exposure);
    suite.run_test("Serialize NewOrderSingle", test_serialize_new_order_single);

    // OrderCancelReplaceRequest tests
    suite.run_test("Parse OrderCancelReplaceRequest", test_parse_order_cancel_replace);
    suite.run_test("Serialize OrderCancelReplaceRequest", test_serialize_order_cancel_replace);

    // OrderCancelRequest tests
    suite.run_test("Parse OrderCancelRequest", test_parse_order_cancel_request);
    suite.run_test("Serialize OrderCancelRequest", test_serialize_order_cancel_request);

    // ExecutionReport tests
    suite.run_test("Parse ExecutionReport - Insert Ack", test_parse_execution_report_insert_ack);
    suite.run_test("Parse ExecutionReport - Insert Nack", test_parse_execution_report_insert_nack);
    suite.run_test("Parse ExecutionReport - Partial Fill", test_parse_execution_report_partial_fill);
    suite.run_test("Parse ExecutionReport - Full Fill", test_parse_execution_report_full_fill);
    suite.run_test("Parse ExecutionReport - Cancel Ack", test_parse_execution_report_cancel_ack);
    suite.run_test("Parse ExecutionReport - Unsolicited Cancel", test_parse_execution_report_unsolicited_cancel);
    suite.run_test("Parse ExecutionReport - Update Ack", test_parse_execution_report_update_ack);
    suite.run_test("Serialize ExecutionReport", test_serialize_execution_report);

    // OrderCancelReject tests
    suite.run_test("Parse OrderCancelReject - Cancel Nack", test_parse_order_cancel_reject_cancel_nack);
    suite.run_test("Parse OrderCancelReject - Update Nack", test_parse_order_cancel_reject_update_nack);
    suite.run_test("Serialize OrderCancelReject", test_serialize_order_cancel_reject);

    return suite;
}
