#include <gtest/gtest.h>
#include "../src/engine/risk_engine_with_limits.hpp"
#include "../src/metrics/notional_metric.hpp"
#include "../src/metrics/delta_metric.hpp"
#include "../src/instrument/instrument.hpp"
#include "../src/fix/fix_messages.hpp"
#include <memory>

using namespace engine;
using namespace fix;
using namespace aggregation;
using namespace metrics;
using namespace instrument;

// ============================================================================
// TestContext with mutable spot price support
// ============================================================================
//
// This context allows spot prices to be updated during test execution
// to verify drift-free behavior.
//

class DriftTestContext {
    StaticInstrumentProvider& provider_;
public:
    explicit DriftTestContext(StaticInstrumentProvider& provider) : provider_(provider) {}

    double spot_price(const InstrumentData& inst) const { return inst.spot_price(); }
    double fx_rate(const InstrumentData& inst) const { return inst.fx_rate(); }
    double contract_size(const InstrumentData& inst) const { return inst.contract_size(); }
    const std::string& underlyer(const InstrumentData& inst) const { return inst.underlyer(); }
    double underlyer_spot(const InstrumentData& inst) const { return inst.underlyer_spot(); }
    double delta(const InstrumentData& inst) const { return inst.delta(); }
    double vega(const InstrumentData& inst) const { return inst.vega(); }
};

// ============================================================================
// Helper functions
// ============================================================================

namespace {

NewOrderSingle create_order(const std::string& cl_ord_id, const std::string& symbol,
                             Side side, double price, int64_t qty,
                             const std::string& strategy = "STRAT1") {
    NewOrderSingle order;
    order.key.cl_ord_id = cl_ord_id;
    order.symbol = symbol;
    order.underlyer = symbol;  // Equities: underlyer = symbol
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
// Test: Notional Drift-Free with Spot Price Changes
// ============================================================================
//
// This test verifies that notional metrics don't drift when spot prices
// change between operations. The key insight is that when removing an order's
// contribution, we use the stored inputs from when it was added.
//

class NotionalDriftTest : public ::testing::Test {
protected:
    using GlobalNotional = GlobalGrossNotionalMetric<DriftTestContext, InstrumentData, OpenStage, InFlightStage>;

    using TestEngine = RiskAggregationEngineWithLimits<
        DriftTestContext,
        InstrumentData,
        GlobalNotional
    >;

    StaticInstrumentProvider provider;
    std::unique_ptr<DriftTestContext> context;
    std::unique_ptr<TestEngine> engine;

    void SetUp() override {
        provider.add_equity("AAPL", 100.0);  // Initial spot = $100
        provider.add_equity("MSFT", 200.0);
        context = std::make_unique<DriftTestContext>(provider);
        engine = std::make_unique<TestEngine>(*context);
    }

    double gross_notional() const {
        return engine->get_metric<GlobalNotional>().get(GlobalKey::instance());
    }

    double get_in_flight_notional() const {
        return engine->get_metric<GlobalNotional>().get_in_flight(GlobalKey::instance());
    }

    double get_open_notional() const {
        return engine->get_metric<GlobalNotional>().get_open(GlobalKey::instance());
    }

    InstrumentData get_instrument(const std::string& symbol) const {
        return provider.get_instrument(symbol);
    }
};

TEST_F(NotionalDriftTest, SpotPriceChangeBetweenInsertAndAck) {
    // Step 1: Insert order qty=10 at spot=$100
    auto order = create_order("ORD001", "AAPL", Side::BID, 100.0, 10);
    auto inst = get_instrument("AAPL");
    engine->on_new_order_single(order, inst);

    // in_flight = 10 * 100 = 1000
    EXPECT_DOUBLE_EQ(get_in_flight_notional(), 1000.0) << "After INSERT at spot=$100";
    EXPECT_DOUBLE_EQ(get_open_notional(), 0.0);

    // Step 2: Spot moves to $110 BEFORE ack
    provider.update_spot_price("AAPL", 110.0);
    auto inst2 = get_instrument("AAPL");

    // ACK order - moves from IN_FLIGHT to OPEN
    // Remove from IN_FLIGHT using stored spot=$100: -1000
    // Add to OPEN using current spot=$110: +1100
    engine->on_execution_report(create_ack("ORD001", 10), inst2);

    EXPECT_DOUBLE_EQ(get_in_flight_notional(), 0.0) << "IN_FLIGHT should be exactly 0 (no drift!)";
    EXPECT_DOUBLE_EQ(get_open_notional(), 1100.0) << "OPEN = 10 * 110 = 1100";
}

TEST_F(NotionalDriftTest, SpotPriceChangeBetweenInsertAndNack) {
    // Step 1: Insert order qty=10 at spot=$100
    auto order = create_order("ORD001", "AAPL", Side::BID, 100.0, 10);
    auto inst = get_instrument("AAPL");
    engine->on_new_order_single(order, inst);

    // in_flight = 10 * 100 = 1000
    EXPECT_DOUBLE_EQ(get_in_flight_notional(), 1000.0);

    // Step 2: Spot moves to $110 BEFORE nack
    provider.update_spot_price("AAPL", 110.0);
    auto inst2 = get_instrument("AAPL");

    // NACK order - removes from IN_FLIGHT using stored spot=$100
    engine->on_execution_report(create_nack("ORD001"), inst2);

    // CRITICAL: IN_FLIGHT should be exactly 0 (no drift!)
    // We remove exactly 1000 (stored), not 1100 (current)
    EXPECT_DOUBLE_EQ(get_in_flight_notional(), 0.0) << "IN_FLIGHT should be exactly 0 (no drift!)";
}

TEST_F(NotionalDriftTest, SpotPriceChangeBetweenAckAndFill) {
    // Step 1: Insert and ACK order at spot=$100
    auto order = create_order("ORD001", "AAPL", Side::BID, 100.0, 10);
    auto inst = get_instrument("AAPL");
    engine->on_new_order_single(order, inst);
    engine->on_execution_report(create_ack("ORD001", 10), inst);

    EXPECT_DOUBLE_EQ(get_open_notional(), 1000.0) << "OPEN = 10 * 100 = 1000";

    // Step 2: Spot moves to $120 BEFORE fill
    provider.update_spot_price("AAPL", 120.0);
    auto inst2 = get_instrument("AAPL");

    // Full fill - removes from OPEN using stored spot=$100
    engine->on_execution_report(create_fill("ORD001", 10, 0, 120.0), inst2);

    // CRITICAL: OPEN should be exactly 0 (no drift!)
    EXPECT_DOUBLE_EQ(get_open_notional(), 0.0) << "OPEN should be exactly 0 (no drift!)";
}

TEST_F(NotionalDriftTest, SpotPriceChangeBetweenAckAndCancel) {
    // Step 1: Insert and ACK order at spot=$100
    auto order = create_order("ORD001", "AAPL", Side::BID, 100.0, 10);
    auto inst = get_instrument("AAPL");
    engine->on_new_order_single(order, inst);
    engine->on_execution_report(create_ack("ORD001", 10), inst);

    EXPECT_DOUBLE_EQ(get_open_notional(), 1000.0);

    // Step 2: Spot moves to $150 BEFORE cancel
    provider.update_spot_price("AAPL", 150.0);
    auto inst2 = get_instrument("AAPL");

    // Cancel request and ack
    engine->on_order_cancel_request(create_cancel_request("CXL001", "ORD001", "AAPL", Side::BID), inst2);
    engine->on_execution_report(create_cancel_ack("CXL001", "ORD001"), inst2);

    // CRITICAL: OPEN should be exactly 0 (no drift!)
    EXPECT_DOUBLE_EQ(get_open_notional(), 0.0) << "OPEN should be exactly 0 (no drift!)";
}

TEST_F(NotionalDriftTest, MultipleOrdersWithSpotChanges) {
    // Insert multiple orders at different spots
    auto order1 = create_order("ORD001", "AAPL", Side::BID, 100.0, 10);
    auto inst1 = get_instrument("AAPL");
    engine->on_new_order_single(order1, inst1);
    engine->on_execution_report(create_ack("ORD001", 10), inst1);
    // OPEN = 10 * 100 = 1000

    // Spot changes before second order
    provider.update_spot_price("AAPL", 150.0);
    auto inst2 = get_instrument("AAPL");

    auto order2 = create_order("ORD002", "AAPL", Side::BID, 150.0, 20);
    engine->on_new_order_single(order2, inst2);
    engine->on_execution_report(create_ack("ORD002", 20), inst2);
    // OPEN = 1000 + 20 * 150 = 1000 + 3000 = 4000

    EXPECT_DOUBLE_EQ(get_open_notional(), 4000.0);

    // Spot changes before canceling first order
    provider.update_spot_price("AAPL", 200.0);
    auto inst3 = get_instrument("AAPL");

    // Cancel first order - should remove exactly 1000 (stored at spot=$100)
    engine->on_order_cancel_request(create_cancel_request("CXL001", "ORD001", "AAPL", Side::BID), inst3);
    engine->on_execution_report(create_cancel_ack("CXL001", "ORD001"), inst3);

    // OPEN = 4000 - 1000 = 3000 (the stored notional for order2)
    EXPECT_DOUBLE_EQ(get_open_notional(), 3000.0) << "OPEN = 3000 (order2 only, stored at spot=$150)";

    // Cancel second order - should remove exactly 3000 (stored at spot=$150)
    engine->on_order_cancel_request(create_cancel_request("CXL002", "ORD002", "AAPL", Side::BID), inst3);
    engine->on_execution_report(create_cancel_ack("CXL002", "ORD002"), inst3);

    EXPECT_DOUBLE_EQ(get_open_notional(), 0.0) << "OPEN should be exactly 0 (no drift!)";
}

TEST_F(NotionalDriftTest, PartialFillWithSpotChange) {
    // Insert and ACK order at spot=$100
    auto order = create_order("ORD001", "AAPL", Side::BID, 100.0, 10);
    auto inst = get_instrument("AAPL");
    engine->on_new_order_single(order, inst);
    engine->on_execution_report(create_ack("ORD001", 10), inst);

    EXPECT_DOUBLE_EQ(get_open_notional(), 1000.0);

    // Spot changes before partial fill
    provider.update_spot_price("AAPL", 120.0);
    auto inst2 = get_instrument("AAPL");

    // Partial fill of 4 shares
    // Remove 4 shares from OPEN using stored inputs: 4 * 100 = 400
    // Remaining: 6 * 100 = 600
    engine->on_execution_report(create_fill("ORD001", 4, 6, 120.0), inst2);

    EXPECT_DOUBLE_EQ(get_open_notional(), 600.0) << "OPEN = 6 * 100 = 600 (stored spot)";

    // Spot changes again
    provider.update_spot_price("AAPL", 150.0);
    auto inst3 = get_instrument("AAPL");

    // Full fill of remaining 6 shares
    // Remove 6 shares from OPEN using stored inputs: 6 * 100 = 600
    engine->on_execution_report(create_fill("ORD001", 6, 0, 150.0), inst3);

    EXPECT_DOUBLE_EQ(get_open_notional(), 0.0) << "OPEN should be exactly 0 (no drift!)";
}

// ============================================================================
// Test: Delta Drift-Free with Underlyer Spot Changes
// ============================================================================

class DeltaDriftTest : public ::testing::Test {
protected:
    using GlobalDelta = GlobalGrossDeltaMetric<DriftTestContext, InstrumentData, OpenStage, InFlightStage>;

    using TestEngine = RiskAggregationEngineWithLimits<
        DriftTestContext,
        InstrumentData,
        GlobalDelta
    >;

    StaticInstrumentProvider provider;
    std::unique_ptr<DriftTestContext> context;
    std::unique_ptr<TestEngine> engine;

    void SetUp() override {
        // Add an option with delta=0.5
        provider.add_option("AAPL_C100", "AAPL", 10.0, 100.0, 0.5, 100.0);
        context = std::make_unique<DriftTestContext>(provider);
        engine = std::make_unique<TestEngine>(*context);
    }

    double get_in_flight_delta() const {
        return engine->get_metric<GlobalDelta>().get_in_flight(GlobalKey::instance());
    }

    double get_open_delta() const {
        return engine->get_metric<GlobalDelta>().get_open(GlobalKey::instance());
    }

    InstrumentData get_instrument(const std::string& symbol) const {
        return provider.get_instrument(symbol);
    }
};

TEST_F(DeltaDriftTest, UnderlyerSpotChangeBetweenInsertAndAck) {
    // Insert order at underlyer_spot=$100, delta=0.5, qty=10, contract=100
    // Delta exposure = 10 * 0.5 * 100 * 100 * 1 = 50000
    auto order = create_order("ORD001", "AAPL_C100", Side::BID, 10.0, 10);
    order.underlyer = "AAPL";
    auto inst = get_instrument("AAPL_C100");
    engine->on_new_order_single(order, inst);

    EXPECT_DOUBLE_EQ(get_in_flight_delta(), 50000.0) << "IN_FLIGHT = 10 * 0.5 * 100 * 100 = 50000";

    // Underlyer spot moves to $120 BEFORE ack
    provider.update_underlyer_spot("AAPL", 120.0);
    auto inst2 = get_instrument("AAPL_C100");

    // ACK order
    engine->on_execution_report(create_ack("ORD001", 10), inst2);

    // IN_FLIGHT removed using stored underlyer_spot=$100, OPEN added at $120
    EXPECT_DOUBLE_EQ(get_in_flight_delta(), 0.0) << "IN_FLIGHT should be exactly 0 (no drift!)";
    EXPECT_DOUBLE_EQ(get_open_delta(), 60000.0) << "OPEN = 10 * 0.5 * 100 * 120 = 60000";
}

TEST_F(DeltaDriftTest, DeltaChangeDoesNotAffectStoredValues) {
    // Insert and ACK order at delta=0.5
    auto order = create_order("ORD001", "AAPL_C100", Side::BID, 10.0, 10);
    order.underlyer = "AAPL";
    auto inst = get_instrument("AAPL_C100");
    engine->on_new_order_single(order, inst);
    engine->on_execution_report(create_ack("ORD001", 10), inst);

    // Delta exposure = 10 * 0.5 * 100 * 100 = 50000
    EXPECT_DOUBLE_EQ(get_open_delta(), 50000.0);

    // Delta changes to 0.6 (price moved ITM)
    provider.update_delta("AAPL_C100", 0.6);
    auto inst2 = get_instrument("AAPL_C100");

    // Cancel - should remove exactly 50000 (stored delta=0.5)
    engine->on_order_cancel_request(create_cancel_request("CXL001", "ORD001", "AAPL_C100", Side::BID), inst2);
    engine->on_execution_report(create_cancel_ack("CXL001", "ORD001"), inst2);

    EXPECT_DOUBLE_EQ(get_open_delta(), 0.0) << "OPEN should be exactly 0 (no drift!)";
}
