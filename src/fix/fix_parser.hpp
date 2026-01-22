#pragma once

#include "fix_messages.hpp"
#include <sstream>
#include <unordered_map>
#include <stdexcept>
#include <charconv>

namespace fix {

// FIX field separator (SOH character, ASCII 1)
constexpr char FIX_DELIMITER = '\x01';

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& msg) : std::runtime_error(msg) {}
};

// Parse a FIX message string into tag-value pairs
inline std::unordered_map<int, std::string> parse_fix_fields(std::string_view message) {
    std::unordered_map<int, std::string> fields;

    size_t pos = 0;
    while (pos < message.size()) {
        // Find the '=' separator
        size_t eq_pos = message.find('=', pos);
        if (eq_pos == std::string_view::npos) break;

        // Find the field delimiter
        size_t delim_pos = message.find(FIX_DELIMITER, eq_pos);
        if (delim_pos == std::string_view::npos) {
            delim_pos = message.size();
        }

        // Parse tag
        int tag = 0;
        auto tag_str = message.substr(pos, eq_pos - pos);
        auto [ptr, ec] = std::from_chars(tag_str.data(), tag_str.data() + tag_str.size(), tag);
        if (ec != std::errc{}) {
            throw ParseError("Invalid tag: " + std::string(tag_str));
        }

        // Extract value
        std::string value(message.substr(eq_pos + 1, delim_pos - eq_pos - 1));
        fields[tag] = std::move(value);

        pos = delim_pos + 1;
    }

    return fields;
}

// Helper to get required field
inline const std::string& get_required(const std::unordered_map<int, std::string>& fields,
                                        int tag, const char* name) {
    auto it = fields.find(tag);
    if (it == fields.end()) {
        throw ParseError(std::string("Missing required field: ") + name + " (tag " + std::to_string(tag) + ")");
    }
    return it->second;
}

// Helper to get optional field
inline std::optional<std::string> get_optional(const std::unordered_map<int, std::string>& fields, int tag) {
    auto it = fields.find(tag);
    if (it == fields.end()) {
        return std::nullopt;
    }
    return it->second;
}

// Parse Side from string
inline Side parse_side(const std::string& value) {
    if (value == "1") return Side::BID;
    if (value == "2") return Side::ASK;
    throw ParseError("Invalid side: " + value);
}

// Parse OrdStatus from string
inline OrdStatus parse_ord_status(const std::string& value) {
    if (value == "0") return OrdStatus::NEW;
    if (value == "1") return OrdStatus::PARTIALLY_FILLED;
    if (value == "2") return OrdStatus::FILLED;
    if (value == "4") return OrdStatus::CANCELED;
    if (value == "8") return OrdStatus::REJECTED;
    throw ParseError("Invalid OrdStatus: " + value);
}

// Parse ExecType from string
inline ExecType parse_exec_type(const std::string& value) {
    if (value == "0") return ExecType::NEW;
    if (value == "1") return ExecType::PARTIAL_FILL;
    if (value == "2") return ExecType::FILL;
    if (value == "4") return ExecType::CANCELED;
    if (value == "5") return ExecType::REPLACED;
    if (value == "8") return ExecType::REJECTED;
    throw ParseError("Invalid ExecType: " + value);
}

// Parse double from string
inline double parse_double(const std::string& value) {
    try {
        return std::stod(value);
    } catch (const std::exception&) {
        throw ParseError("Invalid double: " + value);
    }
}

// Parse int from string
inline int parse_int(const std::string& value) {
    try {
        return std::stoi(value);
    } catch (const std::exception&) {
        throw ParseError("Invalid int: " + value);
    }
}

// Parse int64_t from string
inline int64_t parse_int64(const std::string& value) {
    try {
        return std::stoll(value);
    } catch (const std::exception&) {
        throw ParseError("Invalid int64: " + value);
    }
}

// ============================================================================
// Message Parsers
// ============================================================================

inline NewOrderSingle parse_new_order_single(const std::unordered_map<int, std::string>& fields) {
    NewOrderSingle msg;
    msg.key.cl_ord_id = get_required(fields, tags::CL_ORD_ID, "ClOrdID");
    msg.symbol = get_required(fields, tags::SYMBOL, "Symbol");
    msg.side = parse_side(get_required(fields, tags::SIDE, "Side"));
    msg.quantity = parse_int64(get_required(fields, tags::ORDER_QTY, "OrderQty"));
    msg.price = parse_double(get_required(fields, tags::PRICE, "Price"));

    // Optional fields
    auto underlyer = get_optional(fields, tags::UNDERLYING_SYMBOL);
    msg.underlyer = underlyer.value_or(msg.symbol);  // Default to symbol if no underlyer

    // Custom fields for strategy and portfolio (using placeholder tags for now)
    // In production, these would be defined in a custom FIX dictionary
    msg.strategy_id = get_optional(fields, 7001).value_or("");
    msg.portfolio_id = get_optional(fields, 7002).value_or("");
    // Note: delta is now obtained from InstrumentProvider, not parsed from order

    return msg;
}

inline OrderCancelReplaceRequest parse_order_cancel_replace(const std::unordered_map<int, std::string>& fields) {
    OrderCancelReplaceRequest msg;
    msg.key.cl_ord_id = get_required(fields, tags::CL_ORD_ID, "ClOrdID");
    msg.orig_key.cl_ord_id = get_required(fields, tags::ORIG_CL_ORD_ID, "OrigClOrdID");
    msg.symbol = get_required(fields, tags::SYMBOL, "Symbol");
    msg.side = parse_side(get_required(fields, tags::SIDE, "Side"));
    msg.quantity = parse_int64(get_required(fields, tags::ORDER_QTY, "OrderQty"));
    msg.price = parse_double(get_required(fields, tags::PRICE, "Price"));
    return msg;
}

inline OrderCancelRequest parse_order_cancel_request(const std::unordered_map<int, std::string>& fields) {
    OrderCancelRequest msg;
    msg.key.cl_ord_id = get_required(fields, tags::CL_ORD_ID, "ClOrdID");
    msg.orig_key.cl_ord_id = get_required(fields, tags::ORIG_CL_ORD_ID, "OrigClOrdID");
    msg.symbol = get_required(fields, tags::SYMBOL, "Symbol");
    msg.side = parse_side(get_required(fields, tags::SIDE, "Side"));
    return msg;
}

inline ExecutionReport parse_execution_report(const std::unordered_map<int, std::string>& fields,
                                               bool is_unsolicited = false) {
    ExecutionReport msg;
    msg.key.cl_ord_id = get_required(fields, tags::CL_ORD_ID, "ClOrdID");
    msg.order_id = get_required(fields, tags::ORDER_ID, "OrderID");
    msg.ord_status = parse_ord_status(get_required(fields, tags::ORD_STATUS, "OrdStatus"));
    msg.exec_type = parse_exec_type(get_required(fields, tags::EXEC_TYPE, "ExecType"));

    // Optional OrigClOrdID
    auto orig = get_optional(fields, tags::ORIG_CL_ORD_ID);
    if (orig.has_value()) {
        msg.orig_key = OrderKey{orig.value()};
    }

    // Optional symbol
    msg.symbol = get_optional(fields, tags::SYMBOL).value_or("");

    // Quantities
    msg.leaves_qty = parse_int64(get_optional(fields, tags::LEAVES_QTY).value_or("0"));
    msg.cum_qty = parse_int64(get_optional(fields, tags::CUM_QTY).value_or("0"));
    msg.last_qty = parse_int64(get_optional(fields, tags::LAST_QTY).value_or("0"));
    msg.last_px = parse_double(get_optional(fields, tags::LAST_PX).value_or("0"));

    msg.text = get_optional(fields, tags::TEXT);
    msg.is_unsolicited = is_unsolicited;

    return msg;
}

inline OrderCancelReject parse_order_cancel_reject(const std::unordered_map<int, std::string>& fields) {
    OrderCancelReject msg;
    msg.key.cl_ord_id = get_required(fields, tags::CL_ORD_ID, "ClOrdID");
    msg.orig_key.cl_ord_id = get_required(fields, tags::ORIG_CL_ORD_ID, "OrigClOrdID");
    msg.order_id = get_required(fields, tags::ORDER_ID, "OrderID");
    msg.ord_status = parse_ord_status(get_required(fields, tags::ORD_STATUS, "OrdStatus"));

    auto response_to = get_optional(fields, tags::CXL_REJ_RESPONSE_TO);
    msg.response_to = (response_to.value_or("1") == "2")
        ? CxlRejResponseTo::ORDER_CANCEL_REPLACE_REQUEST
        : CxlRejResponseTo::ORDER_CANCEL_REQUEST;

    msg.cxl_rej_reason = parse_int(get_optional(fields, tags::CXL_REJ_REASON).value_or("0"));
    msg.text = get_optional(fields, tags::TEXT);

    return msg;
}

// ============================================================================
// Message Serializers (for testing/logging)
// ============================================================================

inline std::string serialize_new_order_single(const NewOrderSingle& msg) {
    std::ostringstream oss;
    oss << tags::MSG_TYPE << "=" << msg_type::NEW_ORDER_SINGLE << FIX_DELIMITER
        << tags::CL_ORD_ID << "=" << msg.key.cl_ord_id << FIX_DELIMITER
        << tags::SYMBOL << "=" << msg.symbol << FIX_DELIMITER
        << tags::SIDE << "=" << static_cast<int>(msg.side) << FIX_DELIMITER
        << tags::ORDER_QTY << "=" << msg.quantity << FIX_DELIMITER
        << tags::PRICE << "=" << msg.price << FIX_DELIMITER;

    if (!msg.underlyer.empty() && msg.underlyer != msg.symbol) {
        oss << tags::UNDERLYING_SYMBOL << "=" << msg.underlyer << FIX_DELIMITER;
    }
    if (!msg.strategy_id.empty()) {
        oss << 7001 << "=" << msg.strategy_id << FIX_DELIMITER;
    }
    if (!msg.portfolio_id.empty()) {
        oss << 7002 << "=" << msg.portfolio_id << FIX_DELIMITER;
    }
    // Note: delta is now obtained from InstrumentProvider, not serialized

    return oss.str();
}

inline std::string serialize_order_cancel_replace(const OrderCancelReplaceRequest& msg) {
    std::ostringstream oss;
    oss << tags::MSG_TYPE << "=" << msg_type::ORDER_CANCEL_REPLACE << FIX_DELIMITER
        << tags::CL_ORD_ID << "=" << msg.key.cl_ord_id << FIX_DELIMITER
        << tags::ORIG_CL_ORD_ID << "=" << msg.orig_key.cl_ord_id << FIX_DELIMITER
        << tags::SYMBOL << "=" << msg.symbol << FIX_DELIMITER
        << tags::SIDE << "=" << static_cast<int>(msg.side) << FIX_DELIMITER
        << tags::ORDER_QTY << "=" << msg.quantity << FIX_DELIMITER
        << tags::PRICE << "=" << msg.price << FIX_DELIMITER;
    return oss.str();
}

inline std::string serialize_order_cancel_request(const OrderCancelRequest& msg) {
    std::ostringstream oss;
    oss << tags::MSG_TYPE << "=" << msg_type::ORDER_CANCEL_REQUEST << FIX_DELIMITER
        << tags::CL_ORD_ID << "=" << msg.key.cl_ord_id << FIX_DELIMITER
        << tags::ORIG_CL_ORD_ID << "=" << msg.orig_key.cl_ord_id << FIX_DELIMITER
        << tags::SYMBOL << "=" << msg.symbol << FIX_DELIMITER
        << tags::SIDE << "=" << static_cast<int>(msg.side) << FIX_DELIMITER;
    return oss.str();
}

inline std::string serialize_execution_report(const ExecutionReport& msg) {
    std::ostringstream oss;
    oss << tags::MSG_TYPE << "=" << msg_type::EXECUTION_REPORT << FIX_DELIMITER
        << tags::CL_ORD_ID << "=" << msg.key.cl_ord_id << FIX_DELIMITER
        << tags::ORDER_ID << "=" << msg.order_id << FIX_DELIMITER
        << tags::ORD_STATUS << "=" << static_cast<int>(msg.ord_status) << FIX_DELIMITER
        << tags::EXEC_TYPE << "=" << static_cast<int>(msg.exec_type) << FIX_DELIMITER
        << tags::LEAVES_QTY << "=" << msg.leaves_qty << FIX_DELIMITER
        << tags::CUM_QTY << "=" << msg.cum_qty << FIX_DELIMITER;

    if (msg.orig_key.has_value()) {
        oss << tags::ORIG_CL_ORD_ID << "=" << msg.orig_key->cl_ord_id << FIX_DELIMITER;
    }
    if (!msg.symbol.empty()) {
        oss << tags::SYMBOL << "=" << msg.symbol << FIX_DELIMITER;
    }
    if (msg.last_qty > 0) {
        oss << tags::LAST_QTY << "=" << msg.last_qty << FIX_DELIMITER
            << tags::LAST_PX << "=" << msg.last_px << FIX_DELIMITER;
    }
    if (msg.text.has_value()) {
        oss << tags::TEXT << "=" << msg.text.value() << FIX_DELIMITER;
    }

    return oss.str();
}

inline std::string serialize_order_cancel_reject(const OrderCancelReject& msg) {
    std::ostringstream oss;
    oss << tags::MSG_TYPE << "=" << msg_type::ORDER_CANCEL_REJECT << FIX_DELIMITER
        << tags::CL_ORD_ID << "=" << msg.key.cl_ord_id << FIX_DELIMITER
        << tags::ORIG_CL_ORD_ID << "=" << msg.orig_key.cl_ord_id << FIX_DELIMITER
        << tags::ORDER_ID << "=" << msg.order_id << FIX_DELIMITER
        << tags::ORD_STATUS << "=" << static_cast<int>(msg.ord_status) << FIX_DELIMITER
        << tags::CXL_REJ_RESPONSE_TO << "=" << static_cast<int>(msg.response_to) << FIX_DELIMITER
        << tags::CXL_REJ_REASON << "=" << msg.cxl_rej_reason << FIX_DELIMITER;

    if (msg.text.has_value()) {
        oss << tags::TEXT << "=" << msg.text.value() << FIX_DELIMITER;
    }

    return oss.str();
}

} // namespace fix
