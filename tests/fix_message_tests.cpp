#include <gtest/gtest.h>
#include "../src/fix/fix_types.hpp"
#include "../src/fix/fix_messages.hpp"
#include "../src/fix/fix_parser.hpp"

using namespace fix;

// ============================================================================
// Parameterized Tests for Enum Parsing
// ============================================================================

struct SideTestParam {
    std::string input;
    Side expected;
};

class SideParseTest : public ::testing::TestWithParam<SideTestParam> {};

TEST_P(SideParseTest, ParsesSideCorrectly) {
    auto param = GetParam();
    EXPECT_EQ(parse_side(param.input), param.expected);
}

INSTANTIATE_TEST_SUITE_P(
    SideValues,
    SideParseTest,
    ::testing::Values(
        SideTestParam{"1", Side::BID},
        SideTestParam{"2", Side::ASK}
    )
);

TEST(SideParseTest, InvalidSideThrows) {
    EXPECT_THROW(parse_side("3"), ParseError);
    EXPECT_THROW(parse_side("0"), ParseError);
    EXPECT_THROW(parse_side(""), ParseError);
}

struct OrdStatusTestParam {
    std::string input;
    OrdStatus expected;
};

class OrdStatusParseTest : public ::testing::TestWithParam<OrdStatusTestParam> {};

TEST_P(OrdStatusParseTest, ParsesOrdStatusCorrectly) {
    auto param = GetParam();
    EXPECT_EQ(parse_ord_status(param.input), param.expected);
}

INSTANTIATE_TEST_SUITE_P(
    OrdStatusValues,
    OrdStatusParseTest,
    ::testing::Values(
        OrdStatusTestParam{"0", OrdStatus::NEW},
        OrdStatusTestParam{"1", OrdStatus::PARTIALLY_FILLED},
        OrdStatusTestParam{"2", OrdStatus::FILLED},
        OrdStatusTestParam{"4", OrdStatus::CANCELED},
        OrdStatusTestParam{"8", OrdStatus::REJECTED}
    )
);

struct ExecTypeTestParam {
    std::string input;
    ExecType expected;
};

class ExecTypeParseTest : public ::testing::TestWithParam<ExecTypeTestParam> {};

TEST_P(ExecTypeParseTest, ParsesExecTypeCorrectly) {
    auto param = GetParam();
    EXPECT_EQ(parse_exec_type(param.input), param.expected);
}

INSTANTIATE_TEST_SUITE_P(
    ExecTypeValues,
    ExecTypeParseTest,
    ::testing::Values(
        ExecTypeTestParam{"0", ExecType::NEW},
        ExecTypeTestParam{"1", ExecType::PARTIAL_FILL},
        ExecTypeTestParam{"2", ExecType::FILL},
        ExecTypeTestParam{"4", ExecType::CANCELED},
        ExecTypeTestParam{"5", ExecType::REPLACED},
        ExecTypeTestParam{"8", ExecType::REJECTED}
    )
);

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
    std::unordered_map<OrderKey, int> map;
    map[OrderKey{"ORD001"}] = 1;
    map[OrderKey{"ORD002"}] = 2;

    EXPECT_EQ(map[OrderKey{"ORD001"}], 1);
    EXPECT_EQ(map[OrderKey{"ORD002"}], 2);
}

// ============================================================================
// FIX Parser Tests
// ============================================================================

TEST(FixParserTest, ParseFixFields) {
    std::string msg = "35=D\x01" "11=ORD001\x01" "55=AAPL\x01" "54=1\x01" "38=100\x01" "44=150.50\x01";
    auto fields = parse_fix_fields(msg);

    EXPECT_EQ(fields[tags::MSG_TYPE], "D");
    EXPECT_EQ(fields[tags::CL_ORD_ID], "ORD001");
    EXPECT_EQ(fields[tags::SYMBOL], "AAPL");
    EXPECT_EQ(fields[tags::SIDE], "1");
    EXPECT_EQ(fields[tags::ORDER_QTY], "100");
    EXPECT_EQ(fields[tags::PRICE], "150.50");
}

// ============================================================================
// NewOrderSingle Tests
// ============================================================================

TEST(NewOrderSingleTest, Parse) {
    std::string msg = "35=D\x01" "11=ORD001\x01" "55=AAPL\x01" "311=AAPL\x01"
                      "54=1\x01" "38=100\x01" "44=150.50\x01"
                      "7001=STRAT1\x01" "7002=PORT1\x01" "7003=0.5\x01";
    auto fields = parse_fix_fields(msg);
    auto order = parse_new_order_single(fields);

    EXPECT_EQ(order.key.cl_ord_id, "ORD001");
    EXPECT_EQ(order.symbol, "AAPL");
    EXPECT_EQ(order.underlyer, "AAPL");
    EXPECT_EQ(order.side, Side::BID);
    EXPECT_DOUBLE_EQ(order.quantity, 100.0);
    EXPECT_DOUBLE_EQ(order.price, 150.50);
    EXPECT_EQ(order.strategy_id, "STRAT1");
    EXPECT_EQ(order.portfolio_id, "PORT1");
    EXPECT_DOUBLE_EQ(order.delta, 0.5);
}

TEST(NewOrderSingleTest, NotionalCalculation) {
    NewOrderSingle order;
    order.price = 100.0;
    order.quantity = 50.0;
    EXPECT_DOUBLE_EQ(order.notional(), 5000.0);
}

TEST(NewOrderSingleTest, DeltaExposureCalculation) {
    NewOrderSingle order;
    order.delta = 0.5;
    order.quantity = 100.0;
    EXPECT_DOUBLE_EQ(order.delta_exposure(), 50.0);
}

TEST(NewOrderSingleTest, SerializeAndParse) {
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

    EXPECT_EQ(fields[tags::MSG_TYPE], "D");
    EXPECT_EQ(fields[tags::CL_ORD_ID], "ORD001");
    EXPECT_EQ(fields[tags::SYMBOL], "AAPL");
}

// ============================================================================
// OrderCancelReplaceRequest Tests
// ============================================================================

TEST(OrderCancelReplaceRequestTest, Parse) {
    std::string msg = "35=G\x01" "11=ORD002\x01" "41=ORD001\x01"
                      "55=AAPL\x01" "54=1\x01" "38=150\x01" "44=155.00\x01";
    auto fields = parse_fix_fields(msg);
    auto req = parse_order_cancel_replace(fields);

    EXPECT_EQ(req.key.cl_ord_id, "ORD002");
    EXPECT_EQ(req.orig_key.cl_ord_id, "ORD001");
    EXPECT_EQ(req.symbol, "AAPL");
    EXPECT_EQ(req.side, Side::BID);
    EXPECT_DOUBLE_EQ(req.quantity, 150.0);
    EXPECT_DOUBLE_EQ(req.price, 155.00);
}

TEST(OrderCancelReplaceRequestTest, SerializeAndParse) {
    OrderCancelReplaceRequest req;
    req.key.cl_ord_id = "ORD002";
    req.orig_key.cl_ord_id = "ORD001";
    req.symbol = "AAPL";
    req.side = Side::BID;
    req.quantity = 150;
    req.price = 155.00;

    std::string serialized = serialize_order_cancel_replace(req);
    auto fields = parse_fix_fields(serialized);

    EXPECT_EQ(fields[tags::MSG_TYPE], "G");
    EXPECT_EQ(fields[tags::CL_ORD_ID], "ORD002");
    EXPECT_EQ(fields[tags::ORIG_CL_ORD_ID], "ORD001");
}

// ============================================================================
// OrderCancelRequest Tests
// ============================================================================

TEST(OrderCancelRequestTest, Parse) {
    std::string msg = "35=F\x01" "11=CXLORD001\x01" "41=ORD001\x01"
                      "55=AAPL\x01" "54=1\x01";
    auto fields = parse_fix_fields(msg);
    auto req = parse_order_cancel_request(fields);

    EXPECT_EQ(req.key.cl_ord_id, "CXLORD001");
    EXPECT_EQ(req.orig_key.cl_ord_id, "ORD001");
    EXPECT_EQ(req.symbol, "AAPL");
    EXPECT_EQ(req.side, Side::BID);
}

TEST(OrderCancelRequestTest, SerializeAndParse) {
    OrderCancelRequest req;
    req.key.cl_ord_id = "CXLORD001";
    req.orig_key.cl_ord_id = "ORD001";
    req.symbol = "AAPL";
    req.side = Side::ASK;

    std::string serialized = serialize_order_cancel_request(req);
    auto fields = parse_fix_fields(serialized);

    EXPECT_EQ(fields[tags::MSG_TYPE], "F");
    EXPECT_EQ(fields[tags::CL_ORD_ID], "CXLORD001");
    EXPECT_EQ(fields[tags::ORIG_CL_ORD_ID], "ORD001");
}

// ============================================================================
// ExecutionReport Tests - Parameterized
// ============================================================================

struct ExecutionReportTestParam {
    std::string name;
    std::string message;
    OrdStatus expected_status;
    ExecType expected_exec_type;
    ExecutionReportType expected_report_type;
    bool is_unsolicited;
};

class ExecutionReportParseTest : public ::testing::TestWithParam<ExecutionReportTestParam> {};

TEST_P(ExecutionReportParseTest, ParsesCorrectly) {
    auto param = GetParam();
    auto fields = parse_fix_fields(param.message);
    auto report = parse_execution_report(fields, param.is_unsolicited);

    EXPECT_EQ(report.ord_status, param.expected_status);
    EXPECT_EQ(report.exec_type, param.expected_exec_type);
    EXPECT_EQ(report.report_type(), param.expected_report_type);
}

INSTANTIATE_TEST_SUITE_P(
    ExecutionReportTypes,
    ExecutionReportParseTest,
    ::testing::Values(
        ExecutionReportTestParam{
            "InsertAck",
            "35=8\x01" "11=ORD001\x01" "37=EX001\x01" "39=0\x01" "150=0\x01" "151=100\x01" "14=0\x01",
            OrdStatus::NEW, ExecType::NEW, ExecutionReportType::INSERT_ACK, false
        },
        ExecutionReportTestParam{
            "InsertNack",
            "35=8\x01" "11=ORD001\x01" "37=EX001\x01" "39=8\x01" "150=8\x01" "151=0\x01" "14=0\x01",
            OrdStatus::REJECTED, ExecType::REJECTED, ExecutionReportType::INSERT_NACK, false
        },
        ExecutionReportTestParam{
            "PartialFill",
            "35=8\x01" "11=ORD001\x01" "37=EX001\x01" "39=1\x01" "150=1\x01" "151=50\x01" "14=50\x01" "32=50\x01" "31=150.25\x01",
            OrdStatus::PARTIALLY_FILLED, ExecType::PARTIAL_FILL, ExecutionReportType::PARTIAL_FILL, false
        },
        ExecutionReportTestParam{
            "FullFill",
            "35=8\x01" "11=ORD001\x01" "37=EX001\x01" "39=2\x01" "150=2\x01" "151=0\x01" "14=100\x01" "32=100\x01" "31=150.50\x01",
            OrdStatus::FILLED, ExecType::FILL, ExecutionReportType::FULL_FILL, false
        },
        ExecutionReportTestParam{
            "CancelAck",
            "35=8\x01" "11=CXLORD001\x01" "41=ORD001\x01" "37=EX001\x01" "39=4\x01" "150=4\x01" "151=0\x01" "14=0\x01",
            OrdStatus::CANCELED, ExecType::CANCELED, ExecutionReportType::CANCEL_ACK, false
        },
        ExecutionReportTestParam{
            "UnsolicitedCancel",
            "35=8\x01" "11=ORD001\x01" "37=EX001\x01" "39=4\x01" "150=4\x01" "151=0\x01" "14=0\x01",
            OrdStatus::CANCELED, ExecType::CANCELED, ExecutionReportType::UNSOLICITED_CANCEL, true
        },
        ExecutionReportTestParam{
            "UpdateAck",
            "35=8\x01" "11=ORD002\x01" "41=ORD001\x01" "37=EX001\x01" "39=0\x01" "150=5\x01" "151=150\x01" "14=0\x01",
            OrdStatus::NEW, ExecType::REPLACED, ExecutionReportType::UPDATE_ACK, false
        }
    ),
    [](const ::testing::TestParamInfo<ExecutionReportTestParam>& info) {
        return info.param.name;
    }
);

TEST(ExecutionReportTest, InsertNackWithText) {
    std::string msg = "35=8\x01" "11=ORD001\x01" "37=EX001\x01"
                      "39=8\x01" "150=8\x01" "151=0\x01" "14=0\x01"
                      "58=Insufficient margin\x01";
    auto fields = parse_fix_fields(msg);
    auto report = parse_execution_report(fields);

    EXPECT_TRUE(report.text.has_value());
    EXPECT_EQ(report.text.value(), "Insufficient margin");
}

TEST(ExecutionReportTest, PartialFillQuantities) {
    std::string msg = "35=8\x01" "11=ORD001\x01" "37=EX001\x01"
                      "39=1\x01" "150=1\x01" "151=50\x01" "14=50\x01"
                      "32=50\x01" "31=150.25\x01";
    auto fields = parse_fix_fields(msg);
    auto report = parse_execution_report(fields);

    EXPECT_DOUBLE_EQ(report.leaves_qty, 50.0);
    EXPECT_DOUBLE_EQ(report.cum_qty, 50.0);
    EXPECT_DOUBLE_EQ(report.last_qty, 50.0);
    EXPECT_DOUBLE_EQ(report.last_px, 150.25);
}

TEST(ExecutionReportTest, CancelAckWithOrigKey) {
    std::string msg = "35=8\x01" "11=CXLORD001\x01" "41=ORD001\x01" "37=EX001\x01"
                      "39=4\x01" "150=4\x01" "151=0\x01" "14=0\x01";
    auto fields = parse_fix_fields(msg);
    auto report = parse_execution_report(fields);

    EXPECT_TRUE(report.orig_key.has_value());
    EXPECT_EQ(report.orig_key->cl_ord_id, "ORD001");
}

TEST(ExecutionReportTest, Serialize) {
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

    EXPECT_EQ(fields[tags::MSG_TYPE], "8");
    EXPECT_EQ(fields[tags::CL_ORD_ID], "ORD001");
    EXPECT_EQ(fields[tags::ORDER_ID], "EX001");
}

// ============================================================================
// OrderCancelReject Tests - Parameterized
// ============================================================================

struct OrderCancelRejectTestParam {
    std::string name;
    std::string message;
    CxlRejResponseTo expected_response_to;
    ExecutionReportType expected_report_type;
};

class OrderCancelRejectParseTest : public ::testing::TestWithParam<OrderCancelRejectTestParam> {};

TEST_P(OrderCancelRejectParseTest, ParsesCorrectly) {
    auto param = GetParam();
    auto fields = parse_fix_fields(param.message);
    auto reject = parse_order_cancel_reject(fields);

    EXPECT_EQ(reject.response_to, param.expected_response_to);
    EXPECT_EQ(reject.report_type(), param.expected_report_type);
}

INSTANTIATE_TEST_SUITE_P(
    OrderCancelRejectTypes,
    OrderCancelRejectParseTest,
    ::testing::Values(
        OrderCancelRejectTestParam{
            "CancelNack",
            "35=9\x01" "11=CXLORD001\x01" "41=ORD001\x01" "37=EX001\x01" "39=0\x01" "434=1\x01" "102=0\x01" "58=Too late to cancel\x01",
            CxlRejResponseTo::ORDER_CANCEL_REQUEST, ExecutionReportType::CANCEL_NACK
        },
        OrderCancelRejectTestParam{
            "UpdateNack",
            "35=9\x01" "11=ORD002\x01" "41=ORD001\x01" "37=EX001\x01" "39=0\x01" "434=2\x01" "102=1\x01" "58=Unknown order\x01",
            CxlRejResponseTo::ORDER_CANCEL_REPLACE_REQUEST, ExecutionReportType::UPDATE_NACK
        }
    ),
    [](const ::testing::TestParamInfo<OrderCancelRejectTestParam>& info) {
        return info.param.name;
    }
);

TEST(OrderCancelRejectTest, ParseFields) {
    std::string msg = "35=9\x01" "11=CXLORD001\x01" "41=ORD001\x01" "37=EX001\x01"
                      "39=0\x01" "434=1\x01" "102=0\x01" "58=Too late to cancel\x01";
    auto fields = parse_fix_fields(msg);
    auto reject = parse_order_cancel_reject(fields);

    EXPECT_EQ(reject.key.cl_ord_id, "CXLORD001");
    EXPECT_EQ(reject.orig_key.cl_ord_id, "ORD001");
    EXPECT_TRUE(reject.text.has_value());
}

TEST(OrderCancelRejectTest, Serialize) {
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

    EXPECT_EQ(fields[tags::MSG_TYPE], "9");
    EXPECT_EQ(fields[tags::CL_ORD_ID], "CXLORD001");
}
