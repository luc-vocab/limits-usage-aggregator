#include <gtest/gtest.h>
#include "../src/engine/risk_engine_with_limits.hpp"
#include "../src/metrics/notional_metric.hpp"
#include "../src/instrument/instrument.hpp"
#include "../src/fix/fix_parser.hpp"
#include <memory>

using namespace engine;
using namespace fix;
using namespace aggregation;
using namespace metrics;
using namespace instrument;

// ============================================================================
// Test Context
// ============================================================================

class TestContext {
    const SimpleInstrumentProvider& provider_;
public:
    explicit TestContext(const SimpleInstrumentProvider& provider) : provider_(provider) {}

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
                             const std::string& portfolio = "PORT1",
                             const std::string& strategy = "STRAT1") {
    NewOrderSingle order;
    order.key.cl_ord_id = cl_ord_id;
    order.symbol = symbol;
    order.underlyer = symbol;  // Equities: underlyer = symbol
    order.side = side;
    order.price = price;
    order.quantity = qty;
    order.strategy_id = strategy;
    order.portfolio_id = portfolio;
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
// Test: Per-Portfolio, Per-Symbol Net Notional
// ============================================================================
//
// This test verifies that we track net notional at the (portfolio_id, symbol)
// level correctly across all order stages (IN_FLIGHT, OPEN, POSITION).
//
// Net notional calculation:
//   notional = qty * contract_size * spot_price * fx_rate
//   BID = +notional (long exposure)
//   ASK = -notional (short exposure)
//
// Metrics used:
//   - NetNotionalMetric<PortfolioInstrumentKey, InstrumentData, PositionStage, OpenStage, InFlightStage>
//

class PortfolioInstrumentNetNotionalTest : public ::testing::Test {
protected:
    using PortfolioInstrumentNetNotional = NetNotionalMetric<
        PortfolioInstrumentKey,
        TestContext,
        InstrumentData,
        PositionStage, OpenStage, InFlightStage
    >;

    using TestEngine = RiskAggregationEngineWithLimits<
        TestContext,
        InstrumentData,
        PortfolioInstrumentNetNotional
    >;

    SimpleInstrumentProvider provider;
    std::unique_ptr<TestContext> context;
    std::unique_ptr<TestEngine> engine;

    void SetUp() override {
        provider = create_stock_provider();
        context = std::make_unique<TestContext>(provider);
        engine = std::make_unique<TestEngine>(*context);
    }

    // Helper to get instrument from provider
    InstrumentData get_instrument(const std::string& symbol) const {
        return provider.get_instrument(symbol);
    }

    // Accessor for net notional at (portfolio, symbol)
    double net_notional(const std::string& portfolio, const std::string& symbol) const {
        return engine->get_metric<PortfolioInstrumentNetNotional>().get(
            PortfolioInstrumentKey{portfolio, symbol});
    }

    // Accessor for in-flight notional at (portfolio, symbol)
    double in_flight_notional(const std::string& portfolio, const std::string& symbol) const {
        return engine->get_metric<PortfolioInstrumentNetNotional>().get_in_flight(
            PortfolioInstrumentKey{portfolio, symbol});
    }

    // Accessor for open notional at (portfolio, symbol)
    double open_notional(const std::string& portfolio, const std::string& symbol) const {
        return engine->get_metric<PortfolioInstrumentNetNotional>().get_open(
            PortfolioInstrumentKey{portfolio, symbol});
    }

    // Accessor for position notional at (portfolio, symbol)
    double position_notional(const std::string& portfolio, const std::string& symbol) const {
        return engine->get_metric<PortfolioInstrumentNetNotional>().get_position(
            PortfolioInstrumentKey{portfolio, symbol});
    }

    // Compute expected notional (equities: notional = qty * spot_price)
    double compute_notional(const std::string& symbol, int64_t qty) const {
        return static_cast<double>(qty) * provider.get_spot_price(symbol);
    }
};

// ============================================================================
// Test: SingleOrderFullLifecycle
// ============================================================================
// INSERT->ACK->PARTIAL_FILL->FULL_FILL tracking net notional at each stage

TEST_F(PortfolioInstrumentNetNotionalTest, SingleOrderFullLifecycle) {
    const std::string PORTFOLIO = "PORT1";
    const std::string SYMBOL = "AAPL";
    const double SPOT = 150.0;
    const int64_t QTY = 100;
    const double EXPECTED_NOTIONAL = QTY * SPOT;  // 15,000

    auto inst = get_instrument(SYMBOL);

    // Step 1: INSERT (BID) - should be in IN_FLIGHT stage
    engine->on_new_order_single(create_order("ORD001", SYMBOL, Side::BID, SPOT, QTY, PORTFOLIO), inst);

    EXPECT_DOUBLE_EQ(in_flight_notional(PORTFOLIO, SYMBOL), EXPECTED_NOTIONAL)
        << "After INSERT: notional should be in IN_FLIGHT";
    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), 0.0)
        << "After INSERT: OPEN should be 0";
    EXPECT_DOUBLE_EQ(position_notional(PORTFOLIO, SYMBOL), 0.0)
        << "After INSERT: POSITION should be 0";
    EXPECT_DOUBLE_EQ(net_notional(PORTFOLIO, SYMBOL), EXPECTED_NOTIONAL)
        << "After INSERT: total net notional";

    // Step 2: ACK - moves from IN_FLIGHT to OPEN
    engine->on_execution_report(create_ack("ORD001", QTY), inst);

    EXPECT_DOUBLE_EQ(in_flight_notional(PORTFOLIO, SYMBOL), 0.0)
        << "After ACK: IN_FLIGHT should be 0";
    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), EXPECTED_NOTIONAL)
        << "After ACK: notional should be in OPEN";
    EXPECT_DOUBLE_EQ(position_notional(PORTFOLIO, SYMBOL), 0.0)
        << "After ACK: POSITION should be 0";
    EXPECT_DOUBLE_EQ(net_notional(PORTFOLIO, SYMBOL), EXPECTED_NOTIONAL)
        << "After ACK: total net notional unchanged";

    // Step 3: PARTIAL_FILL (50 shares) - moves partial from OPEN to POSITION
    const int64_t FILL1_QTY = 50;
    engine->on_execution_report(create_fill("ORD001", FILL1_QTY, QTY - FILL1_QTY, SPOT), inst);

    double remaining_notional = (QTY - FILL1_QTY) * SPOT;  // 7,500
    double filled_notional = FILL1_QTY * SPOT;             // 7,500

    EXPECT_DOUBLE_EQ(in_flight_notional(PORTFOLIO, SYMBOL), 0.0)
        << "After PARTIAL_FILL: IN_FLIGHT should be 0";
    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), remaining_notional)
        << "After PARTIAL_FILL: remaining qty in OPEN";
    EXPECT_DOUBLE_EQ(position_notional(PORTFOLIO, SYMBOL), filled_notional)
        << "After PARTIAL_FILL: filled qty in POSITION";
    EXPECT_DOUBLE_EQ(net_notional(PORTFOLIO, SYMBOL), EXPECTED_NOTIONAL)
        << "After PARTIAL_FILL: total net notional unchanged";

    // Step 4: FULL_FILL (remaining 50 shares) - all goes to POSITION
    engine->on_execution_report(create_fill("ORD001", FILL1_QTY, 0, SPOT), inst);

    EXPECT_DOUBLE_EQ(in_flight_notional(PORTFOLIO, SYMBOL), 0.0)
        << "After FULL_FILL: IN_FLIGHT should be 0";
    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), 0.0)
        << "After FULL_FILL: OPEN should be 0";
    EXPECT_DOUBLE_EQ(position_notional(PORTFOLIO, SYMBOL), EXPECTED_NOTIONAL)
        << "After FULL_FILL: all notional in POSITION";
    EXPECT_DOUBLE_EQ(net_notional(PORTFOLIO, SYMBOL), EXPECTED_NOTIONAL)
        << "After FULL_FILL: total net notional unchanged";
}

// ============================================================================
// Test: MultipleOrdersDifferentPortfolios
// ============================================================================
// Orders in PORT1/AAPL vs PORT2/AAPL tracked separately

TEST_F(PortfolioInstrumentNetNotionalTest, MultipleOrdersDifferentPortfolios) {
    const std::string SYMBOL = "AAPL";
    const double SPOT = 150.0;
    auto inst = get_instrument(SYMBOL);

    // Portfolio 1: 100 shares = $15,000
    engine->on_new_order_single(create_order("ORD001", SYMBOL, Side::BID, SPOT, 100, "PORT1"), inst);
    engine->on_execution_report(create_ack("ORD001", 100), inst);

    // Portfolio 2: 200 shares = $30,000
    engine->on_new_order_single(create_order("ORD002", SYMBOL, Side::BID, SPOT, 200, "PORT2"), inst);
    engine->on_execution_report(create_ack("ORD002", 200), inst);

    // Verify separate tracking
    EXPECT_DOUBLE_EQ(open_notional("PORT1", SYMBOL), 15000.0)
        << "PORT1/AAPL should have $15,000";
    EXPECT_DOUBLE_EQ(open_notional("PORT2", SYMBOL), 30000.0)
        << "PORT2/AAPL should have $30,000";

    // Fill PORT1 order - should not affect PORT2
    engine->on_execution_report(create_fill("ORD001", 100, 0, SPOT), inst);

    EXPECT_DOUBLE_EQ(open_notional("PORT1", SYMBOL), 0.0)
        << "PORT1/AAPL OPEN should be 0 after fill";
    EXPECT_DOUBLE_EQ(position_notional("PORT1", SYMBOL), 15000.0)
        << "PORT1/AAPL POSITION should be $15,000";
    EXPECT_DOUBLE_EQ(open_notional("PORT2", SYMBOL), 30000.0)
        << "PORT2/AAPL should be unchanged";
}

// ============================================================================
// Test: MultipleOrdersSamePortfolioDifferentSymbols
// ============================================================================
// Orders in PORT1/AAPL vs PORT1/MSFT tracked separately

TEST_F(PortfolioInstrumentNetNotionalTest, MultipleOrdersSamePortfolioDifferentSymbols) {
    const std::string PORTFOLIO = "PORT1";

    // AAPL: 100 * $150 = $15,000
    auto aapl_inst = get_instrument("AAPL");
    engine->on_new_order_single(create_order("ORD001", "AAPL", Side::BID, 150.0, 100, PORTFOLIO), aapl_inst);
    engine->on_execution_report(create_ack("ORD001", 100), aapl_inst);

    // MSFT: 50 * $300 = $15,000
    auto msft_inst = get_instrument("MSFT");
    engine->on_new_order_single(create_order("ORD002", "MSFT", Side::BID, 300.0, 50, PORTFOLIO), msft_inst);
    engine->on_execution_report(create_ack("ORD002", 50), msft_inst);

    // Verify separate tracking
    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, "AAPL"), 15000.0)
        << "PORT1/AAPL should have $15,000";
    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, "MSFT"), 15000.0)
        << "PORT1/MSFT should have $15,000";

    // Cancel AAPL order - should not affect MSFT
    engine->on_order_cancel_request(create_cancel_request("CXL001", "ORD001", "AAPL", Side::BID), aapl_inst);
    engine->on_execution_report(create_cancel_ack("CXL001", "ORD001"), aapl_inst);

    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, "AAPL"), 0.0)
        << "PORT1/AAPL should be 0 after cancel";
    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, "MSFT"), 15000.0)
        << "PORT1/MSFT should be unchanged";
}

// ============================================================================
// Test: NetNotionalDirectional
// ============================================================================
// BID adds positive notional, ASK adds negative (can offset)

TEST_F(PortfolioInstrumentNetNotionalTest, NetNotionalDirectional) {
    const std::string PORTFOLIO = "PORT1";
    const std::string SYMBOL = "AAPL";
    const double SPOT = 150.0;
    auto inst = get_instrument(SYMBOL);

    // BID 100 shares = +$15,000
    engine->on_new_order_single(create_order("ORD001", SYMBOL, Side::BID, SPOT, 100, PORTFOLIO), inst);
    engine->on_execution_report(create_ack("ORD001", 100), inst);

    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), 15000.0)
        << "BID should add positive notional";

    // ASK 100 shares = -$15,000 (offsets the BID)
    engine->on_new_order_single(create_order("ORD002", SYMBOL, Side::ASK, SPOT, 100, PORTFOLIO), inst);
    engine->on_execution_report(create_ack("ORD002", 100), inst);

    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), 0.0)
        << "ASK should offset BID, net = 0";

    // ASK another 50 shares = -$7,500 (net negative)
    engine->on_new_order_single(create_order("ORD003", SYMBOL, Side::ASK, SPOT, 50, PORTFOLIO), inst);
    engine->on_execution_report(create_ack("ORD003", 50), inst);

    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), -7500.0)
        << "Net should be negative (short exposure)";
}

// ============================================================================
// Test: CancelFreesNotional
// ============================================================================
// Cancel removes notional from OPEN stage

TEST_F(PortfolioInstrumentNetNotionalTest, CancelFreesNotional) {
    const std::string PORTFOLIO = "PORT1";
    const std::string SYMBOL = "AAPL";
    auto inst = get_instrument(SYMBOL);

    // Insert and ACK order
    engine->on_new_order_single(create_order("ORD001", SYMBOL, Side::BID, 150.0, 100, PORTFOLIO), inst);
    engine->on_execution_report(create_ack("ORD001", 100), inst);

    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), 15000.0);

    // Send cancel request - order moves to PENDING_CANCEL (still in OPEN stage)
    engine->on_order_cancel_request(create_cancel_request("CXL001", "ORD001", SYMBOL, Side::BID), inst);

    // Still counts until cancel is acknowledged
    EXPECT_DOUBLE_EQ(net_notional(PORTFOLIO, SYMBOL), 15000.0)
        << "Pending cancel still counts";

    // Cancel ACK - notional freed
    engine->on_execution_report(create_cancel_ack("CXL001", "ORD001"), inst);

    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), 0.0)
        << "Cancel should free OPEN notional";
    EXPECT_DOUBLE_EQ(net_notional(PORTFOLIO, SYMBOL), 0.0)
        << "Total net notional should be 0";
}

// ============================================================================
// Test: NackFreesNotional
// ============================================================================
// Rejection removes notional from IN_FLIGHT stage

TEST_F(PortfolioInstrumentNetNotionalTest, NackFreesNotional) {
    const std::string PORTFOLIO = "PORT1";
    const std::string SYMBOL = "AAPL";
    auto inst = get_instrument(SYMBOL);

    // Insert order - goes to IN_FLIGHT
    engine->on_new_order_single(create_order("ORD001", SYMBOL, Side::BID, 150.0, 100, PORTFOLIO), inst);

    EXPECT_DOUBLE_EQ(in_flight_notional(PORTFOLIO, SYMBOL), 15000.0)
        << "After INSERT: notional in IN_FLIGHT";
    EXPECT_DOUBLE_EQ(net_notional(PORTFOLIO, SYMBOL), 15000.0)
        << "Total net notional";

    // NACK - notional freed
    engine->on_execution_report(create_nack("ORD001"), inst);

    EXPECT_DOUBLE_EQ(in_flight_notional(PORTFOLIO, SYMBOL), 0.0)
        << "NACK should free IN_FLIGHT notional";
    EXPECT_DOUBLE_EQ(net_notional(PORTFOLIO, SYMBOL), 0.0)
        << "Total net notional should be 0";
}

// ============================================================================
// Test: CombinedFlowAllStages
// ============================================================================
// Complex flow exercising all stages and transitions

TEST_F(PortfolioInstrumentNetNotionalTest, CombinedFlowAllStages) {
    const std::string PORTFOLIO = "PORT1";
    const std::string SYMBOL = "AAPL";
    const double SPOT = 150.0;
    auto inst = get_instrument(SYMBOL);

    // Step 1: Two BID orders inserted
    engine->on_new_order_single(create_order("ORD001", SYMBOL, Side::BID, SPOT, 100, PORTFOLIO), inst);
    engine->on_new_order_single(create_order("ORD002", SYMBOL, Side::BID, SPOT, 200, PORTFOLIO), inst);

    EXPECT_DOUBLE_EQ(in_flight_notional(PORTFOLIO, SYMBOL), 45000.0)
        << "Two orders in flight: 15K + 30K";

    // Step 2: ACK ORD001
    engine->on_execution_report(create_ack("ORD001", 100), inst);

    EXPECT_DOUBLE_EQ(in_flight_notional(PORTFOLIO, SYMBOL), 30000.0)
        << "ORD002 still in flight";
    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), 15000.0)
        << "ORD001 now open";

    // Step 3: NACK ORD002
    engine->on_execution_report(create_nack("ORD002"), inst);

    EXPECT_DOUBLE_EQ(in_flight_notional(PORTFOLIO, SYMBOL), 0.0)
        << "ORD002 nacked";
    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), 15000.0)
        << "Only ORD001 remains open";

    // Step 4: Partial fill ORD001 (60 shares)
    engine->on_execution_report(create_fill("ORD001", 60, 40, SPOT), inst);

    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), 6000.0)
        << "40 shares remain open: 40 * 150";
    EXPECT_DOUBLE_EQ(position_notional(PORTFOLIO, SYMBOL), 9000.0)
        << "60 shares filled: 60 * 150";

    // Step 5: Insert ASK order (short 50 shares)
    engine->on_new_order_single(create_order("ORD003", SYMBOL, Side::ASK, SPOT, 50, PORTFOLIO), inst);
    engine->on_execution_report(create_ack("ORD003", 50), inst);

    // Net open = 40 * 150 - 50 * 150 = -1500
    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), -1500.0)
        << "40 long - 50 short = -10 shares net";

    // Step 6: Fill remaining ORD001 (40 shares)
    engine->on_execution_report(create_fill("ORD001", 40, 0, SPOT), inst);

    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), -7500.0)
        << "Only ASK order remains: -50 * 150";
    EXPECT_DOUBLE_EQ(position_notional(PORTFOLIO, SYMBOL), 15000.0)
        << "Full ORD001 in position: 100 * 150";

    // Net total = position (15K long) + open (-7.5K short) = 7.5K
    EXPECT_DOUBLE_EQ(net_notional(PORTFOLIO, SYMBOL), 7500.0)
        << "Net = position + open = 15000 - 7500";
}

// ============================================================================
// Test: Clear
// ============================================================================
// Verify clear() resets all stages

TEST_F(PortfolioInstrumentNetNotionalTest, Clear) {
    const std::string PORTFOLIO = "PORT1";
    const std::string SYMBOL = "AAPL";
    auto inst = get_instrument(SYMBOL);

    // Build up some state
    engine->on_new_order_single(create_order("ORD001", SYMBOL, Side::BID, 150.0, 100, PORTFOLIO), inst);
    engine->on_execution_report(create_ack("ORD001", 100), inst);
    engine->on_execution_report(create_fill("ORD001", 50, 50, 150.0), inst);

    EXPECT_NE(net_notional(PORTFOLIO, SYMBOL), 0.0);

    // Clear
    engine->clear();

    EXPECT_DOUBLE_EQ(in_flight_notional(PORTFOLIO, SYMBOL), 0.0);
    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), 0.0);
    EXPECT_DOUBLE_EQ(position_notional(PORTFOLIO, SYMBOL), 0.0);
    EXPECT_DOUBLE_EQ(net_notional(PORTFOLIO, SYMBOL), 0.0);
}

// ============================================================================
// Test: PreTradeCheckPositiveNetNotionalBreach
// ============================================================================
// Verify pre_trade_check blocks when positive net notional (long exposure)
// would exceed the limit

TEST_F(PortfolioInstrumentNetNotionalTest, PreTradeCheckPositiveNetNotionalBreach) {
    const std::string PORTFOLIO = "PORT1";
    const std::string SYMBOL = "AAPL";
    const double SPOT = 150.0;
    const double LIMIT = 20000.0;  // Max $20,000 net notional (either direction)
    auto inst = get_instrument(SYMBOL);

    // Set limit (ABSOLUTE mode is default, checks both positive and negative breaches)
    PortfolioInstrumentKey key{PORTFOLIO, SYMBOL};
    engine->set_limit<PortfolioInstrumentNetNotional>(key, LIMIT);

    // First order: BID 100 shares = +$15,000 (within limit)
    auto order1 = create_order("ORD001", SYMBOL, Side::BID, SPOT, 100, PORTFOLIO);
    auto check1 = engine->pre_trade_check(order1, inst);
    EXPECT_FALSE(check1.would_breach)
        << "First order should pass: 15000 < 20000";

    engine->on_new_order_single(order1, inst);
    engine->on_execution_report(create_ack("ORD001", 100), inst);
    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), 15000.0);

    // Second order: BID 50 shares = +$7,500
    // Hypothetical: 15000 + 7500 = 22500 > 20000 -> BREACH
    auto order2 = create_order("ORD002", SYMBOL, Side::BID, SPOT, 50, PORTFOLIO);
    auto check2 = engine->pre_trade_check(order2, inst);
    EXPECT_TRUE(check2.would_breach)
        << "Second order should breach: 15000 + 7500 = 22500 > 20000";
    EXPECT_EQ(check2.breaches.size(), 1u);

    // Verify breach details
    const auto& breach = check2.breaches[0];
    EXPECT_DOUBLE_EQ(breach.current_usage, 15000.0);
    EXPECT_DOUBLE_EQ(breach.hypothetical_usage, 22500.0);
    EXPECT_DOUBLE_EQ(breach.limit_value, LIMIT);

    // Third order: BID 30 shares = +$4,500
    // Hypothetical: 15000 + 4500 = 19500 < 20000 -> OK
    auto order3 = create_order("ORD003", SYMBOL, Side::BID, SPOT, 30, PORTFOLIO);
    auto check3 = engine->pre_trade_check(order3, inst);
    EXPECT_FALSE(check3.would_breach)
        << "Third order should pass: 15000 + 4500 = 19500 < 20000";
}

// ============================================================================
// Test: PreTradeCheckNegativeNetNotionalBreach
// ============================================================================
// Verify pre_trade_check blocks when negative net notional (short exposure)
// would exceed the limit (in absolute value)

TEST_F(PortfolioInstrumentNetNotionalTest, PreTradeCheckNegativeNetNotionalBreach) {
    const std::string PORTFOLIO = "PORT1";
    const std::string SYMBOL = "AAPL";
    const double SPOT = 150.0;
    const double LIMIT = 20000.0;  // Max $20,000 net notional (either direction)
    auto inst = get_instrument(SYMBOL);

    // Set limit (ABSOLUTE mode is default, checks both positive and negative breaches)
    PortfolioInstrumentKey key{PORTFOLIO, SYMBOL};
    engine->set_limit<PortfolioInstrumentNetNotional>(key, LIMIT);

    // First order: ASK 100 shares = -$15,000 (within limit, |-15000| < 20000)
    auto order1 = create_order("ORD001", SYMBOL, Side::ASK, SPOT, 100, PORTFOLIO);
    auto check1 = engine->pre_trade_check(order1, inst);
    EXPECT_FALSE(check1.would_breach)
        << "First order should pass: |-15000| = 15000 < 20000";

    engine->on_new_order_single(order1, inst);
    engine->on_execution_report(create_ack("ORD001", 100), inst);
    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), -15000.0);

    // Second order: ASK 50 shares = -$7,500
    // Hypothetical: -15000 + (-7500) = -22500, |-22500| = 22500 > 20000 -> BREACH
    auto order2 = create_order("ORD002", SYMBOL, Side::ASK, SPOT, 50, PORTFOLIO);
    auto check2 = engine->pre_trade_check(order2, inst);
    EXPECT_TRUE(check2.would_breach)
        << "Second order should breach: |-15000 - 7500| = 22500 > 20000";
    EXPECT_EQ(check2.breaches.size(), 1u);

    // Verify breach details
    const auto& breach = check2.breaches[0];
    EXPECT_DOUBLE_EQ(breach.current_usage, -15000.0);
    EXPECT_DOUBLE_EQ(breach.hypothetical_usage, -22500.0);
    EXPECT_DOUBLE_EQ(breach.limit_value, LIMIT);

    // Third order: ASK 30 shares = -$4,500
    // Hypothetical: -15000 + (-4500) = -19500, |-19500| = 19500 < 20000 -> OK
    auto order3 = create_order("ORD003", SYMBOL, Side::ASK, SPOT, 30, PORTFOLIO);
    auto check3 = engine->pre_trade_check(order3, inst);
    EXPECT_FALSE(check3.would_breach)
        << "Third order should pass: |-15000 - 4500| = 19500 < 20000";
}

// ============================================================================
// Test: PreTradeCheckMixedDirectionsWithLimit
// ============================================================================
// Verify that offsetting positions (BID reduces short, ASK reduces long)
// can bring net notional back within limits

TEST_F(PortfolioInstrumentNetNotionalTest, PreTradeCheckMixedDirectionsWithLimit) {
    const std::string PORTFOLIO = "PORT1";
    const std::string SYMBOL = "AAPL";
    const double SPOT = 150.0;
    const double LIMIT = 20000.0;
    auto inst = get_instrument(SYMBOL);

    PortfolioInstrumentKey key{PORTFOLIO, SYMBOL};
    engine->set_limit<PortfolioInstrumentNetNotional>(key, LIMIT);

    // Build up short position: ASK 150 shares = -$22,500 (breach)
    // But we'll do it in parts that don't breach individually

    // ASK 100 shares = -$15,000
    engine->on_new_order_single(create_order("ORD001", SYMBOL, Side::ASK, SPOT, 100, PORTFOLIO), inst);
    engine->on_execution_report(create_ack("ORD001", 100), inst);
    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), -15000.0);

    // Try ASK 50 more = -$7,500 additional -> would breach
    auto ask_order = create_order("ORD002", SYMBOL, Side::ASK, SPOT, 50, PORTFOLIO);
    auto check_ask = engine->pre_trade_check(ask_order, inst);
    EXPECT_TRUE(check_ask.would_breach)
        << "Additional short should breach: |-15000 - 7500| = 22500 > 20000";

    // Instead, try BID 50 = +$7,500 (reduces short exposure)
    // Hypothetical: -15000 + 7500 = -7500, |-7500| = 7500 < 20000 -> OK
    auto bid_order = create_order("ORD003", SYMBOL, Side::BID, SPOT, 50, PORTFOLIO);
    auto check_bid = engine->pre_trade_check(bid_order, inst);
    EXPECT_FALSE(check_bid.would_breach)
        << "BID order should pass: |-15000 + 7500| = 7500 < 20000";

    // Execute the BID to reduce short exposure
    engine->on_new_order_single(bid_order, inst);
    engine->on_execution_report(create_ack("ORD003", 50), inst);
    EXPECT_DOUBLE_EQ(open_notional(PORTFOLIO, SYMBOL), -7500.0);

    // Now we have more room - try ASK 80 shares = -$12,000
    // Hypothetical: -7500 + (-12000) = -19500, |-19500| = 19500 < 20000 -> OK
    auto ask_order2 = create_order("ORD004", SYMBOL, Side::ASK, SPOT, 80, PORTFOLIO);
    auto check_ask2 = engine->pre_trade_check(ask_order2, inst);
    EXPECT_FALSE(check_ask2.would_breach)
        << "Now more short is allowed: |-7500 - 12000| = 19500 < 20000";
}
