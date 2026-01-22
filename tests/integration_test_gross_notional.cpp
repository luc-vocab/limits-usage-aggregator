#include <gtest/gtest.h>
#include "../src/engine/risk_engine_with_limits.hpp"
#include "../src/metrics/notional_metric.hpp"
#include "../src/instrument/instrument.hpp"
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

// Create provider for stocks
SimpleInstrumentProvider create_stock_provider() {
    SimpleInstrumentProvider provider;
    // Equities: contract_size=1, fx_rate=1
    provider.set_spot_price("AAPL", 150.0);
    provider.set_spot_price("MSFT", 300.0);
    provider.set_spot_price("GOOG", 100.0);
    provider.set_spot_price("TSLA", 200.0);
    return provider;
}

}  // namespace

// ============================================================================
// Test: Gross Open Order Notional
// ============================================================================
//
// This test verifies that we track global gross open order notional correctly.
// "Gross" means we sum the absolute values of all open order notionals.
// For equities, notional = quantity * spot_price (contract_size=1, fx_rate=1).
//
// Metrics used:
//   - GlobalNotional: NotionalMetric<GlobalKey, Provider, OpenStage, InFlightStage>
//

class GrossOpenNotionalTest : public ::testing::Test {
protected:
    using GlobalNotional = GlobalNotionalMetric<SimpleInstrumentProvider, OpenStage, InFlightStage>;

    using TestEngine = RiskAggregationEngineWithLimits<
        SimpleInstrumentProvider,
        GlobalNotional
    >;

    SimpleInstrumentProvider provider;
    TestEngine engine;

    // Limit for gross notional
    static constexpr double MAX_GROSS_NOTIONAL = 100000.0;

    void SetUp() override {
        provider = create_stock_provider();
        engine.set_instrument_provider(&provider);
        engine.set_global_notional_limit(MAX_GROSS_NOTIONAL);
    }

    double gross_notional() const {
        return engine.get_metric<GlobalNotional>().get(GlobalKey::instance());
    }

    // Compute notional for an order
    double compute_notional(const std::string& symbol, int64_t qty) const {
        // For equities: notional = qty * spot_price * contract_size * fx_rate
        // With defaults: contract_size=1, fx_rate=1
        return static_cast<double>(qty) * provider.get_spot_price(symbol);
    }
};

TEST_F(GrossOpenNotionalTest, SingleOrderLifecycle) {
    // Step 1: Send order (100 shares of AAPL at $150 = $15,000 notional)
    engine.on_new_order_single(create_order("ORD001", "AAPL", Side::BID, 150.0, 100));

    double expected_notional = 100 * 150.0;  // 15,000
    EXPECT_DOUBLE_EQ(gross_notional(), expected_notional) << "After INSERT";

    // Step 2: ACK order
    engine.on_execution_report(create_ack("ORD001", 100));
    EXPECT_DOUBLE_EQ(gross_notional(), expected_notional) << "After ACK (unchanged)";

    // Step 3: Partial fill (50 shares)
    engine.on_execution_report(create_fill("ORD001", 50, 50, 150.0));
    expected_notional = 50 * 150.0;  // 7,500 (50 shares remaining)
    EXPECT_DOUBLE_EQ(gross_notional(), expected_notional) << "After PARTIAL_FILL";

    // Step 4: Full fill
    engine.on_execution_report(create_fill("ORD001", 50, 0, 150.0));
    EXPECT_DOUBLE_EQ(gross_notional(), 0.0) << "After FULL_FILL";
}

TEST_F(GrossOpenNotionalTest, MultipleStocks) {
    // Send orders for multiple stocks
    // AAPL: 100 * $150 = $15,000
    engine.on_new_order_single(create_order("ORD001", "AAPL", Side::BID, 150.0, 100));
    EXPECT_DOUBLE_EQ(gross_notional(), 15000.0) << "After AAPL order";

    // MSFT: 50 * $300 = $15,000
    engine.on_new_order_single(create_order("ORD002", "MSFT", Side::BID, 300.0, 50));
    EXPECT_DOUBLE_EQ(gross_notional(), 30000.0) << "After MSFT order";

    // GOOG: 200 * $100 = $20,000
    engine.on_new_order_single(create_order("ORD003", "GOOG", Side::ASK, 100.0, 200));
    EXPECT_DOUBLE_EQ(gross_notional(), 50000.0) << "After GOOG order";

    // TSLA: 100 * $200 = $20,000
    engine.on_new_order_single(create_order("ORD004", "TSLA", Side::ASK, 200.0, 100));
    EXPECT_DOUBLE_EQ(gross_notional(), 70000.0) << "After TSLA order";

    // ACK all orders
    engine.on_execution_report(create_ack("ORD001", 100));
    engine.on_execution_report(create_ack("ORD002", 50));
    engine.on_execution_report(create_ack("ORD003", 200));
    engine.on_execution_report(create_ack("ORD004", 100));

    EXPECT_DOUBLE_EQ(gross_notional(), 70000.0) << "After all ACKs";
}

TEST_F(GrossOpenNotionalTest, BidAndAskBothCountTowardsGross) {
    // BID order: 100 * $150 = $15,000
    engine.on_new_order_single(create_order("ORD001", "AAPL", Side::BID, 150.0, 100));
    EXPECT_DOUBLE_EQ(gross_notional(), 15000.0);

    // ASK order: 100 * $150 = $15,000 (adds to gross, not subtracts)
    engine.on_new_order_single(create_order("ORD002", "AAPL", Side::ASK, 150.0, 100));
    EXPECT_DOUBLE_EQ(gross_notional(), 30000.0) << "Gross = |BID| + |ASK|";
}

TEST_F(GrossOpenNotionalTest, LimitEnforcement) {
    // Initial order: 100 * $150 = $15,000
    engine.on_new_order_single(create_order("ORD001", "AAPL", Side::BID, 150.0, 100));
    engine.on_execution_report(create_ack("ORD001", 100));

    // Pre-trade check for MSFT order (+$60,000 = $75,000 < $100,000)
    auto msft_order = create_order("ORD002", "MSFT", Side::BID, 300.0, 200);
    auto result1 = engine.pre_trade_check(msft_order);
    EXPECT_FALSE(result1.would_breach) << "Should not breach: 15000 + 60000 = 75000 < 100000";

    // Add more: 200 * $300 = $60,000 (total: $75,000)
    engine.on_new_order_single(msft_order);
    engine.on_execution_report(create_ack("ORD002", 200));

    EXPECT_DOUBLE_EQ(gross_notional(), 75000.0);

    // Pre-trade check for GOOG order (+$30,000 = $105,000 > $100,000)
    auto goog_order = create_order("ORD003", "GOOG", Side::BID, 100.0, 300);
    auto result2 = engine.pre_trade_check(goog_order);
    EXPECT_TRUE(result2.would_breach) << "Should breach: 75000 + 30000 = 105000 > 100000";
    EXPECT_TRUE(result2.has_breach(LimitType::GLOBAL_NOTIONAL));

    // Verify breach details
    const auto* breach = result2.get_breach(LimitType::GLOBAL_NOTIONAL);
    ASSERT_NE(breach, nullptr);
    EXPECT_DOUBLE_EQ(breach->current_usage, 75000.0);
    EXPECT_DOUBLE_EQ(breach->hypothetical_usage, 105000.0);
    EXPECT_DOUBLE_EQ(breach->limit_value, 100000.0);
}

TEST_F(GrossOpenNotionalTest, NackFreesNotional) {
    engine.on_new_order_single(create_order("ORD001", "AAPL", Side::BID, 150.0, 100));
    EXPECT_DOUBLE_EQ(gross_notional(), 15000.0);

    engine.on_execution_report(create_nack("ORD001"));
    EXPECT_DOUBLE_EQ(gross_notional(), 0.0) << "NACK should free notional";
}

TEST_F(GrossOpenNotionalTest, CancelFreesNotional) {
    engine.on_new_order_single(create_order("ORD001", "AAPL", Side::BID, 150.0, 100));
    engine.on_execution_report(create_ack("ORD001", 100));

    EXPECT_DOUBLE_EQ(gross_notional(), 15000.0);

    engine.on_order_cancel_request(create_cancel_request("CXL001", "ORD001", "AAPL", Side::BID));
    EXPECT_DOUBLE_EQ(gross_notional(), 15000.0) << "Pending cancel still counts";

    engine.on_execution_report(create_cancel_ack("CXL001", "ORD001"));
    EXPECT_DOUBLE_EQ(gross_notional(), 0.0) << "Cancel should free notional";
}

TEST_F(GrossOpenNotionalTest, FullFlowWithAssertions) {
    // Step 1: INSERT ORD001 (AAPL BID 100 @ $150)
    engine.on_new_order_single(create_order("ORD001", "AAPL", Side::BID, 150.0, 100));
    EXPECT_DOUBLE_EQ(gross_notional(), 15000.0) << "Step 1: After INSERT ORD001";

    // Step 2: INSERT ORD002 (MSFT ASK 50 @ $300) - short position
    engine.on_new_order_single(create_order("ORD002", "MSFT", Side::ASK, 300.0, 50));
    EXPECT_DOUBLE_EQ(gross_notional(), 30000.0) << "Step 2: After INSERT ORD002";

    // Step 3: ACK ORD001
    engine.on_execution_report(create_ack("ORD001", 100));
    EXPECT_DOUBLE_EQ(gross_notional(), 30000.0) << "Step 3: After ACK ORD001";

    // Step 4: ACK ORD002
    engine.on_execution_report(create_ack("ORD002", 50));
    EXPECT_DOUBLE_EQ(gross_notional(), 30000.0) << "Step 4: After ACK ORD002";

    // Step 5: INSERT ORD003 (GOOG BID 200 @ $100)
    engine.on_new_order_single(create_order("ORD003", "GOOG", Side::BID, 100.0, 200));
    EXPECT_DOUBLE_EQ(gross_notional(), 50000.0) << "Step 5: After INSERT ORD003";

    // Step 6: NACK ORD003
    engine.on_execution_report(create_nack("ORD003"));
    EXPECT_DOUBLE_EQ(gross_notional(), 30000.0) << "Step 6: After NACK ORD003";

    // Step 7: PARTIAL_FILL ORD001 (50 shares filled)
    engine.on_execution_report(create_fill("ORD001", 50, 50, 150.0));
    EXPECT_DOUBLE_EQ(gross_notional(), 22500.0) << "Step 7: After PARTIAL_FILL ORD001";
    // AAPL: 50 * 150 = 7500, MSFT: 50 * 300 = 15000, Total: 22500

    // Step 8: FULL_FILL ORD001
    engine.on_execution_report(create_fill("ORD001", 50, 0, 150.0));
    EXPECT_DOUBLE_EQ(gross_notional(), 15000.0) << "Step 8: After FULL_FILL ORD001";

    // Step 9: CANCEL ORD002
    engine.on_order_cancel_request(create_cancel_request("CXL001", "ORD002", "MSFT", Side::ASK));
    EXPECT_DOUBLE_EQ(gross_notional(), 15000.0) << "Step 9: After CANCEL_REQ ORD002";

    engine.on_execution_report(create_cancel_ack("CXL001", "ORD002"));
    EXPECT_DOUBLE_EQ(gross_notional(), 0.0) << "Step 9: After CANCEL_ACK ORD002";
}

TEST_F(GrossOpenNotionalTest, Clear) {
    engine.on_new_order_single(create_order("ORD001", "AAPL", Side::BID, 150.0, 100));
    engine.on_new_order_single(create_order("ORD002", "MSFT", Side::ASK, 300.0, 50));

    EXPECT_DOUBLE_EQ(gross_notional(), 30000.0);

    engine.clear();

    EXPECT_DOUBLE_EQ(gross_notional(), 0.0);
}

TEST_F(GrossOpenNotionalTest, PreTradeCheckResultToString) {
    // Fill up to near the limit: $75,000
    engine.on_new_order_single(create_order("ORD001", "AAPL", Side::BID, 150.0, 500));  // $75,000
    engine.on_execution_report(create_ack("ORD001", 500));

    // Check pre-trade for order that would breach
    auto order = create_order("ORD002", "MSFT", Side::BID, 300.0, 100);  // $30,000
    auto result = engine.pre_trade_check(order);

    EXPECT_TRUE(result.would_breach);
    EXPECT_EQ(result.breaches.size(), 1u);

    // Verify to_string() contains expected information
    std::string result_str = result.to_string();
    EXPECT_NE(result_str.find("GLOBAL_NOTIONAL"), std::string::npos);
    EXPECT_NE(result_str.find("FAILED"), std::string::npos);
}
