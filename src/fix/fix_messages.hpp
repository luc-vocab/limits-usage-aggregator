#pragma once

#include "fix_types.hpp"
#include <string>
#include <optional>
#include <cstdint>

namespace fix {

// ============================================================================
// Outgoing Messages
// ============================================================================

// New Order Single (MsgType=D)
struct NewOrderSingle {
    OrderKey key;
    std::string symbol;
    std::string underlyer;
    std::string strategy_id;
    std::string portfolio_id;
    Side side;
    double price;
    int64_t quantity;
    // Note: delta is now obtained from InstrumentProvider, not from the order
};

// Order Cancel/Replace Request (MsgType=G)
struct OrderCancelReplaceRequest {
    OrderKey key;              // New ClOrdID
    OrderKey orig_key;         // Original ClOrdID being modified
    std::string symbol;
    Side side;
    double price;
    int64_t quantity;
};

// Order Cancel Request (MsgType=F)
struct OrderCancelRequest {
    OrderKey key;              // New ClOrdID for this cancel request
    OrderKey orig_key;         // Original ClOrdID being canceled
    std::string symbol;
    Side side;
};

// ============================================================================
// Incoming Messages
// ============================================================================

// High-level categorization of execution report types
enum class ExecutionReportType {
    INSERT_ACK,
    INSERT_NACK,
    UPDATE_ACK,
    UPDATE_NACK,
    CANCEL_ACK,
    CANCEL_NACK,
    PARTIAL_FILL,
    FULL_FILL,
    UNSOLICITED_CANCEL
};

inline const char* to_string(ExecutionReportType type) {
    switch (type) {
        case ExecutionReportType::INSERT_ACK: return "INSERT_ACK";
        case ExecutionReportType::INSERT_NACK: return "INSERT_NACK";
        case ExecutionReportType::UPDATE_ACK: return "UPDATE_ACK";
        case ExecutionReportType::UPDATE_NACK: return "UPDATE_NACK";
        case ExecutionReportType::CANCEL_ACK: return "CANCEL_ACK";
        case ExecutionReportType::CANCEL_NACK: return "CANCEL_NACK";
        case ExecutionReportType::PARTIAL_FILL: return "PARTIAL_FILL";
        case ExecutionReportType::FULL_FILL: return "FULL_FILL";
        case ExecutionReportType::UNSOLICITED_CANCEL: return "UNSOLICITED_CANCEL";
        default: return "UNKNOWN";
    }
}

// Execution Report (MsgType=8)
struct ExecutionReport {
    OrderKey key;                      // ClOrdID
    std::optional<OrderKey> orig_key;  // OrigClOrdID (for cancel/replace responses)
    std::string order_id;              // Exchange order ID
    std::string symbol;
    OrdStatus ord_status;
    ExecType exec_type;
    int64_t leaves_qty;                // Remaining quantity
    int64_t cum_qty;                   // Cumulative filled quantity
    int64_t last_qty;                  // Last fill quantity (0 if not a fill)
    double last_px;                    // Last fill price (0 if not a fill)
    std::optional<std::string> text;   // Rejection reason text
    bool is_unsolicited;               // True if exchange-initiated cancel

    // Determine the high-level report type
    ExecutionReportType report_type() const {
        if (exec_type == ExecType::REJECTED) {
            if (orig_key.has_value()) {
                return ExecutionReportType::UPDATE_NACK;
            }
            return ExecutionReportType::INSERT_NACK;
        }

        if (exec_type == ExecType::CANCELED) {
            if (is_unsolicited) {
                return ExecutionReportType::UNSOLICITED_CANCEL;
            }
            return ExecutionReportType::CANCEL_ACK;
        }

        if (exec_type == ExecType::REPLACED) {
            return ExecutionReportType::UPDATE_ACK;
        }

        if (exec_type == ExecType::FILL) {
            return ExecutionReportType::FULL_FILL;
        }

        if (exec_type == ExecType::PARTIAL_FILL) {
            return ExecutionReportType::PARTIAL_FILL;
        }

        // ExecType::NEW
        if (orig_key.has_value()) {
            return ExecutionReportType::UPDATE_ACK;
        }
        return ExecutionReportType::INSERT_ACK;
    }
};

// Order Cancel Reject (MsgType=9)
struct OrderCancelReject {
    OrderKey key;                      // ClOrdID of the cancel/replace request
    OrderKey orig_key;                 // Original ClOrdID that was attempted to be canceled/replaced
    std::string order_id;
    OrdStatus ord_status;              // Current status of the order
    CxlRejResponseTo response_to;      // What type of request was rejected
    int cxl_rej_reason;                // Rejection reason code
    std::optional<std::string> text;   // Rejection reason text

    // Determine if this is a cancel nack or update nack
    ExecutionReportType report_type() const {
        if (response_to == CxlRejResponseTo::ORDER_CANCEL_REQUEST) {
            return ExecutionReportType::CANCEL_NACK;
        }
        return ExecutionReportType::UPDATE_NACK;
    }
};

} // namespace fix
