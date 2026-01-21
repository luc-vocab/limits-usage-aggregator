#include <gtest/gtest.h>
#include <vector>
#include <string>
#include <variant>
#include <functional>
#include "../src/engine/risk_engine.hpp"
#include "../src/engine/risk_engine_with_limits.hpp"
#include "../src/fix/fix_parser.hpp"

using namespace engine;
using namespace fix;

// ============================================================================
// Order Step Definitions for Parameterized Tests
// ============================================================================

namespace {

// Actions that can be taken in a test scenario
enum class OrderAction {
    INSERT,             // Send NewOrderSingle
    ACK,                // Receive InsertAck
    NACK,               // Receive InsertNack (rejected)
    CANCEL_REQUEST,     // Send OrderCancelRequest
    CANCEL_ACK,         // Receive CancelAck
    CANCEL_NACK,        // Receive CancelNack (cancel rejected)
    REPLACE_REQUEST,    // Send OrderCancelReplaceRequest
    REPLACE_ACK,        // Receive ReplaceAck (UpdateAck)
    REPLACE_NACK,       // Receive ReplaceNack (UpdateNack)
    PARTIAL_FILL,       // Receive PartialFill
    FULL_FILL,          // Receive FullFill
    UNSOLICITED_CANCEL  // Receive UnsolicitedCancel
};

inline const char* to_string(OrderAction action) {
    switch (action) {
        case OrderAction::INSERT: return "INSERT";
        case OrderAction::ACK: return "ACK";
        case OrderAction::NACK: return "NACK";
        case OrderAction::CANCEL_REQUEST: return "CANCEL_REQUEST";
        case OrderAction::CANCEL_ACK: return "CANCEL_ACK";
        case OrderAction::CANCEL_NACK: return "CANCEL_NACK";
        case OrderAction::REPLACE_REQUEST: return "REPLACE_REQUEST";
        case OrderAction::REPLACE_ACK: return "REPLACE_ACK";
        case OrderAction::REPLACE_NACK: return "REPLACE_NACK";
        case OrderAction::PARTIAL_FILL: return "PARTIAL_FILL";
        case OrderAction::FULL_FILL: return "FULL_FILL";
        case OrderAction::UNSOLICITED_CANCEL: return "UNSOLICITED_CANCEL";
        default: return "UNKNOWN";
    }
}

// A single step in an order scenario
struct OrderStep {
    OrderAction action;
    std::string order_id;        // ClOrdID for the order
    std::string symbol;          // Instrument symbol
    std::string underlyer;       // Underlyer symbol
    Side side = Side::BID;       // BID or ASK
    double price = 0.0;          // Order price
    double quantity = 0.0;       // Order quantity
    double delta = 1.0;          // Delta per contract
    double fill_qty = 0.0;       // For fills: quantity filled
    double new_price = 0.0;      // For replace: new price
    double new_quantity = 0.0;   // For replace: new quantity
    bool expect_limit_breach = false;  // Whether this step should trigger a limit breach

    // Builder pattern for fluent API
    OrderStep& with_symbol(const std::string& s) { symbol = s; return *this; }
    OrderStep& with_underlyer(const std::string& u) { underlyer = u; return *this; }
    OrderStep& with_side(Side s) { side = s; return *this; }
    OrderStep& with_price(double p) { price = p; return *this; }
    OrderStep& with_quantity(double q) { quantity = q; return *this; }
    OrderStep& with_delta(double d) { delta = d; return *this; }
    OrderStep& with_fill_qty(double fq) { fill_qty = fq; return *this; }
    OrderStep& with_new_price(double np) { new_price = np; return *this; }
    OrderStep& with_new_quantity(double nq) { new_quantity = nq; return *this; }
    OrderStep& expect_breach() { expect_limit_breach = true; return *this; }
};

// Factory functions for creating order steps
OrderStep insert(const std::string& order_id) {
    return OrderStep{OrderAction::INSERT, order_id};
}

OrderStep ack(const std::string& order_id) {
    return OrderStep{OrderAction::ACK, order_id};
}

OrderStep nack(const std::string& order_id) {
    return OrderStep{OrderAction::NACK, order_id};
}

OrderStep cancel_request(const std::string& order_id) {
    return OrderStep{OrderAction::CANCEL_REQUEST, order_id};
}

OrderStep cancel_ack(const std::string& order_id) {
    return OrderStep{OrderAction::CANCEL_ACK, order_id};
}

OrderStep cancel_nack(const std::string& order_id) {
    return OrderStep{OrderAction::CANCEL_NACK, order_id};
}

OrderStep replace_request(const std::string& order_id) {
    return OrderStep{OrderAction::REPLACE_REQUEST, order_id};
}

OrderStep replace_ack(const std::string& order_id) {
    return OrderStep{OrderAction::REPLACE_ACK, order_id};
}

OrderStep replace_nack(const std::string& order_id) {
    return OrderStep{OrderAction::REPLACE_NACK, order_id};
}

OrderStep partial_fill(const std::string& order_id) {
    return OrderStep{OrderAction::PARTIAL_FILL, order_id};
}

OrderStep full_fill(const std::string& order_id) {
    return OrderStep{OrderAction::FULL_FILL, order_id};
}

OrderStep unsolicited_cancel(const std::string& order_id) {
    return OrderStep{OrderAction::UNSOLICITED_CANCEL, order_id};
}

// ============================================================================
// Underlyer Order Count Limit Engine
// ============================================================================
//
// This class demonstrates the use of RiskAggregationEngineWithLimits for
// tracking and enforcing limits on quoted instruments per underlyer.
//

class UnderlyerLimitEngine {
private:
    // Use the new engine with limits support
    RiskAggregationEngineWithLimits<
        metrics::DeltaMetrics,
        metrics::OrderCountMetrics,
        metrics::NotionalMetrics
    > risk_engine_;

    // Track pending orders by ID for message construction
    std::unordered_map<std::string, NewOrderSingle> pending_orders_;
    std::unordered_map<std::string, std::string> cancel_request_map_;  // cancel_id -> orig_id
    std::unordered_map<std::string, std::string> replace_request_map_; // new_id -> orig_id
    int replace_counter_ = 0;
    int cancel_counter_ = 0;

public:
    void set_underlyer_limit(const std::string& underlyer, int64_t limit) {
        risk_engine_.set_quoted_instruments_limit(underlyer, static_cast<double>(limit));
    }

    void set_default_limit(int64_t limit) {
        risk_engine_.set_default_quoted_instruments_limit(static_cast<double>(limit));
    }

    int64_t get_limit(const std::string& underlyer) const {
        return static_cast<int64_t>(risk_engine_.get_quoted_instruments_limit(underlyer));
    }

    int64_t get_open_order_count(const std::string& underlyer) const {
        return risk_engine_.quoted_instruments_count(underlyer);
    }

    // Check if a symbol is already quoted (has at least one order)
    bool is_instrument_quoted(const std::string& symbol) const {
        return risk_engine_.is_instrument_quoted(symbol);
    }

    // Check if adding an order on a specific instrument would breach the limit
    // Uses the new would_breach_quoted_instruments_limit from RiskAggregationEngineWithLimits
    bool would_breach_limit(const std::string& underlyer, const std::string& symbol) const {
        return risk_engine_.would_breach_quoted_instruments_limit(underlyer, symbol);
    }

    // Process an order step, return true if approved, false if rejected due to limit
    struct StepResult {
        bool approved = true;
        bool limit_breached = false;
        std::string message;
    };

    StepResult process_step(const OrderStep& step) {
        StepResult result;

        switch (step.action) {
            case OrderAction::INSERT: {
                // Check limit before inserting (considers if instrument is already quoted)
                if (would_breach_limit(step.underlyer, step.symbol)) {
                    result.approved = false;
                    result.limit_breached = true;
                    result.message = "Order rejected: underlyer " + step.underlyer +
                                   " at limit (" + std::to_string(get_limit(step.underlyer)) + ")";
                    return result;
                }

                NewOrderSingle order;
                order.key.cl_ord_id = step.order_id;
                order.symbol = step.symbol;
                order.underlyer = step.underlyer;
                order.side = step.side;
                order.price = step.price;
                order.quantity = step.quantity;
                order.delta = step.delta;
                order.strategy_id = "STRAT1";
                order.portfolio_id = "PORT1";

                pending_orders_[step.order_id] = order;
                risk_engine_.on_new_order_single(order);
                result.message = "Order inserted: " + step.order_id;
                break;
            }

            case OrderAction::ACK: {
                auto it = pending_orders_.find(step.order_id);
                if (it == pending_orders_.end()) {
                    result.approved = false;
                    result.message = "Order not found: " + step.order_id;
                    return result;
                }

                ExecutionReport report;
                report.key.cl_ord_id = step.order_id;
                report.order_id = "EX" + step.order_id;
                report.ord_status = OrdStatus::NEW;
                report.exec_type = ExecType::NEW;
                report.leaves_qty = it->second.quantity;
                report.cum_qty = 0;
                report.is_unsolicited = false;

                risk_engine_.on_execution_report(report);
                result.message = "Order acknowledged: " + step.order_id;
                break;
            }

            case OrderAction::NACK: {
                auto it = pending_orders_.find(step.order_id);
                if (it == pending_orders_.end()) {
                    result.approved = false;
                    result.message = "Order not found: " + step.order_id;
                    return result;
                }

                ExecutionReport report;
                report.key.cl_ord_id = step.order_id;
                report.order_id = "EX" + step.order_id;
                report.ord_status = OrdStatus::REJECTED;
                report.exec_type = ExecType::REJECTED;
                report.leaves_qty = 0;
                report.cum_qty = 0;
                report.is_unsolicited = false;

                risk_engine_.on_execution_report(report);
                pending_orders_.erase(it);
                result.message = "Order rejected: " + step.order_id;
                break;
            }

            case OrderAction::CANCEL_REQUEST: {
                auto it = pending_orders_.find(step.order_id);
                if (it == pending_orders_.end()) {
                    result.approved = false;
                    result.message = "Order not found: " + step.order_id;
                    return result;
                }

                std::string cancel_id = "CXL" + std::to_string(++cancel_counter_);
                cancel_request_map_[cancel_id] = step.order_id;

                OrderCancelRequest cancel_req;
                cancel_req.key.cl_ord_id = cancel_id;
                cancel_req.orig_key.cl_ord_id = step.order_id;
                cancel_req.symbol = it->second.symbol;
                cancel_req.side = it->second.side;

                risk_engine_.on_order_cancel_request(cancel_req);
                result.message = "Cancel request sent: " + cancel_id + " for " + step.order_id;
                break;
            }

            case OrderAction::CANCEL_ACK: {
                auto it = pending_orders_.find(step.order_id);
                if (it == pending_orders_.end()) {
                    result.approved = false;
                    result.message = "Order not found: " + step.order_id;
                    return result;
                }

                // Find the cancel request ID
                std::string cancel_id;
                for (const auto& [cid, oid] : cancel_request_map_) {
                    if (oid == step.order_id) {
                        cancel_id = cid;
                        break;
                    }
                }

                ExecutionReport report;
                report.key.cl_ord_id = cancel_id.empty() ? step.order_id : cancel_id;
                report.order_id = "EX" + step.order_id;
                report.ord_status = OrdStatus::CANCELED;
                report.exec_type = ExecType::CANCELED;
                report.leaves_qty = 0;
                report.cum_qty = it->second.quantity - it->second.quantity;  // No fills
                report.is_unsolicited = false;
                if (!cancel_id.empty()) {
                    report.orig_key = OrderKey{step.order_id};
                }

                risk_engine_.on_execution_report(report);
                pending_orders_.erase(it);
                result.message = "Order canceled: " + step.order_id;
                break;
            }

            case OrderAction::CANCEL_NACK: {
                auto it = pending_orders_.find(step.order_id);
                if (it == pending_orders_.end()) {
                    result.approved = false;
                    result.message = "Order not found: " + step.order_id;
                    return result;
                }

                // Find the cancel request ID
                std::string cancel_id;
                for (const auto& [cid, oid] : cancel_request_map_) {
                    if (oid == step.order_id) {
                        cancel_id = cid;
                        break;
                    }
                }

                OrderCancelReject reject;
                reject.key.cl_ord_id = cancel_id.empty() ? step.order_id : cancel_id;
                reject.orig_key.cl_ord_id = step.order_id;
                reject.order_id = "EX" + step.order_id;
                reject.ord_status = OrdStatus::NEW;
                reject.response_to = CxlRejResponseTo::ORDER_CANCEL_REQUEST;
                reject.cxl_rej_reason = 0;

                risk_engine_.on_order_cancel_reject(reject);
                result.message = "Cancel rejected for: " + step.order_id;
                break;
            }

            case OrderAction::REPLACE_REQUEST: {
                auto it = pending_orders_.find(step.order_id);
                if (it == pending_orders_.end()) {
                    result.approved = false;
                    result.message = "Order not found: " + step.order_id;
                    return result;
                }

                std::string new_id = "RPL" + std::to_string(++replace_counter_);
                replace_request_map_[new_id] = step.order_id;

                OrderCancelReplaceRequest replace_req;
                replace_req.key.cl_ord_id = new_id;
                replace_req.orig_key.cl_ord_id = step.order_id;
                replace_req.symbol = it->second.symbol;
                replace_req.side = it->second.side;
                replace_req.price = step.new_price > 0 ? step.new_price : it->second.price;
                replace_req.quantity = step.new_quantity > 0 ? step.new_quantity : it->second.quantity;

                risk_engine_.on_order_cancel_replace(replace_req);
                result.message = "Replace request sent: " + new_id + " for " + step.order_id;
                break;
            }

            case OrderAction::REPLACE_ACK: {
                auto it = pending_orders_.find(step.order_id);
                if (it == pending_orders_.end()) {
                    result.approved = false;
                    result.message = "Order not found: " + step.order_id;
                    return result;
                }

                // Find the replace request ID
                std::string new_id;
                for (const auto& [nid, oid] : replace_request_map_) {
                    if (oid == step.order_id) {
                        new_id = nid;
                        break;
                    }
                }

                double new_qty = step.new_quantity > 0 ? step.new_quantity : it->second.quantity;
                double new_price = step.new_price > 0 ? step.new_price : it->second.price;

                ExecutionReport report;
                report.key.cl_ord_id = new_id.empty() ? step.order_id : new_id;
                report.order_id = "EX" + step.order_id;
                report.ord_status = OrdStatus::NEW;
                report.exec_type = ExecType::REPLACED;
                report.leaves_qty = new_qty;
                report.cum_qty = 0;
                report.is_unsolicited = false;
                report.orig_key = OrderKey{step.order_id};

                // Update the pending order with new values
                it->second.price = new_price;
                it->second.quantity = new_qty;

                // Remap to new ID
                if (!new_id.empty()) {
                    pending_orders_[new_id] = it->second;
                    pending_orders_[new_id].key.cl_ord_id = new_id;
                    pending_orders_.erase(it);
                }

                risk_engine_.on_execution_report(report);
                result.message = "Replace acknowledged: " + step.order_id + " -> " + new_id;
                break;
            }

            case OrderAction::REPLACE_NACK: {
                auto it = pending_orders_.find(step.order_id);
                if (it == pending_orders_.end()) {
                    result.approved = false;
                    result.message = "Order not found: " + step.order_id;
                    return result;
                }

                // Find the replace request ID
                std::string new_id;
                for (const auto& [nid, oid] : replace_request_map_) {
                    if (oid == step.order_id) {
                        new_id = nid;
                        break;
                    }
                }

                OrderCancelReject reject;
                reject.key.cl_ord_id = new_id.empty() ? step.order_id : new_id;
                reject.orig_key.cl_ord_id = step.order_id;
                reject.order_id = "EX" + step.order_id;
                reject.ord_status = OrdStatus::NEW;
                reject.response_to = CxlRejResponseTo::ORDER_CANCEL_REPLACE_REQUEST;
                reject.cxl_rej_reason = 0;

                risk_engine_.on_order_cancel_reject(reject);
                result.message = "Replace rejected for: " + step.order_id;
                break;
            }

            case OrderAction::PARTIAL_FILL: {
                auto it = pending_orders_.find(step.order_id);
                if (it == pending_orders_.end()) {
                    result.approved = false;
                    result.message = "Order not found: " + step.order_id;
                    return result;
                }

                double fill_qty = step.fill_qty > 0 ? step.fill_qty : it->second.quantity / 2;

                ExecutionReport report;
                report.key.cl_ord_id = step.order_id;
                report.order_id = "EX" + step.order_id;
                report.ord_status = OrdStatus::PARTIALLY_FILLED;
                report.exec_type = ExecType::PARTIAL_FILL;
                report.leaves_qty = it->second.quantity - fill_qty;
                report.cum_qty = fill_qty;
                report.last_qty = fill_qty;
                report.last_px = it->second.price;
                report.is_unsolicited = false;

                // Update pending order
                it->second.quantity -= fill_qty;

                risk_engine_.on_execution_report(report);
                result.message = "Partial fill: " + step.order_id + " qty=" + std::to_string(fill_qty);
                break;
            }

            case OrderAction::FULL_FILL: {
                auto it = pending_orders_.find(step.order_id);
                if (it == pending_orders_.end()) {
                    result.approved = false;
                    result.message = "Order not found: " + step.order_id;
                    return result;
                }

                double fill_qty = step.fill_qty > 0 ? step.fill_qty : it->second.quantity;

                ExecutionReport report;
                report.key.cl_ord_id = step.order_id;
                report.order_id = "EX" + step.order_id;
                report.ord_status = OrdStatus::FILLED;
                report.exec_type = ExecType::FILL;
                report.leaves_qty = 0;
                report.cum_qty = fill_qty;
                report.last_qty = fill_qty;
                report.last_px = it->second.price;
                report.is_unsolicited = false;

                risk_engine_.on_execution_report(report);
                pending_orders_.erase(it);
                result.message = "Full fill: " + step.order_id;
                break;
            }

            case OrderAction::UNSOLICITED_CANCEL: {
                auto it = pending_orders_.find(step.order_id);
                if (it == pending_orders_.end()) {
                    result.approved = false;
                    result.message = "Order not found: " + step.order_id;
                    return result;
                }

                ExecutionReport report;
                report.key.cl_ord_id = step.order_id;
                report.order_id = "EX" + step.order_id;
                report.ord_status = OrdStatus::CANCELED;
                report.exec_type = ExecType::CANCELED;
                report.leaves_qty = 0;
                report.cum_qty = 0;
                report.is_unsolicited = true;

                risk_engine_.on_execution_report(report);
                pending_orders_.erase(it);
                result.message = "Unsolicited cancel: " + step.order_id;
                break;
            }
        }

        return result;
    }

    // Return const reference to the underlying engine (with limits)
    const auto& risk_engine() const { return risk_engine_; }
    auto& risk_engine() { return risk_engine_; }

    void clear() {
        risk_engine_.clear();  // This also clears all limits
        pending_orders_.clear();
        cancel_request_map_.clear();
        replace_request_map_.clear();
        replace_counter_ = 0;
        cancel_counter_ = 0;
    }
};

// ============================================================================
// Test Scenario Definition
// ============================================================================

struct TestScenario {
    std::string name;
    std::unordered_map<std::string, int64_t> underlyer_limits;
    std::vector<OrderStep> steps;
    std::unordered_map<std::string, int64_t> expected_open_counts;  // underlyer -> expected count at end
};

}  // namespace

// ============================================================================
// Parameterized Test Fixture
// ============================================================================

class UnderlyerOrderCountTest : public ::testing::TestWithParam<TestScenario> {
protected:
    UnderlyerLimitEngine engine;

    void SetUp() override {
        engine.clear();
    }
};

TEST_P(UnderlyerOrderCountTest, ScenarioExecution) {
    const auto& scenario = GetParam();

    // Set up limits
    for (const auto& [underlyer, limit] : scenario.underlyer_limits) {
        engine.set_underlyer_limit(underlyer, limit);
    }

    // Execute steps
    for (size_t i = 0; i < scenario.steps.size(); ++i) {
        const auto& step = scenario.steps[i];
        auto result = engine.process_step(step);

        // Verify breach expectation
        if (step.expect_limit_breach) {
            EXPECT_TRUE(result.limit_breached)
                << "Step " << i << " (" << to_string(step.action) << " " << step.order_id
                << "): Expected limit breach but order was approved";
        } else {
            EXPECT_FALSE(result.limit_breached)
                << "Step " << i << " (" << to_string(step.action) << " " << step.order_id
                << "): Unexpected limit breach: " << result.message;
        }
    }

    // Verify final state
    for (const auto& [underlyer, expected_count] : scenario.expected_open_counts) {
        EXPECT_EQ(engine.get_open_order_count(underlyer), expected_count)
            << "Underlyer " << underlyer << " has wrong open order count";
    }
}

// ============================================================================
// Test Scenarios
// ============================================================================

INSTANTIATE_TEST_SUITE_P(
    BasicInsertAck,
    UnderlyerOrderCountTest,
    ::testing::Values(
        TestScenario{
            "SingleOrderInsertAck",
            {{"AAPL", 5}},
            {
                insert("ORD001").with_symbol("AAPL230120C150").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                ack("ORD001")
            },
            {{"AAPL", 1}}
        },
        TestScenario{
            "MultipleOrdersUnderLimit",
            {{"AAPL", 5}},
            {
                insert("ORD001").with_symbol("AAPL230120C150").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                ack("ORD001"),
                insert("ORD002").with_symbol("AAPL230120C155").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(6.0).with_quantity(50),
                ack("ORD002"),
                insert("ORD003").with_symbol("AAPL230120P145").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(3.0).with_quantity(75),
                ack("ORD003")
            },
            {{"AAPL", 3}}
        }
    ),
    [](const ::testing::TestParamInfo<TestScenario>& info) {
        return info.param.name;
    }
);

INSTANTIATE_TEST_SUITE_P(
    LimitEnforcement,
    UnderlyerOrderCountTest,
    ::testing::Values(
        TestScenario{
            "ReachLimitExactly",
            {{"AAPL", 2}},
            {
                insert("ORD001").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                ack("ORD001"),
                insert("ORD002").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(6.0).with_quantity(50),
                ack("ORD002")
            },
            {{"AAPL", 2}}
        },
        TestScenario{
            "ExceedLimitRejected",
            {{"AAPL", 2}},
            {
                insert("ORD001").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                ack("ORD001"),
                insert("ORD002").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(6.0).with_quantity(50),
                ack("ORD002"),
                insert("ORD003").with_symbol("AAPL_OPT3").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(4.0).with_quantity(25).expect_breach()
            },
            {{"AAPL", 2}}
        },
        TestScenario{
            "MultipleUnderlyersIndependentLimits",
            {{"AAPL", 2}, {"MSFT", 3}},
            {
                // AAPL orders
                insert("ORD001").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                ack("ORD001"),
                insert("ORD002").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(6.0).with_quantity(50),
                ack("ORD002"),
                // AAPL limit reached, this should be rejected
                insert("ORD003").with_symbol("AAPL_OPT3").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(4.0).with_quantity(25).expect_breach(),
                // MSFT orders should still work
                insert("ORD004").with_symbol("MSFT_OPT1").with_underlyer("MSFT")
                    .with_side(Side::BID).with_price(10.0).with_quantity(200),
                ack("ORD004"),
                insert("ORD005").with_symbol("MSFT_OPT2").with_underlyer("MSFT")
                    .with_side(Side::ASK).with_price(11.0).with_quantity(150),
                ack("ORD005")
            },
            {{"AAPL", 2}, {"MSFT", 2}}
        }
    ),
    [](const ::testing::TestParamInfo<TestScenario>& info) {
        return info.param.name;
    }
);

INSTANTIATE_TEST_SUITE_P(
    CancelFlow,
    UnderlyerOrderCountTest,
    ::testing::Values(
        TestScenario{
            "CancelFreesCapacity",
            {{"AAPL", 2}},
            {
                insert("ORD001").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                ack("ORD001"),
                insert("ORD002").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(6.0).with_quantity(50),
                ack("ORD002"),
                // Limit reached, cancel one order
                cancel_request("ORD001"),
                cancel_ack("ORD001"),
                // Now we can insert again
                insert("ORD003").with_symbol("AAPL_OPT3").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(4.0).with_quantity(25),
                ack("ORD003")
            },
            {{"AAPL", 2}}
        },
        TestScenario{
            "CancelNackKeepsOrder",
            {{"AAPL", 2}},
            {
                insert("ORD001").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                ack("ORD001"),
                insert("ORD002").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(6.0).with_quantity(50),
                ack("ORD002"),
                // Try to cancel but get rejected
                cancel_request("ORD001"),
                cancel_nack("ORD001"),
                // Still at limit
                insert("ORD003").with_symbol("AAPL_OPT3").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(4.0).with_quantity(25).expect_breach()
            },
            {{"AAPL", 2}}
        }
    ),
    [](const ::testing::TestParamInfo<TestScenario>& info) {
        return info.param.name;
    }
);

INSTANTIATE_TEST_SUITE_P(
    ReplaceFlow,
    UnderlyerOrderCountTest,
    ::testing::Values(
        TestScenario{
            "ReplaceDoesNotChangeCount",
            {{"AAPL", 2}},
            {
                insert("ORD001").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                ack("ORD001"),
                insert("ORD002").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(6.0).with_quantity(50),
                ack("ORD002"),
                // Replace ORD001 - should not change count
                replace_request("ORD001").with_new_price(5.5).with_new_quantity(150),
                replace_ack("ORD001").with_new_price(5.5).with_new_quantity(150),
                // Still at limit
                insert("ORD003").with_symbol("AAPL_OPT3").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(4.0).with_quantity(25).expect_breach()
            },
            {{"AAPL", 2}}
        },
        TestScenario{
            "ReplaceNackKeepsOriginal",
            {{"AAPL", 2}},
            {
                insert("ORD001").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                ack("ORD001"),
                insert("ORD002").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(6.0).with_quantity(50),
                ack("ORD002"),
                // Try to replace but get rejected
                replace_request("ORD001").with_new_price(5.5).with_new_quantity(150),
                replace_nack("ORD001"),
                // Still at limit
                insert("ORD003").with_symbol("AAPL_OPT3").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(4.0).with_quantity(25).expect_breach()
            },
            {{"AAPL", 2}}
        }
    ),
    [](const ::testing::TestParamInfo<TestScenario>& info) {
        return info.param.name;
    }
);

INSTANTIATE_TEST_SUITE_P(
    FillFlow,
    UnderlyerOrderCountTest,
    ::testing::Values(
        TestScenario{
            "PartialFillKeepsOrder",
            {{"AAPL", 2}},
            {
                insert("ORD001").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                ack("ORD001"),
                insert("ORD002").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(6.0).with_quantity(50),
                ack("ORD002"),
                // Partial fill doesn't free capacity
                partial_fill("ORD001").with_fill_qty(50),
                // Still at limit
                insert("ORD003").with_symbol("AAPL_OPT3").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(4.0).with_quantity(25).expect_breach()
            },
            {{"AAPL", 2}}
        },
        TestScenario{
            "FullFillFreesCapacity",
            {{"AAPL", 2}},
            {
                insert("ORD001").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                ack("ORD001"),
                insert("ORD002").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(6.0).with_quantity(50),
                ack("ORD002"),
                // Full fill frees capacity
                full_fill("ORD001"),
                // Now we can insert again
                insert("ORD003").with_symbol("AAPL_OPT3").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(4.0).with_quantity(25),
                ack("ORD003")
            },
            {{"AAPL", 2}}
        },
        TestScenario{
            "PartialThenFullFill",
            {{"AAPL", 2}},
            {
                insert("ORD001").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                ack("ORD001"),
                insert("ORD002").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(6.0).with_quantity(50),
                ack("ORD002"),
                // Partial fill
                partial_fill("ORD001").with_fill_qty(50),
                // Still at limit
                insert("ORD003").with_symbol("AAPL_OPT3").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(4.0).with_quantity(25).expect_breach(),
                // Complete the fill
                full_fill("ORD001").with_fill_qty(50),
                // Now we can insert
                insert("ORD004").with_symbol("AAPL_OPT4").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(4.0).with_quantity(25),
                ack("ORD004")
            },
            {{"AAPL", 2}}
        }
    ),
    [](const ::testing::TestParamInfo<TestScenario>& info) {
        return info.param.name;
    }
);

INSTANTIATE_TEST_SUITE_P(
    UnsolicitedCancelFlow,
    UnderlyerOrderCountTest,
    ::testing::Values(
        TestScenario{
            "UnsolicitedCancelFreesCapacity",
            {{"AAPL", 2}},
            {
                insert("ORD001").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                ack("ORD001"),
                insert("ORD002").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(6.0).with_quantity(50),
                ack("ORD002"),
                // Unsolicited cancel from exchange
                unsolicited_cancel("ORD001"),
                // Now we can insert again
                insert("ORD003").with_symbol("AAPL_OPT3").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(4.0).with_quantity(25),
                ack("ORD003")
            },
            {{"AAPL", 2}}
        }
    ),
    [](const ::testing::TestParamInfo<TestScenario>& info) {
        return info.param.name;
    }
);

INSTANTIATE_TEST_SUITE_P(
    NackFlow,
    UnderlyerOrderCountTest,
    ::testing::Values(
        TestScenario{
            "InsertNackFreesCapacity",
            {{"AAPL", 2}},
            {
                insert("ORD001").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                ack("ORD001"),
                insert("ORD002").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(6.0).with_quantity(50),
                // ORD002 gets rejected
                nack("ORD002"),
                // Only ORD001 is open, so we can add another
                insert("ORD003").with_symbol("AAPL_OPT3").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(4.0).with_quantity(25),
                ack("ORD003")
            },
            {{"AAPL", 2}}
        },
        TestScenario{
            "PendingOrderCountsTowardsLimit",
            {{"AAPL", 2}},
            {
                insert("ORD001").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                // Don't ack yet, insert another
                insert("ORD002").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(6.0).with_quantity(50),
                // Both pending, at limit
                insert("ORD003").with_symbol("AAPL_OPT3").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(4.0).with_quantity(25).expect_breach(),
                // Now ack them
                ack("ORD001"),
                ack("ORD002")
            },
            {{"AAPL", 2}}
        }
    ),
    [](const ::testing::TestParamInfo<TestScenario>& info) {
        return info.param.name;
    }
);

INSTANTIATE_TEST_SUITE_P(
    BidAskMixed,
    UnderlyerOrderCountTest,
    ::testing::Values(
        TestScenario{
            "MixedBidAskOrders",
            {{"AAPL", 2}},  // Limit of 2 unique instruments
            {
                // OPT1: bid and ask orders (counts as 1 instrument)
                insert("ORD001").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                ack("ORD001"),
                insert("ORD002").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(5.5).with_quantity(100),
                ack("ORD002"),
                // OPT2: bid and ask orders (counts as 1 more instrument, now at limit)
                insert("ORD003").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(3.0).with_quantity(50),
                ack("ORD003"),
                insert("ORD004").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(3.5).with_quantity(50),
                ack("ORD004"),
                // OPT3: new instrument, should breach limit
                insert("ORD005").with_symbol("AAPL_OPT3").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(2.0).with_quantity(25).expect_breach()
            },
            {{"AAPL", 2}}  // quoted_instruments_count counts unique instruments, not orders
        },
        TestScenario{
            "SameInstrumentMultipleOrders",
            {{"AAPL", 3}},
            {
                // Multiple orders on same instrument (bid and ask)
                insert("ORD001").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                ack("ORD001"),
                insert("ORD002").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(5.5).with_quantity(50),
                ack("ORD002"),
                insert("ORD003").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(4.9).with_quantity(75),
                ack("ORD003"),
                // Different instrument
                insert("ORD004").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(3.0).with_quantity(50),
                ack("ORD004"),
                insert("ORD005").with_symbol("AAPL_OPT3").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(2.0).with_quantity(25),
                ack("ORD005")
            },
            {{"AAPL", 3}}  // 3 unique instruments: OPT1, OPT2, OPT3
        }
    ),
    [](const ::testing::TestParamInfo<TestScenario>& info) {
        return info.param.name;
    }
);

INSTANTIATE_TEST_SUITE_P(
    ComplexScenarios,
    UnderlyerOrderCountTest,
    ::testing::Values(
        TestScenario{
            "FullLifecycleScenario",
            {{"AAPL", 3}, {"MSFT", 2}},
            {
                // Start with AAPL orders
                insert("ORD001").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                ack("ORD001"),
                insert("ORD002").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(6.0).with_quantity(50),
                ack("ORD002"),
                // Add MSFT orders
                insert("ORD003").with_symbol("MSFT_OPT1").with_underlyer("MSFT")
                    .with_side(Side::BID).with_price(10.0).with_quantity(200),
                ack("ORD003"),
                // Partial fill on AAPL order
                partial_fill("ORD001").with_fill_qty(50),
                // Replace AAPL order
                replace_request("ORD002").with_new_price(6.5).with_new_quantity(75),
                replace_ack("ORD002").with_new_price(6.5).with_new_quantity(75),
                // Add more AAPL
                insert("ORD004").with_symbol("AAPL_OPT3").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(4.0).with_quantity(25),
                ack("ORD004"),
                // AAPL at limit
                insert("ORD005").with_symbol("AAPL_OPT4").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(3.0).with_quantity(10).expect_breach(),
                // Cancel one AAPL
                cancel_request("ORD001"),
                cancel_ack("ORD001"),
                // Now can add AAPL
                insert("ORD006").with_symbol("AAPL_OPT4").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(3.0).with_quantity(10),
                ack("ORD006"),
                // MSFT gets unsolicited cancel
                unsolicited_cancel("ORD003"),
                // MSFT now empty, can add
                insert("ORD007").with_symbol("MSFT_OPT2").with_underlyer("MSFT")
                    .with_side(Side::BID).with_price(11.0).with_quantity(100),
                ack("ORD007")
            },
            {{"AAPL", 3}, {"MSFT", 1}}
        },
        TestScenario{
            "AllMessageTypesInSequence",
            {{"AAPL", 5}},
            {
                // INSERT -> ACK
                insert("ORD001").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(5.0).with_quantity(100),
                ack("ORD001"),
                // INSERT -> NACK
                insert("ORD002").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(6.0).with_quantity(50),
                nack("ORD002"),
                // INSERT -> ACK -> PARTIAL_FILL
                insert("ORD003").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(6.0).with_quantity(50),
                ack("ORD003"),
                partial_fill("ORD003").with_fill_qty(25),
                // INSERT -> ACK -> FULL_FILL
                insert("ORD004").with_symbol("AAPL_OPT3").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(4.0).with_quantity(30),
                ack("ORD004"),
                full_fill("ORD004"),
                // INSERT -> ACK -> CANCEL_REQUEST -> CANCEL_ACK
                insert("ORD005").with_symbol("AAPL_OPT3").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(4.0).with_quantity(30),
                ack("ORD005"),
                cancel_request("ORD005"),
                cancel_ack("ORD005"),
                // INSERT -> ACK -> CANCEL_REQUEST -> CANCEL_NACK
                insert("ORD006").with_symbol("AAPL_OPT4").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(3.0).with_quantity(20),
                ack("ORD006"),
                cancel_request("ORD006"),
                cancel_nack("ORD006"),
                // INSERT -> ACK -> REPLACE_REQUEST -> REPLACE_ACK
                insert("ORD007").with_symbol("AAPL_OPT5").with_underlyer("AAPL")
                    .with_side(Side::BID).with_price(2.0).with_quantity(15),
                ack("ORD007"),
                replace_request("ORD007").with_new_price(2.5).with_new_quantity(20),
                replace_ack("ORD007").with_new_price(2.5).with_new_quantity(20),
                // INSERT -> ACK -> UNSOLICITED_CANCEL
                insert("ORD008").with_symbol("AAPL_OPT6").with_underlyer("AAPL")
                    .with_side(Side::ASK).with_price(1.5).with_quantity(10),
                ack("ORD008"),
                unsolicited_cancel("ORD008")
            },
            {{"AAPL", 4}}  // ORD001, ORD003 (partial), ORD006 (cancel failed), RPL1 (replaced ORD007)
        }
    ),
    [](const ::testing::TestParamInfo<TestScenario>& info) {
        return info.param.name;
    }
);

// ============================================================================
// Unit Tests for UnderlyerLimitEngine
// ============================================================================

class UnderlyerLimitEngineTest : public ::testing::Test {
protected:
    UnderlyerLimitEngine engine;

    void SetUp() override {
        engine.clear();
    }
};

TEST_F(UnderlyerLimitEngineTest, DefaultLimitApplied) {
    engine.set_default_limit(5);
    EXPECT_EQ(engine.get_limit("UNKNOWN"), 5);
}

TEST_F(UnderlyerLimitEngineTest, SpecificLimitOverridesDefault) {
    engine.set_default_limit(10);
    engine.set_underlyer_limit("AAPL", 3);
    EXPECT_EQ(engine.get_limit("AAPL"), 3);
    EXPECT_EQ(engine.get_limit("MSFT"), 10);
}

TEST_F(UnderlyerLimitEngineTest, WouldBreachLimitAtZero) {
    engine.set_underlyer_limit("AAPL", 0);
    // New instrument would breach limit of 0
    EXPECT_TRUE(engine.would_breach_limit("AAPL", "AAPL_OPT1"));
}

TEST_F(UnderlyerLimitEngineTest, LimitCheckAccurate) {
    engine.set_underlyer_limit("AAPL", 2);

    auto step1 = insert("ORD001").with_symbol("AAPL_OPT1").with_underlyer("AAPL")
        .with_side(Side::BID).with_price(5.0).with_quantity(100);
    engine.process_step(step1);

    // Adding a new instrument (OPT2) should not breach with 1 out of 2 used
    EXPECT_FALSE(engine.would_breach_limit("AAPL", "AAPL_OPT2"));

    auto step2 = insert("ORD002").with_symbol("AAPL_OPT2").with_underlyer("AAPL")
        .with_side(Side::ASK).with_price(6.0).with_quantity(50);
    engine.process_step(step2);

    // Adding a new instrument (OPT3) should breach with 2 out of 2 used
    EXPECT_TRUE(engine.would_breach_limit("AAPL", "AAPL_OPT3"));
    // But adding to existing instrument (OPT1) should not breach
    EXPECT_FALSE(engine.would_breach_limit("AAPL", "AAPL_OPT1"));
}
