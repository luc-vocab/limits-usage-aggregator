#include <gtest/gtest.h>
#include "../src/engine/risk_engine_with_limits.hpp"
#include "../src/metrics/notional_metric.hpp"
#include "../src/instrument/instrument.hpp"
#include "../src/fix/fix_messages.hpp"

using namespace engine;
using namespace fix;
using namespace aggregation;
using namespace metrics;
using namespace instrument;

// ============================================================================
// Integration Test: Options Gross/Net Position Notional
// ============================================================================
//
// This test verifies global gross and net notional tracking at the position
// stage for options. Position stage tracks the notional of filled orders.
//
// Metrics used:
//   - GrossPositionNotional: sum of |notional| for all fills (position stage only)
//   - NetPositionNotional: signed notional (BID = +, ASK = -) for fills
//
// Notional computation: quantity * contract_size * spot_price * fx_rate
//

namespace {

// ============================================================================
// TestContext - Provides accessor methods for instrument data
// ============================================================================

class TestContext {
    const StaticInstrumentProvider& provider_;
public:
    explicit TestContext(const StaticInstrumentProvider& provider) : provider_(provider) {}

    double spot_price(const InstrumentData& inst) const { return inst.spot_price(); }
    double fx_rate(const InstrumentData& inst) const { return inst.fx_rate(); }
    double contract_size(const InstrumentData& inst) const { return inst.contract_size(); }
    const std::string& underlyer(const InstrumentData& inst) const { return inst.underlyer(); }
    double underlyer_spot(const InstrumentData& inst) const { return inst.underlyer_spot(); }
    double delta(const InstrumentData& inst) const { return inst.delta(); }
    double vega(const InstrumentData& inst) const { return inst.vega(); }
};

NewOrderSingle create_option_order(const std::string& cl_ord_id, const std::string& symbol,
                                    const std::string& underlyer, Side side,
                                    double price, int64_t qty,
                                    const std::string& strategy = "STRAT1") {
    NewOrderSingle order;
    order.key.cl_ord_id = cl_ord_id;
    order.symbol = symbol;
    order.underlyer = underlyer;
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

// Create provider for options
StaticInstrumentProvider create_options_provider() {
    StaticInstrumentProvider provider;

    // AAPL options (contract_size=100)
    // AAPL_C150: Call, spot=$5.00, delta=0.5
    provider.add_option("AAPL_C150", "AAPL", 5.0, 150.0, 0.5, 100.0, 1.0);
    // AAPL_P150: Put, spot=$3.00, delta=-0.4
    provider.add_option("AAPL_P150", "AAPL", 3.0, 150.0, -0.4, 100.0, 1.0);

    // MSFT options (contract_size=50, to test different sizes)
    // MSFT_C300: Call, spot=$8.00, delta=0.6
    provider.add_option("MSFT_C300", "MSFT", 8.0, 300.0, 0.6, 50.0, 1.0);
    // MSFT_P300: Put, spot=$6.00, delta=-0.5
    provider.add_option("MSFT_P300", "MSFT", 6.0, 300.0, -0.5, 50.0, 1.0);

    return provider;
}

}  // namespace

// ============================================================================
// Test Fixture: Options Gross/Net Position Notional
// ============================================================================

class OptionsGrossNetPositionTest : public ::testing::Test {
protected:
    // Position-stage only notional metrics (track filled orders)
    using GrossPositionNotional = GlobalGrossNotionalMetric<TestContext, InstrumentData, PositionStage>;
    using NetPositionNotional = GlobalNetNotionalMetric<TestContext, InstrumentData, PositionStage>;

    using TestEngine = RiskAggregationEngineWithLimits<
        TestContext,
        InstrumentData,
        GrossPositionNotional,
        NetPositionNotional
    >;

    StaticInstrumentProvider provider;
    std::unique_ptr<TestContext> context;
    std::unique_ptr<TestEngine> engine;

    // Limits
    static constexpr double MAX_GROSS_POSITION = 50000.0;
    static constexpr double MAX_NET_POSITION = 25000.0;

    void SetUp() override {
        provider = create_options_provider();
        context = std::make_unique<TestContext>(provider);
        engine = std::make_unique<TestEngine>(*context);
        engine->set_limit<GrossPositionNotional>(GlobalKey::instance(), MAX_GROSS_POSITION);
        engine->set_limit<NetPositionNotional>(GlobalKey::instance(), MAX_NET_POSITION);
    }

    // Position accessors
    double gross_position() const {
        return engine->get_metric<GrossPositionNotional>().get_position(GlobalKey::instance());
    }

    double net_position() const {
        return engine->get_metric<NetPositionNotional>().get_position(GlobalKey::instance());
    }

    // Helper to get instrument from provider
    InstrumentData get_instrument(const std::string& symbol) const {
        return provider.get_instrument(symbol);
    }

    // Instrument-based position manipulation (engine-level interface)
    // A single call updates both gross and net metrics
    void set_instrument_position(const std::string& symbol, int64_t signed_quantity) {
        engine->set_instrument_position(symbol, signed_quantity, get_instrument(symbol));
    }

    // Compute expected notional for an option
    double compute_notional(const std::string& symbol, int64_t qty) const {
        return static_cast<double>(qty)
             * provider.get_contract_size(symbol)
             * provider.get_spot_price(symbol)
             * provider.get_fx_rate(symbol);
    }
};

// ============================================================================
// Test: Single BID fill creates positive gross and positive net
// ============================================================================

TEST_F(OptionsGrossNetPositionTest, SingleBidFillCreatesPosition) {
    // AAPL_C150: qty=10, contract_size=100, spot=$5.00, fx=1.0
    // notional = 10 * 100 * 5.0 * 1.0 = 5,000

    EXPECT_DOUBLE_EQ(gross_position(), 0.0) << "Initial: gross_position=0";
    EXPECT_DOUBLE_EQ(net_position(), 0.0) << "Initial: net_position=0";

    auto order = create_option_order("ORD001", "AAPL_C150", "AAPL", Side::BID, 5.0, 10);
    auto inst = get_instrument(order.symbol);
    engine->on_new_order_single(order, inst);
    engine->on_execution_report(create_ack("ORD001", 10), inst);
    engine->on_execution_report(create_fill("ORD001", 10, 0, 5.0), inst);

    EXPECT_DOUBLE_EQ(gross_position(), 5000.0) << "After fill: gross_position=5000";
    EXPECT_DOUBLE_EQ(net_position(), 5000.0) << "After fill: net_position=+5000 (BID)";
}

// ============================================================================
// Test: Single ASK fill creates positive gross and negative net
// ============================================================================

TEST_F(OptionsGrossNetPositionTest, SingleAskFillCreatesPosition) {
    // MSFT_C300: qty=20, contract_size=50, spot=$8.00, fx=1.0
    // notional = 20 * 50 * 8.0 * 1.0 = 8,000

    auto order = create_option_order("ORD001", "MSFT_C300", "MSFT", Side::ASK, 8.0, 20);
    auto inst = get_instrument(order.symbol);
    engine->on_new_order_single(order, inst);
    engine->on_execution_report(create_ack("ORD001", 20), inst);
    engine->on_execution_report(create_fill("ORD001", 20, 0, 8.0), inst);

    EXPECT_DOUBLE_EQ(gross_position(), 8000.0) << "After fill: gross_position=8000";
    EXPECT_DOUBLE_EQ(net_position(), -8000.0) << "After fill: net_position=-8000 (ASK)";
}

// ============================================================================
// Test: Multiple options - gross vs net difference
// ============================================================================

TEST_F(OptionsGrossNetPositionTest, MultipleOptionsGrossVsNet) {
    // Scenario:
    // AAPL_C150 BID qty=10: notional = 10 * 100 * 5.0 = 5,000
    // MSFT_C300 ASK qty=20: notional = 20 * 50 * 8.0 = 8,000
    //
    // After both fills:
    //   gross_position = 5,000 + 8,000 = 13,000
    //   net_position = 5,000 - 8,000 = -3,000

    // BID order
    auto order1 = create_option_order("ORD001", "AAPL_C150", "AAPL", Side::BID, 5.0, 10);
    auto inst1 = get_instrument(order1.symbol);
    engine->on_new_order_single(order1, inst1);
    engine->on_execution_report(create_ack("ORD001", 10), inst1);
    engine->on_execution_report(create_fill("ORD001", 10, 0, 5.0), inst1);

    EXPECT_DOUBLE_EQ(gross_position(), 5000.0) << "After AAPL BID fill: gross=5000";
    EXPECT_DOUBLE_EQ(net_position(), 5000.0) << "After AAPL BID fill: net=+5000";

    // ASK order
    auto order2 = create_option_order("ORD002", "MSFT_C300", "MSFT", Side::ASK, 8.0, 20);
    auto inst2 = get_instrument(order2.symbol);
    engine->on_new_order_single(order2, inst2);
    engine->on_execution_report(create_ack("ORD002", 20), inst2);
    engine->on_execution_report(create_fill("ORD002", 20, 0, 8.0), inst2);

    EXPECT_DOUBLE_EQ(gross_position(), 13000.0) << "After both fills: gross=13000";
    EXPECT_DOUBLE_EQ(net_position(), -3000.0) << "After both fills: net=-3000";
}

// ============================================================================
// Test: Different contract sizes
// ============================================================================

TEST_F(OptionsGrossNetPositionTest, DifferentContractSizes) {
    // AAPL_C150: contract_size=100
    // MSFT_C300: contract_size=50
    //
    // Same qty=10 for both:
    // AAPL: 10 * 100 * 5.0 = 5,000
    // MSFT: 10 * 50 * 8.0 = 4,000

    // AAPL BID
    auto order1 = create_option_order("ORD001", "AAPL_C150", "AAPL", Side::BID, 5.0, 10);
    auto inst1 = get_instrument(order1.symbol);
    engine->on_new_order_single(order1, inst1);
    engine->on_execution_report(create_ack("ORD001", 10), inst1);
    engine->on_execution_report(create_fill("ORD001", 10, 0, 5.0), inst1);

    double aapl_notional = compute_notional("AAPL_C150", 10);
    EXPECT_DOUBLE_EQ(aapl_notional, 5000.0) << "AAPL notional = 10 * 100 * 5.0 = 5000";
    EXPECT_DOUBLE_EQ(gross_position(), 5000.0);

    // MSFT BID (same qty, different contract size)
    auto order2 = create_option_order("ORD002", "MSFT_C300", "MSFT", Side::BID, 8.0, 10);
    auto inst2 = get_instrument(order2.symbol);
    engine->on_new_order_single(order2, inst2);
    engine->on_execution_report(create_ack("ORD002", 10), inst2);
    engine->on_execution_report(create_fill("ORD002", 10, 0, 8.0), inst2);

    double msft_notional = compute_notional("MSFT_C300", 10);
    EXPECT_DOUBLE_EQ(msft_notional, 4000.0) << "MSFT notional = 10 * 50 * 8.0 = 4000";
    EXPECT_DOUBLE_EQ(gross_position(), 9000.0) << "Total gross = 5000 + 4000 = 9000";
    EXPECT_DOUBLE_EQ(net_position(), 9000.0) << "Both BID, net = +9000";
}

// ============================================================================
// Test: Gross position limit check
// ============================================================================

TEST_F(OptionsGrossNetPositionTest, GrossPositionLimitCheck) {
    // Set a lower limit for testing
    engine->set_limit<GrossPositionNotional>(GlobalKey::instance(), 20000.0);

    // AAPL_C150 BID qty=30 (notional = 30 * 100 * 5.0 = 15,000)
    auto order1 = create_option_order("ORD001", "AAPL_C150", "AAPL", Side::BID, 5.0, 30);
    auto inst1 = get_instrument(order1.symbol);
    engine->on_new_order_single(order1, inst1);
    engine->on_execution_report(create_ack("ORD001", 30), inst1);
    engine->on_execution_report(create_fill("ORD001", 30, 0, 5.0), inst1);

    EXPECT_DOUBLE_EQ(gross_position(), 15000.0);

    // Pre-trade check: MSFT_C300 BID qty=20 (notional = 20 * 50 * 8.0 = 8,000)
    // Would push gross to 23,000 > 20,000 limit
    auto order = create_option_order("ORD002", "MSFT_C300", "MSFT", Side::BID, 8.0, 20);
    auto inst = get_instrument(order.symbol);
    auto result = engine->pre_trade_check(order, inst);

    EXPECT_TRUE(result.would_breach) << "Should breach gross limit: 15000 + 8000 = 23000 > 20000";
    EXPECT_TRUE(result.has_breach(LimitType::GLOBAL_GROSS_NOTIONAL));

    const auto* breach = result.get_breach(LimitType::GLOBAL_GROSS_NOTIONAL);
    ASSERT_NE(breach, nullptr);
    EXPECT_DOUBLE_EQ(breach->current_usage, 15000.0);
    EXPECT_DOUBLE_EQ(breach->hypothetical_usage, 23000.0);
    EXPECT_DOUBLE_EQ(breach->limit_value, 20000.0);
}

// ============================================================================
// Test: Net position limit check
// ============================================================================

TEST_F(OptionsGrossNetPositionTest, NetPositionLimitCheck) {
    // Set a limit for net position
    engine->set_limit<NetPositionNotional>(GlobalKey::instance(), 10000.0);

    // AAPL_C150 BID qty=15 (notional = 15 * 100 * 5.0 = 7,500)
    auto order1 = create_option_order("ORD001", "AAPL_C150", "AAPL", Side::BID, 5.0, 15);
    auto inst1 = get_instrument(order1.symbol);
    engine->on_new_order_single(order1, inst1);
    engine->on_execution_report(create_ack("ORD001", 15), inst1);
    engine->on_execution_report(create_fill("ORD001", 15, 0, 5.0), inst1);

    EXPECT_DOUBLE_EQ(net_position(), 7500.0);

    // Pre-trade check: AAPL_P150 BID qty=20 (notional = 20 * 100 * 3.0 = 6,000)
    // Would push net to 13,500 > 10,000 limit
    auto order = create_option_order("ORD002", "AAPL_P150", "AAPL", Side::BID, 3.0, 20);
    auto inst = get_instrument(order.symbol);
    auto result = engine->pre_trade_check(order, inst);

    EXPECT_TRUE(result.would_breach) << "Should breach net limit: 7500 + 6000 = 13500 > 10000";
    EXPECT_TRUE(result.has_breach(LimitType::GLOBAL_NET_NOTIONAL));

    const auto* breach = result.get_breach(LimitType::GLOBAL_NET_NOTIONAL);
    ASSERT_NE(breach, nullptr);
    EXPECT_DOUBLE_EQ(breach->current_usage, 7500.0);
    EXPECT_DOUBLE_EQ(breach->hypothetical_usage, 13500.0);
    EXPECT_DOUBLE_EQ(breach->limit_value, 10000.0);
}

// ============================================================================
// Test: ASK orders reduce net but increase gross
// ============================================================================

TEST_F(OptionsGrossNetPositionTest, AskOrdersReduceNetButIncreaseGross) {
    // Start with BID position: AAPL_C150 BID qty=10 (notional=5000)
    auto order1 = create_option_order("ORD001", "AAPL_C150", "AAPL", Side::BID, 5.0, 10);
    auto inst1 = get_instrument(order1.symbol);
    engine->on_new_order_single(order1, inst1);
    engine->on_execution_report(create_ack("ORD001", 10), inst1);
    engine->on_execution_report(create_fill("ORD001", 10, 0, 5.0), inst1);

    EXPECT_DOUBLE_EQ(gross_position(), 5000.0);
    EXPECT_DOUBLE_EQ(net_position(), 5000.0);

    // Add ASK position: AAPL_C150 ASK qty=10 (notional=5000)
    // This increases gross but decreases net
    auto order2 = create_option_order("ORD002", "AAPL_C150", "AAPL", Side::ASK, 5.0, 10);
    auto inst2 = get_instrument(order2.symbol);
    engine->on_new_order_single(order2, inst2);
    engine->on_execution_report(create_ack("ORD002", 10), inst2);
    engine->on_execution_report(create_fill("ORD002", 10, 0, 5.0), inst2);

    EXPECT_DOUBLE_EQ(gross_position(), 10000.0) << "Gross increases: 5000 + 5000 = 10000";
    EXPECT_DOUBLE_EQ(net_position(), 0.0) << "Net cancels out: 5000 - 5000 = 0";
}

// ============================================================================
// Test: Clear resets position
// ============================================================================

TEST_F(OptionsGrossNetPositionTest, ClearResetsPosition) {
    // Create position
    auto order = create_option_order("ORD001", "AAPL_C150", "AAPL", Side::BID, 5.0, 10);
    auto inst = get_instrument(order.symbol);
    engine->on_new_order_single(order, inst);
    engine->on_execution_report(create_ack("ORD001", 10), inst);
    engine->on_execution_report(create_fill("ORD001", 10, 0, 5.0), inst);

    EXPECT_GT(gross_position(), 0.0);

    engine->clear();

    EXPECT_DOUBLE_EQ(gross_position(), 0.0);
    EXPECT_DOUBLE_EQ(net_position(), 0.0);
}

// ============================================================================
// Test: Set gross position for single instrument
// ============================================================================

TEST_F(OptionsGrossNetPositionTest, SetGrossPositionForInstrument) {
    // Start with no position
    EXPECT_DOUBLE_EQ(gross_position(), 0.0);

    // Set position for AAPL_C150: qty=20 (long position)
    // Notional = 20 * 100 * 5.0 = 10,000
    set_instrument_position("AAPL_C150", 20);

    EXPECT_DOUBLE_EQ(gross_position(), 10000.0)
        << "After AAPL_C150: global gross = 10000";

    // Set position for MSFT_C300: qty=30 (long position)
    // Notional = 30 * 50 * 8.0 = 12,000
    set_instrument_position("MSFT_C300", 30);

    EXPECT_DOUBLE_EQ(gross_position(), 22000.0)
        << "Global gross = 10000 + 12000 = 22000";
}

// ============================================================================
// Test: Set net position for instrument (long and short)
// ============================================================================

TEST_F(OptionsGrossNetPositionTest, SetNetPositionForInstrument) {
    // Start with no position
    EXPECT_DOUBLE_EQ(net_position(), 0.0);

    // Set long position for AAPL_C150: qty=+20 (positive = long/BID)
    // Notional = 20 * 100 * 5.0 = 10,000
    set_instrument_position("AAPL_C150", 20);

    EXPECT_DOUBLE_EQ(net_position(), 10000.0)
        << "After long position: global net = +10000";

    // Set short position for MSFT_C300: qty=-30 (negative = short/ASK)
    // Notional = 30 * 50 * 8.0 = 12,000 (but negative for short)
    set_instrument_position("MSFT_C300", -30);

    EXPECT_DOUBLE_EQ(net_position(), -2000.0)
        << "Global net = 10000 - 12000 = -2000";
}

// ============================================================================
// Test: Manual position combined with fills
// ============================================================================

TEST_F(OptionsGrossNetPositionTest, ManualPositionCombinedWithFills) {
    // Set initial position for AAPL_C150: qty=20 (notional=10000)
    // Engine-level interface updates both gross and net metrics
    set_instrument_position("AAPL_C150", 20);

    EXPECT_DOUBLE_EQ(gross_position(), 10000.0);
    EXPECT_DOUBLE_EQ(net_position(), 10000.0);

    // Now fill an order for MSFT_C300: qty=20 (notional=8000)
    auto order = create_option_order("ORD001", "MSFT_C300", "MSFT", Side::BID, 8.0, 20);
    auto inst = get_instrument(order.symbol);
    engine->on_new_order_single(order, inst);
    engine->on_execution_report(create_ack("ORD001", 20), inst);
    engine->on_execution_report(create_fill("ORD001", 20, 0, 8.0), inst);

    EXPECT_DOUBLE_EQ(gross_position(), 18000.0)
        << "Manual 10000 + fill 8000 = 18000";
    EXPECT_DOUBLE_EQ(net_position(), 18000.0)
        << "Manual +10000 + fill +8000 = +18000";
}

// ============================================================================
// Test: Manual position affects pre-trade checks
// ============================================================================

TEST_F(OptionsGrossNetPositionTest, ManualPositionAffectsPreTradeCheck) {
    // Set a limit
    engine->set_limit<GrossPositionNotional>(GlobalKey::instance(), 20000.0);

    // Set manual position for AAPL_C150: qty=30 (notional=15000)
    set_instrument_position("AAPL_C150", 30);
    EXPECT_DOUBLE_EQ(gross_position(), 15000.0);

    // Pre-trade check: MSFT_C300 BID qty=20 (notional = 20 * 50 * 8.0 = 8,000)
    // Would push gross to 23,000 > 20,000 limit
    auto order = create_option_order("ORD001", "MSFT_C300", "MSFT", Side::BID, 8.0, 20);
    auto inst = get_instrument(order.symbol);
    auto result = engine->pre_trade_check(order, inst);

    EXPECT_TRUE(result.would_breach) << "Should breach: 15000 (manual) + 8000 = 23000 > 20000";
    EXPECT_TRUE(result.has_breach(LimitType::GLOBAL_GROSS_NOTIONAL));

    const auto* breach = result.get_breach(LimitType::GLOBAL_GROSS_NOTIONAL);
    ASSERT_NE(breach, nullptr);
    EXPECT_DOUBLE_EQ(breach->current_usage, 15000.0);
    EXPECT_DOUBLE_EQ(breach->hypothetical_usage, 23000.0);
}

// ============================================================================
// Test: Update existing instrument position
// ============================================================================

TEST_F(OptionsGrossNetPositionTest, UpdateExistingInstrumentPosition) {
    // Set initial position for AAPL_C150: qty=10 (notional=5000)
    set_instrument_position("AAPL_C150", 10);
    EXPECT_DOUBLE_EQ(gross_position(), 5000.0);

    // Update position to qty=30 (notional=15000)
    set_instrument_position("AAPL_C150", 30);
    EXPECT_DOUBLE_EQ(gross_position(), 15000.0)
        << "Position updated to 30 * 100 * 5.0 = 15000";
}

// ============================================================================
// Test: Multiple instruments with different contract sizes
// ============================================================================

TEST_F(OptionsGrossNetPositionTest, MultipleInstrumentsWithDifferentContractSizes) {
    // Set positions for multiple instruments
    // AAPL_C150: qty=10, contract_size=100, spot=5.0 => notional=5000
    // AAPL_P150: qty=15, contract_size=100, spot=3.0 => notional=4500
    // MSFT_C300: qty=20, contract_size=50, spot=8.0 => notional=8000
    // MSFT_P300: qty=25, contract_size=50, spot=6.0 => notional=7500

    set_instrument_position("AAPL_C150", 10);
    EXPECT_DOUBLE_EQ(gross_position(), 5000.0) << "After AAPL_C150: 5000";

    set_instrument_position("AAPL_P150", 15);
    EXPECT_DOUBLE_EQ(gross_position(), 9500.0) << "After AAPL_P150: 5000 + 4500 = 9500";

    set_instrument_position("MSFT_C300", 20);
    EXPECT_DOUBLE_EQ(gross_position(), 17500.0) << "After MSFT_C300: 9500 + 8000 = 17500";

    set_instrument_position("MSFT_P300", 25);
    EXPECT_DOUBLE_EQ(gross_position(), 25000.0)
        << "Global gross = 5000 + 4500 + 8000 + 7500 = 25000";
}

// ============================================================================
// Test: Net position with mixed long and short across instruments
// ============================================================================

TEST_F(OptionsGrossNetPositionTest, NetPositionMixedLongShort) {
    // Set long and short positions across instruments
    // AAPL_C150: long 20 => +10000
    // AAPL_P150: short 15 => -4500
    // MSFT_C300: short 10 => -4000
    // MSFT_P300: long 30 => +9000

    set_instrument_position("AAPL_C150", 20);   // long
    EXPECT_DOUBLE_EQ(net_position(), 10000.0) << "After AAPL_C150 long: +10000";

    set_instrument_position("AAPL_P150", -15);  // short
    EXPECT_DOUBLE_EQ(net_position(), 5500.0) << "After AAPL_P150 short: 10000 - 4500 = 5500";

    set_instrument_position("MSFT_C300", -10);  // short
    EXPECT_DOUBLE_EQ(net_position(), 1500.0) << "After MSFT_C300 short: 5500 - 4000 = 1500";

    set_instrument_position("MSFT_P300", 30);   // long
    EXPECT_DOUBLE_EQ(net_position(), 10500.0)
        << "Global net = 10000 - 4500 - 4000 + 9000 = 10500";
}

// ============================================================================
// Test: Clear removes all instrument positions
// ============================================================================

TEST_F(OptionsGrossNetPositionTest, ClearRemovesInstrumentPositions) {
    // Set positions for multiple instruments using engine-level interface
    // AAPL_C150: long 20, MSFT_C300: short 30
    set_instrument_position("AAPL_C150", 20);
    set_instrument_position("MSFT_C300", -30);

    EXPECT_GT(gross_position(), 0.0);
    EXPECT_NE(net_position(), 0.0);

    // Clear
    engine->clear();

    EXPECT_DOUBLE_EQ(gross_position(), 0.0) << "Clear removes all gross positions";
    EXPECT_DOUBLE_EQ(net_position(), 0.0) << "Clear removes all net positions";
}

// ============================================================================
// Test: Pre-trade check with mixed manual and filled positions
// ============================================================================

TEST_F(OptionsGrossNetPositionTest, PreTradeCheckMixedPositions) {
    engine->set_limit<NetPositionNotional>(GlobalKey::instance(), 15000.0);

    // Set manual long position for AAPL_C150: qty=16 (notional=8000)
    set_instrument_position("AAPL_C150", 16);
    EXPECT_DOUBLE_EQ(net_position(), 8000.0);

    // Fill a BID order for MSFT_C300: qty=10 (notional=4000)
    auto order1 = create_option_order("ORD001", "MSFT_C300", "MSFT", Side::BID, 8.0, 10);
    auto inst1 = get_instrument(order1.symbol);
    engine->on_new_order_single(order1, inst1);
    engine->on_execution_report(create_ack("ORD001", 10), inst1);
    engine->on_execution_report(create_fill("ORD001", 10, 0, 8.0), inst1);

    EXPECT_DOUBLE_EQ(net_position(), 12000.0) << "Manual 8000 + fill 4000 = 12000";

    // Pre-trade check: Another BID for AAPL_P150 qty=20 (notional=6000) would breach limit
    auto order = create_option_order("ORD002", "AAPL_P150", "AAPL", Side::BID, 3.0, 20);
    auto inst = get_instrument(order.symbol);
    auto result = engine->pre_trade_check(order, inst);

    EXPECT_TRUE(result.would_breach) << "Should breach: 12000 + 6000 = 18000 > 15000";
    EXPECT_TRUE(result.has_breach(LimitType::GLOBAL_NET_NOTIONAL));

    const auto* breach = result.get_breach(LimitType::GLOBAL_NET_NOTIONAL);
    ASSERT_NE(breach, nullptr);
    EXPECT_DOUBLE_EQ(breach->current_usage, 12000.0);
    EXPECT_DOUBLE_EQ(breach->hypothetical_usage, 18000.0);
    EXPECT_DOUBLE_EQ(breach->limit_value, 15000.0);
}
