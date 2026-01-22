#include <gtest/gtest.h>
#include "../src/engine/risk_engine_with_limits.hpp"
#include "../src/metrics/delta_metric.hpp"
#include "../src/metrics/notional_metric.hpp"
#include "../src/metrics/order_count_metric.hpp"
#include "../src/instrument/instrument.hpp"
#include "../src/fix/fix_parser.hpp"
#include <memory>

using namespace engine;
using namespace fix;
using namespace aggregation;
using namespace metrics;
using namespace instrument;

// ============================================================================
// Test Contexts
// ============================================================================

// Simple context for tests using SimpleInstrumentProvider
class SimpleTestContext {
    const SimpleInstrumentProvider& provider_;
public:
    explicit SimpleTestContext(const SimpleInstrumentProvider& provider) : provider_(provider) {}

    double spot_price(const InstrumentData& inst) const { return inst.spot_price(); }
    double fx_rate(const InstrumentData& inst) const { return inst.fx_rate(); }
    double contract_size(const InstrumentData& inst) const { return inst.contract_size(); }
    const std::string& underlyer(const InstrumentData& inst) const { return inst.underlyer(); }
    double underlyer_spot(const InstrumentData& inst) const { return inst.underlyer_spot(); }
    double delta(const InstrumentData& inst) const { return inst.delta(); }
    double vega(const InstrumentData& inst) const { return inst.vega(); }
};

// Static context for tests using StaticInstrumentProvider
class StaticTestContext {
    const StaticInstrumentProvider& provider_;
public:
    explicit StaticTestContext(const StaticInstrumentProvider& provider) : provider_(provider) {}

    double spot_price(const InstrumentData& inst) const { return inst.spot_price(); }
    double fx_rate(const InstrumentData& inst) const { return inst.fx_rate(); }
    double contract_size(const InstrumentData& inst) const { return inst.contract_size(); }
    const std::string& underlyer(const InstrumentData& inst) const { return inst.underlyer(); }
    double underlyer_spot(const InstrumentData& inst) const { return inst.underlyer_spot(); }
    double delta(const InstrumentData& inst) const { return inst.delta(); }
    double vega(const InstrumentData& inst) const { return inst.vega(); }
};

// ============================================================================
// Test: Pre-trade checks for order updates (OrderCancelReplaceRequest)
// ============================================================================
//
// These tests verify:
//   1. pre_trade_check(OrderCancelReplaceRequest) works correctly
//   2. pre_trade_check_single<Metric>() works for both new orders and updates
//   3. compute_update_contribution() correctly computes deltas for updates
//

namespace {

NewOrderSingle create_order(const std::string& cl_ord_id, const std::string& symbol,
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

OrderCancelReplaceRequest create_replace(const std::string& new_id, const std::string& orig_id,
                                          const std::string& symbol, Side side,
                                          double new_price, int64_t new_qty) {
    OrderCancelReplaceRequest req;
    req.key.cl_ord_id = new_id;
    req.orig_key.cl_ord_id = orig_id;
    req.symbol = symbol;
    req.side = side;
    req.price = new_price;
    req.quantity = new_qty;
    return req;
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

ExecutionReport create_replace_ack(const std::string& new_id, const std::string& orig_id,
                                    int64_t leaves_qty) {
    ExecutionReport report;
    report.key.cl_ord_id = new_id;
    report.orig_key = OrderKey{orig_id};
    report.order_id = "EX" + orig_id;
    report.ord_status = OrdStatus::NEW;
    report.exec_type = ExecType::REPLACED;
    report.leaves_qty = leaves_qty;
    report.cum_qty = 0;
    report.is_unsolicited = false;
    return report;
}

OrderCancelReject create_replace_nack(const std::string& new_id, const std::string& orig_id) {
    OrderCancelReject reject;
    reject.key.cl_ord_id = new_id;
    reject.orig_key.cl_ord_id = orig_id;
    reject.order_id = "EX" + orig_id;
    reject.ord_status = OrdStatus::NEW;  // Order still in original state
    reject.response_to = CxlRejResponseTo::ORDER_CANCEL_REPLACE_REQUEST;
    reject.cxl_rej_reason = 0;
    return reject;
}

// Create provider for options with delta (requires StaticInstrumentProvider for delta support)
StaticInstrumentProvider create_option_provider() {
    StaticInstrumentProvider provider;
    // Add underlyer
    provider.add_equity("AAPL", 150.0);
    // Options with delta
    // add_option(symbol, underlyer, spot_price, underlyer_spot, delta, contract_size, fx_rate)
    provider.add_option("AAPL_OPT1", "AAPL", 5.0, 150.0, 0.5, 100.0, 1.0);
    provider.add_option("AAPL_OPT2", "AAPL", 3.0, 150.0, 0.3, 100.0, 1.0);
    return provider;
}

// Create provider for stocks (no delta)
SimpleInstrumentProvider create_stock_provider() {
    SimpleInstrumentProvider provider;
    provider.set_spot_price("AAPL", 150.0);
    provider.set_spot_price("MSFT", 300.0);
    return provider;
}

}  // namespace

// ============================================================================
// Test: Pre-trade check for order updates - Notional
// ============================================================================

class PreTradeCheckUpdateNotionalTest : public ::testing::Test {
protected:
    using GlobalNotional = GlobalNotionalMetric<SimpleTestContext, InstrumentData, OpenStage, InFlightStage>;

    using TestEngine = RiskAggregationEngineWithLimits<
        SimpleTestContext,
        InstrumentData,
        GlobalNotional
    >;

    SimpleInstrumentProvider provider;
    std::unique_ptr<SimpleTestContext> context;
    std::unique_ptr<TestEngine> engine;

    static constexpr double MAX_NOTIONAL = 50000.0;

    void SetUp() override {
        provider = create_stock_provider();
        context = std::make_unique<SimpleTestContext>(provider);
        engine = std::make_unique<TestEngine>(*context);
        engine->set_limit<GlobalNotional>(GlobalKey::instance(), MAX_NOTIONAL);
    }

    double notional() const {
        return engine->get_metric<GlobalNotional>().get(GlobalKey::instance());
    }

    // Helper to get instrument from provider
    InstrumentData get_instrument(const std::string& symbol) const {
        return provider.get_instrument(symbol);
    }
};

TEST_F(PreTradeCheckUpdateNotionalTest, UpdateIncreaseQuantityBreachesLimit) {
    // Insert order: 100 AAPL @ $150 = $15,000
    auto order = create_order("ORD001", "AAPL", "AAPL", Side::BID, 150.0, 100);
    auto inst = get_instrument(order.symbol);
    engine->on_new_order_single(order, inst);
    engine->on_execution_report(create_ack("ORD001", 100), inst);

    EXPECT_DOUBLE_EQ(notional(), 15000.0);

    // Update to increase quantity: 400 AAPL @ $150 = $60,000
    // Delta = 60,000 - 15,000 = +45,000
    // After: 15,000 + 45,000 = 60,000 > 50,000 limit
    auto replace = create_replace("ORD001_R", "ORD001", "AAPL", Side::BID, 150.0, 400);
    engine->on_order_cancel_replace(replace, inst);

    auto result = engine->pre_trade_check(replace, inst);
    EXPECT_TRUE(result.would_breach) << "Update should breach limit";
    EXPECT_TRUE(result.has_breach(LimitType::GLOBAL_NOTIONAL));

    const auto* breach = result.get_breach(LimitType::GLOBAL_NOTIONAL);
    ASSERT_NE(breach, nullptr);
    EXPECT_DOUBLE_EQ(breach->current_usage, 15000.0);
    EXPECT_DOUBLE_EQ(breach->hypothetical_usage, 60000.0);
    EXPECT_DOUBLE_EQ(breach->limit_value, 50000.0);
}

TEST_F(PreTradeCheckUpdateNotionalTest, UpdateIncreaseWithinLimit) {
    // Insert order: 100 AAPL @ $150 = $15,000
    auto order = create_order("ORD001", "AAPL", "AAPL", Side::BID, 150.0, 100);
    auto inst = get_instrument(order.symbol);
    engine->on_new_order_single(order, inst);
    engine->on_execution_report(create_ack("ORD001", 100), inst);

    // Update to 200 AAPL @ $150 = $30,000
    // Delta = 30,000 - 15,000 = +15,000
    // After: 15,000 + 15,000 = 30,000 < 50,000 limit
    auto replace = create_replace("ORD001_R", "ORD001", "AAPL", Side::BID, 150.0, 200);
    engine->on_order_cancel_replace(replace, inst);

    auto result = engine->pre_trade_check(replace, inst);
    EXPECT_FALSE(result.would_breach) << "Update within limit should pass";
}

TEST_F(PreTradeCheckUpdateNotionalTest, UpdateDecreaseQuantityAlwaysPasses) {
    // Insert order: 300 AAPL @ $150 = $45,000
    auto order = create_order("ORD001", "AAPL", "AAPL", Side::BID, 150.0, 300);
    auto inst = get_instrument(order.symbol);
    engine->on_new_order_single(order, inst);
    engine->on_execution_report(create_ack("ORD001", 300), inst);

    EXPECT_DOUBLE_EQ(notional(), 45000.0);

    // Update to decrease: 100 AAPL @ $150 = $15,000
    // Delta = 15,000 - 45,000 = -30,000
    // After: 45,000 - 30,000 = 15,000 < 50,000
    auto replace = create_replace("ORD001_R", "ORD001", "AAPL", Side::BID, 150.0, 100);
    engine->on_order_cancel_replace(replace, inst);

    auto result = engine->pre_trade_check(replace, inst);
    EXPECT_FALSE(result.would_breach) << "Decrease should never breach";
}

TEST_F(PreTradeCheckUpdateNotionalTest, UpdateNonExistentOrderReturnsOk) {
    // Pre-trade check for update on non-existent order should return OK
    // (order not found, so no breach can be detected)
    auto replace = create_replace("ORD001_R", "NONEXISTENT", "AAPL", Side::BID, 150.0, 100);
    auto inst = get_instrument("AAPL");

    auto result = engine->pre_trade_check(replace, inst);
    EXPECT_FALSE(result.would_breach) << "Non-existent order should not breach";
}

// ============================================================================
// Test: Pre-trade check for order updates - Delta
// ============================================================================

class PreTradeCheckUpdateDeltaTest : public ::testing::Test {
protected:
    using GrossDelta = GrossDeltaMetric<UnderlyerKey, StaticTestContext, InstrumentData, AllStages>;
    using NetDelta = NetDeltaMetric<UnderlyerKey, StaticTestContext, InstrumentData, AllStages>;

    using TestEngine = RiskAggregationEngineWithLimits<
        StaticTestContext,
        InstrumentData,
        GrossDelta,
        NetDelta
    >;

    StaticInstrumentProvider provider;
    std::unique_ptr<StaticTestContext> context;
    std::unique_ptr<TestEngine> engine;

    // Delta exposure = qty * delta * contract_size * underlyer_spot * fx_rate
    // For 100 contracts: 100 * 0.5 * 100 * 150 = 750,000
    static constexpr double MAX_GROSS_DELTA = 1000000.0;  // 1M limit
    static constexpr double MAX_NET_DELTA = 500000.0;     // 500K limit

    void SetUp() override {
        provider = create_option_provider();
        context = std::make_unique<StaticTestContext>(provider);
        engine = std::make_unique<TestEngine>(*context);
        engine->set_default_limit<GrossDelta>(MAX_GROSS_DELTA);
        engine->set_default_limit<NetDelta>(MAX_NET_DELTA);
    }

    double gross_delta(const std::string& underlyer) const {
        return engine->get_metric<GrossDelta>().get(UnderlyerKey{underlyer});
    }

    double net_delta(const std::string& underlyer) const {
        return engine->get_metric<NetDelta>().get(UnderlyerKey{underlyer});
    }

    // Helper to get instrument from provider
    InstrumentData get_instrument(const std::string& symbol) const {
        return provider.get_instrument(symbol);
    }
};

TEST_F(PreTradeCheckUpdateDeltaTest, UpdateBreachesGrossDelta) {
    // Insert order: 100 contracts of AAPL_OPT1
    // Delta exposure = qty * delta * contract_size * underlyer_spot * fx_rate
    //                = 100 * 0.5 * 100 * 150.0 * 1.0 = 750,000
    auto order = create_order("ORD001", "AAPL_OPT1", "AAPL", Side::BID, 5.0, 100);
    auto inst = get_instrument(order.symbol);
    engine->on_new_order_single(order, inst);
    engine->on_execution_report(create_ack("ORD001", 100), inst);

    EXPECT_DOUBLE_EQ(gross_delta("AAPL"), 750000.0);

    // Update to 150 contracts
    // New delta exposure = 150 * 0.5 * 100 * 150.0 = 1,125,000
    // Delta contribution = 1,125,000 - 750,000 = +375,000
    // After: 750,000 + 375,000 = 1,125,000 > 1,000,000 limit
    auto replace = create_replace("ORD001_R", "ORD001", "AAPL_OPT1", Side::BID, 5.0, 150);
    engine->on_order_cancel_replace(replace, inst);

    auto result = engine->pre_trade_check(replace, inst);
    EXPECT_TRUE(result.would_breach);
    EXPECT_TRUE(result.has_breach(LimitType::GROSS_DELTA));
}

TEST_F(PreTradeCheckUpdateDeltaTest, UpdateBreachesNetDelta) {
    // Insert BID order: 50 contracts
    // Net delta (BID) = 50 * 0.5 * 100 * 150 = 375,000
    auto order = create_order("ORD001", "AAPL_OPT1", "AAPL", Side::BID, 5.0, 50);
    auto inst = get_instrument(order.symbol);
    engine->on_new_order_single(order, inst);
    engine->on_execution_report(create_ack("ORD001", 50), inst);

    EXPECT_DOUBLE_EQ(net_delta("AAPL"), 375000.0);

    // Update to 100 contracts
    // New net delta = 100 * 0.5 * 100 * 150 = 750,000
    // Delta contribution = 750,000 - 375,000 = +375,000
    // After: 375,000 + 375,000 = 750,000 > 500,000 limit
    auto replace = create_replace("ORD001_R", "ORD001", "AAPL_OPT1", Side::BID, 5.0, 100);
    engine->on_order_cancel_replace(replace, inst);

    auto result = engine->pre_trade_check(replace, inst);
    EXPECT_TRUE(result.would_breach);
    EXPECT_TRUE(result.has_breach(LimitType::NET_DELTA));
}

// ============================================================================
// Test: Single metric pre-trade check
// ============================================================================

class PreTradeCheckSingleMetricTest : public ::testing::Test {
protected:
    using GrossDelta = GrossDeltaMetric<UnderlyerKey, StaticTestContext, InstrumentData, AllStages>;
    using NetDelta = NetDeltaMetric<UnderlyerKey, StaticTestContext, InstrumentData, AllStages>;
    using GlobalNotional = GlobalNotionalMetric<StaticTestContext, InstrumentData, AllStages>;

    using TestEngine = RiskAggregationEngineWithLimits<
        StaticTestContext,
        InstrumentData,
        GrossDelta,
        NetDelta,
        GlobalNotional
    >;

    StaticInstrumentProvider provider;
    std::unique_ptr<StaticTestContext> context;
    std::unique_ptr<TestEngine> engine;

    void SetUp() override {
        provider = create_option_provider();
        context = std::make_unique<StaticTestContext>(provider);
        engine = std::make_unique<TestEngine>(*context);
        // Gross delta limit: 500K (100 contracts = 750K, so it will breach)
        engine->set_default_limit<GrossDelta>(500000.0);
        // Net delta limit: 400K (100 contracts = 750K, so it will breach)
        engine->set_default_limit<NetDelta>(400000.0);
        // Notional limit: 100K (100 contracts @ $5 = 50K, won't breach)
        engine->set_limit<GlobalNotional>(GlobalKey::instance(), 100000.0);
    }

    // Helper to get instrument from provider
    InstrumentData get_instrument(const std::string& symbol) const {
        return provider.get_instrument(symbol);
    }
};

TEST_F(PreTradeCheckSingleMetricTest, SingleMetricCheckNewOrder) {
    // Order: 100 contracts of AAPL_OPT1
    // Gross delta = 100 * 0.5 * 100 * 150 = 750,000 (above 500K limit)
    // Net delta = 750,000 (above 400K limit)
    // Notional = 100 * 100 * 5.0 = 50,000 (within 100K limit)
    auto order = create_order("ORD001", "AAPL_OPT1", "AAPL", Side::BID, 5.0, 100);
    auto inst = get_instrument(order.symbol);

    // Full pre-trade check should breach both delta limits
    auto full_result = engine->pre_trade_check(order, inst);
    EXPECT_TRUE(full_result.would_breach);
    EXPECT_TRUE(full_result.has_breach(LimitType::GROSS_DELTA));
    EXPECT_TRUE(full_result.has_breach(LimitType::NET_DELTA));

    // Single metric check for gross delta - above limit
    auto gross_result = engine->pre_trade_check_single<GrossDelta>(order, inst);
    EXPECT_TRUE(gross_result.would_breach) << "Gross delta above limit";
    EXPECT_TRUE(gross_result.has_breach(LimitType::GROSS_DELTA));

    // Single metric check for net delta - above limit
    auto net_result = engine->pre_trade_check_single<NetDelta>(order, inst);
    EXPECT_TRUE(net_result.would_breach) << "Net delta above limit";
    EXPECT_TRUE(net_result.has_breach(LimitType::NET_DELTA));

    // Single metric check for notional - within limit
    auto notional_result = engine->pre_trade_check_single<GlobalNotional>(order, inst);
    EXPECT_FALSE(notional_result.would_breach) << "Notional within limit";
}

TEST_F(PreTradeCheckSingleMetricTest, SingleMetricCheckUpdate) {
    // Insert order: 50 contracts
    // Delta = 50 * 0.5 * 100 * 150 = 375,000 (within 500K gross, 400K net limits)
    auto order = create_order("ORD001", "AAPL_OPT1", "AAPL", Side::BID, 5.0, 50);
    auto inst = get_instrument(order.symbol);
    engine->on_new_order_single(order, inst);
    engine->on_execution_report(create_ack("ORD001", 50), inst);

    // Current state:
    // Gross delta = 375,000
    // Net delta = 375,000

    // Update to 100 contracts
    // New gross delta = 100 * 0.5 * 100 * 150 = 750,000
    // Gross delta contribution = 750,000 - 375,000 = +375,000
    // After: 375,000 + 375,000 = 750,000 > 500,000 limit
    auto replace = create_replace("ORD001_R", "ORD001", "AAPL_OPT1", Side::BID, 5.0, 100);
    engine->on_order_cancel_replace(replace, inst);

    // Single metric check for gross delta
    auto gross_result = engine->pre_trade_check_single<GrossDelta>(replace, inst);
    EXPECT_TRUE(gross_result.would_breach) << "Gross delta update should breach";
    EXPECT_TRUE(gross_result.has_breach(LimitType::GROSS_DELTA));

    // Single metric check for net delta
    auto net_result = engine->pre_trade_check_single<NetDelta>(replace, inst);
    EXPECT_TRUE(net_result.would_breach) << "Net delta update should breach";
}

// ============================================================================
// Test: Order count doesn't change on update
// ============================================================================

class PreTradeCheckUpdateOrderCountTest : public ::testing::Test {
protected:
    using OrderCount = OrderCountMetric<InstrumentSideKey, AllStages>;

    using TestEngine = RiskAggregationEngineWithLimits<
        void,
        void,
        OrderCount
    >;

    TestEngine engine;

    void SetUp() override {
        engine.set_default_limit<OrderCount>(1.0);  // 1 order per side limit
    }
};

TEST_F(PreTradeCheckUpdateOrderCountTest, UpdateDoesNotAffectOrderCount) {
    // Insert and ack order
    auto order = create_order("ORD001", "AAPL", "AAPL", Side::BID, 150.0, 100);
    engine.on_new_order_single(order);
    engine.on_execution_report(create_ack("ORD001", 100));

    // Now at limit (1 order)

    // A second new order should breach
    auto order2 = create_order("ORD002", "AAPL", "AAPL", Side::BID, 150.0, 50);
    auto new_order_result = engine.pre_trade_check(order2);
    EXPECT_TRUE(new_order_result.would_breach) << "New order should breach count limit";

    // But an update should NOT breach (count doesn't change)
    auto replace = create_replace("ORD001_R", "ORD001", "AAPL", Side::BID, 150.0, 200);
    engine.on_order_cancel_replace(replace);

    auto update_result = engine.pre_trade_check(replace);
    EXPECT_FALSE(update_result.would_breach) << "Update should not affect order count";
}

// ============================================================================
// Test: compute_update_contribution static methods
// ============================================================================

class ComputeUpdateContributionTest : public ::testing::Test {
protected:
    StaticInstrumentProvider provider;
    std::unique_ptr<StaticTestContext> context;

    void SetUp() override {
        provider = create_option_provider();
        context = std::make_unique<StaticTestContext>(provider);
    }
};

TEST_F(ComputeUpdateContributionTest, GrossDeltaContribution) {
    using GrossDelta = GrossDeltaMetric<UnderlyerKey, StaticTestContext, InstrumentData, AllStages>;

    TrackedOrder existing;
    existing.symbol = "AAPL_OPT1";
    existing.underlyer = "AAPL";
    existing.side = Side::BID;
    existing.leaves_qty = 100;

    OrderCancelReplaceRequest update;
    update.symbol = "AAPL_OPT1";
    update.side = Side::BID;
    update.quantity = 200;

    auto inst = provider.get_instrument("AAPL_OPT1");

    // Delta exposure = qty * delta * contract_size * underlyer_spot * fx_rate
    // Old gross delta = 100 * 0.5 * 100 * 150 = 750,000
    // New gross delta = 200 * 0.5 * 100 * 150 = 1,500,000
    // Contribution = 1,500,000 - 750,000 = 750,000
    double contribution = GrossDelta::compute_update_contribution(update, existing, inst, *context);
    EXPECT_DOUBLE_EQ(contribution, 750000.0);
}

TEST_F(ComputeUpdateContributionTest, NetDeltaContribution) {
    using NetDelta = NetDeltaMetric<UnderlyerKey, StaticTestContext, InstrumentData, AllStages>;

    TrackedOrder existing;
    existing.symbol = "AAPL_OPT1";
    existing.underlyer = "AAPL";
    existing.side = Side::BID;
    existing.leaves_qty = 100;

    OrderCancelReplaceRequest update;
    update.symbol = "AAPL_OPT1";
    update.side = Side::BID;
    update.quantity = 50;

    auto inst = provider.get_instrument("AAPL_OPT1");

    // Net delta exposure = qty * delta * contract_size * underlyer_spot * fx_rate (signed by side)
    // Old net delta (BID) = 100 * 0.5 * 100 * 150 = 750,000
    // New net delta (BID) = 50 * 0.5 * 100 * 150 = 375,000
    // Contribution = 375,000 - 750,000 = -375,000
    double contribution = NetDelta::compute_update_contribution(update, existing, inst, *context);
    EXPECT_DOUBLE_EQ(contribution, -375000.0);
}

TEST_F(ComputeUpdateContributionTest, NotionalContribution) {
    using Notional = GlobalNotionalMetric<StaticTestContext, InstrumentData, AllStages>;

    TrackedOrder existing;
    existing.symbol = "AAPL_OPT1";
    existing.underlyer = "AAPL";
    existing.side = Side::BID;
    existing.leaves_qty = 100;

    OrderCancelReplaceRequest update;
    update.symbol = "AAPL_OPT1";
    update.side = Side::BID;
    update.quantity = 150;

    auto inst = provider.get_instrument("AAPL_OPT1");

    // Old notional = 100 * 100 * 5.0 = 50000
    // New notional = 150 * 100 * 5.0 = 75000
    // Contribution = 75000 - 50000 = 25000
    double contribution = Notional::compute_update_contribution(update, existing, inst, *context);
    EXPECT_DOUBLE_EQ(contribution, 25000.0);
}

TEST_F(ComputeUpdateContributionTest, OrderCountContributionIsZero) {
    using OrderCount = OrderCountMetric<InstrumentSideKey, AllStages>;

    TrackedOrder existing;
    existing.symbol = "AAPL";
    existing.side = Side::BID;
    existing.leaves_qty = 100;

    OrderCancelReplaceRequest update;
    update.symbol = "AAPL";
    update.side = Side::BID;
    update.quantity = 200;

    // Order count doesn't change on update (no context needed for order count)
    int64_t contribution = OrderCount::compute_update_contribution(update, existing);
    EXPECT_EQ(contribution, 0);
}

// ============================================================================
// Test: Full flow with updates and pre-trade checks
// ============================================================================

class PreTradeCheckUpdateFullFlowTest : public ::testing::Test {
protected:
    using GlobalNotional = GlobalNotionalMetric<SimpleTestContext, InstrumentData, OpenStage, InFlightStage>;

    using TestEngine = RiskAggregationEngineWithLimits<
        SimpleTestContext,
        InstrumentData,
        GlobalNotional
    >;

    SimpleInstrumentProvider provider;
    std::unique_ptr<SimpleTestContext> context;
    std::unique_ptr<TestEngine> engine;

    void SetUp() override {
        provider = create_stock_provider();
        context = std::make_unique<SimpleTestContext>(provider);
        engine = std::make_unique<TestEngine>(*context);
        engine->set_limit<GlobalNotional>(GlobalKey::instance(), 50000.0);
    }

    double notional() const {
        return engine->get_metric<GlobalNotional>().get(GlobalKey::instance());
    }

    // Helper to get instrument from provider
    InstrumentData get_instrument(const std::string& symbol) const {
        return provider.get_instrument(symbol);
    }
};

TEST_F(PreTradeCheckUpdateFullFlowTest, CheckBeforeSendingUpdate) {
    // Step 1: Insert order
    auto order = create_order("ORD001", "AAPL", "AAPL", Side::BID, 150.0, 100);
    auto inst = get_instrument(order.symbol);
    engine->on_new_order_single(order, inst);
    engine->on_execution_report(create_ack("ORD001", 100), inst);
    EXPECT_DOUBLE_EQ(notional(), 15000.0);

    // Step 2: Check update BEFORE sending
    auto replace = create_replace("ORD001_R", "ORD001", "AAPL", Side::BID, 150.0, 400);

    // Pre-check: 15000 + (60000 - 15000) = 60000 > 50000
    auto check1 = engine->pre_trade_check(replace, inst);
    EXPECT_TRUE(check1.would_breach) << "Pre-check should catch breach";

    // Step 3: Try smaller update
    auto replace2 = create_replace("ORD001_R", "ORD001", "AAPL", Side::BID, 150.0, 300);

    // Pre-check: 15000 + (45000 - 15000) = 45000 < 50000
    auto check2 = engine->pre_trade_check(replace2, inst);
    EXPECT_FALSE(check2.would_breach) << "Smaller update should pass";

    // Step 4: Send the valid update
    engine->on_order_cancel_replace(replace2, inst);

    // Step 5: ACK the update
    engine->on_execution_report(create_replace_ack("ORD001_R", "ORD001", 300), inst);
    EXPECT_DOUBLE_EQ(notional(), 45000.0);
}

TEST_F(PreTradeCheckUpdateFullFlowTest, RejectedUpdateDoesNotAffectMetrics) {
    // Insert order
    auto order = create_order("ORD001", "AAPL", "AAPL", Side::BID, 150.0, 100);
    auto inst = get_instrument(order.symbol);
    engine->on_new_order_single(order, inst);
    engine->on_execution_report(create_ack("ORD001", 100), inst);
    EXPECT_DOUBLE_EQ(notional(), 15000.0);

    // Send update
    auto replace = create_replace("ORD001_R", "ORD001", "AAPL", Side::BID, 150.0, 200);
    engine->on_order_cancel_replace(replace, inst);

    // NACK the update
    engine->on_order_cancel_reject(create_replace_nack("ORD001_R", "ORD001"), inst);

    // Notional should be unchanged
    EXPECT_DOUBLE_EQ(notional(), 15000.0) << "Rejected update should not change metrics";
}
