#include <gtest/gtest.h>
#include "../src/fix/fix_types.hpp"
#include "../src/fix/fix_messages.hpp"
#include "../src/aggregation/container_types.hpp"

using namespace fix;

// ============================================================================
// FIX Types Tests
// ============================================================================

TEST(SideEnumTest, HasCorrectValues) {
    EXPECT_EQ(static_cast<uint8_t>(Side::BID), 1);
    EXPECT_EQ(static_cast<uint8_t>(Side::ASK), 2);
}

TEST(OrdStatusEnumTest, HasCorrectValues) {
    EXPECT_EQ(static_cast<uint8_t>(OrdStatus::NEW), 0);
    EXPECT_EQ(static_cast<uint8_t>(OrdStatus::PARTIALLY_FILLED), 1);
    EXPECT_EQ(static_cast<uint8_t>(OrdStatus::FILLED), 2);
    EXPECT_EQ(static_cast<uint8_t>(OrdStatus::CANCELED), 4);
    EXPECT_EQ(static_cast<uint8_t>(OrdStatus::REJECTED), 8);
}

TEST(ExecTypeEnumTest, HasCorrectValues) {
    EXPECT_EQ(static_cast<uint8_t>(ExecType::NEW), 0);
    EXPECT_EQ(static_cast<uint8_t>(ExecType::PARTIAL_FILL), 1);
    EXPECT_EQ(static_cast<uint8_t>(ExecType::FILL), 2);
    EXPECT_EQ(static_cast<uint8_t>(ExecType::CANCELED), 4);
    EXPECT_EQ(static_cast<uint8_t>(ExecType::REPLACED), 5);
    EXPECT_EQ(static_cast<uint8_t>(ExecType::REJECTED), 8);
}

TEST(OrderKeyTest, Equality) {
    OrderKey k1{"ORD001"};
    OrderKey k2{"ORD001"};
    OrderKey k3{"ORD002"};

    EXPECT_EQ(k1, k2);
    EXPECT_NE(k1, k3);
}

TEST(OrderKeyTest, HashMapUsage) {
    aggregation::HashMap<OrderKey, int> map;
    map[OrderKey{"ORD001"}] = 1;
    map[OrderKey{"ORD002"}] = 2;

    EXPECT_EQ(map[OrderKey{"ORD001"}], 1);
    EXPECT_EQ(map[OrderKey{"ORD002"}], 2);
}

// ============================================================================
// ExecutionReport report_type() Tests
// ============================================================================

TEST(ExecutionReportTypeTest, InsertAck) {
    ExecutionReport report;
    report.key.cl_ord_id = "ORD001";
    report.order_id = "EX001";
    report.ord_status = OrdStatus::NEW;
    report.exec_type = ExecType::NEW;
    report.leaves_qty = 100;
    report.cum_qty = 0;
    report.is_unsolicited = false;
    // No orig_key

    EXPECT_EQ(report.report_type(), ExecutionReportType::INSERT_ACK);
}

TEST(ExecutionReportTypeTest, InsertNack) {
    ExecutionReport report;
    report.key.cl_ord_id = "ORD001";
    report.order_id = "EX001";
    report.ord_status = OrdStatus::REJECTED;
    report.exec_type = ExecType::REJECTED;
    report.leaves_qty = 0;
    report.cum_qty = 0;
    report.is_unsolicited = false;
    // No orig_key

    EXPECT_EQ(report.report_type(), ExecutionReportType::INSERT_NACK);
}

TEST(ExecutionReportTypeTest, UpdateAckViaReplaced) {
    ExecutionReport report;
    report.key.cl_ord_id = "ORD002";
    report.orig_key = OrderKey{"ORD001"};
    report.order_id = "EX001";
    report.ord_status = OrdStatus::NEW;
    report.exec_type = ExecType::REPLACED;
    report.leaves_qty = 150;
    report.cum_qty = 0;
    report.is_unsolicited = false;

    EXPECT_EQ(report.report_type(), ExecutionReportType::UPDATE_ACK);
}

TEST(ExecutionReportTypeTest, UpdateNackViaRejectedWithOrigKey) {
    ExecutionReport report;
    report.key.cl_ord_id = "ORD002";
    report.orig_key = OrderKey{"ORD001"};
    report.order_id = "EX001";
    report.ord_status = OrdStatus::REJECTED;
    report.exec_type = ExecType::REJECTED;
    report.leaves_qty = 0;
    report.cum_qty = 0;
    report.is_unsolicited = false;

    EXPECT_EQ(report.report_type(), ExecutionReportType::UPDATE_NACK);
}

TEST(ExecutionReportTypeTest, CancelAck) {
    ExecutionReport report;
    report.key.cl_ord_id = "CXLORD001";
    report.orig_key = OrderKey{"ORD001"};
    report.order_id = "EX001";
    report.ord_status = OrdStatus::CANCELED;
    report.exec_type = ExecType::CANCELED;
    report.leaves_qty = 0;
    report.cum_qty = 0;
    report.is_unsolicited = false;

    EXPECT_EQ(report.report_type(), ExecutionReportType::CANCEL_ACK);
}

TEST(ExecutionReportTypeTest, UnsolicitedCancel) {
    ExecutionReport report;
    report.key.cl_ord_id = "ORD001";
    report.order_id = "EX001";
    report.ord_status = OrdStatus::CANCELED;
    report.exec_type = ExecType::CANCELED;
    report.leaves_qty = 0;
    report.cum_qty = 0;
    report.is_unsolicited = true;

    EXPECT_EQ(report.report_type(), ExecutionReportType::UNSOLICITED_CANCEL);
}

TEST(ExecutionReportTypeTest, PartialFill) {
    ExecutionReport report;
    report.key.cl_ord_id = "ORD001";
    report.order_id = "EX001";
    report.ord_status = OrdStatus::PARTIALLY_FILLED;
    report.exec_type = ExecType::PARTIAL_FILL;
    report.leaves_qty = 50;
    report.cum_qty = 50;
    report.last_qty = 50;
    report.last_px = 150.25;
    report.is_unsolicited = false;

    EXPECT_EQ(report.report_type(), ExecutionReportType::PARTIAL_FILL);
}

TEST(ExecutionReportTypeTest, FullFill) {
    ExecutionReport report;
    report.key.cl_ord_id = "ORD001";
    report.order_id = "EX001";
    report.ord_status = OrdStatus::FILLED;
    report.exec_type = ExecType::FILL;
    report.leaves_qty = 0;
    report.cum_qty = 100;
    report.last_qty = 100;
    report.last_px = 150.50;
    report.is_unsolicited = false;

    EXPECT_EQ(report.report_type(), ExecutionReportType::FULL_FILL);
}

// ============================================================================
// OrderCancelReject report_type() Tests
// ============================================================================

TEST(OrderCancelRejectTypeTest, CancelNack) {
    OrderCancelReject reject;
    reject.key.cl_ord_id = "CXLORD001";
    reject.orig_key.cl_ord_id = "ORD001";
    reject.order_id = "EX001";
    reject.ord_status = OrdStatus::NEW;
    reject.response_to = CxlRejResponseTo::ORDER_CANCEL_REQUEST;
    reject.cxl_rej_reason = 0;
    reject.text = "Too late to cancel";

    EXPECT_EQ(reject.report_type(), ExecutionReportType::CANCEL_NACK);
}

TEST(OrderCancelRejectTypeTest, UpdateNack) {
    OrderCancelReject reject;
    reject.key.cl_ord_id = "ORD002";
    reject.orig_key.cl_ord_id = "ORD001";
    reject.order_id = "EX001";
    reject.ord_status = OrdStatus::NEW;
    reject.response_to = CxlRejResponseTo::ORDER_CANCEL_REPLACE_REQUEST;
    reject.cxl_rej_reason = 1;
    reject.text = "Unknown order";

    EXPECT_EQ(reject.report_type(), ExecutionReportType::UPDATE_NACK);
}
