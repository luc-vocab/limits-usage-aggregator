#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <functional>

namespace fix {

// Standard FIX tags
namespace tags {
    constexpr int MSG_TYPE = 35;
    constexpr int CL_ORD_ID = 11;
    constexpr int ORIG_CL_ORD_ID = 41;
    constexpr int ORDER_ID = 37;
    constexpr int SYMBOL = 55;
    constexpr int SIDE = 54;
    constexpr int ORDER_QTY = 38;
    constexpr int PRICE = 44;
    constexpr int ORD_STATUS = 39;
    constexpr int EXEC_TYPE = 150;
    constexpr int LEAVES_QTY = 151;
    constexpr int CUM_QTY = 14;
    constexpr int LAST_QTY = 32;
    constexpr int LAST_PX = 31;
    constexpr int UNDERLYING_SYMBOL = 311;
    constexpr int SECURITY_TYPE = 167;
    constexpr int TEXT = 58;
    constexpr int ORD_REJ_REASON = 103;
    constexpr int CXL_REJ_REASON = 102;
    constexpr int CXL_REJ_RESPONSE_TO = 434;
}

// FIX message types (tag 35)
namespace msg_type {
    constexpr char NEW_ORDER_SINGLE = 'D';
    constexpr char ORDER_CANCEL_REPLACE = 'G';
    constexpr char ORDER_CANCEL_REQUEST = 'F';
    constexpr char EXECUTION_REPORT = '8';
    constexpr char ORDER_CANCEL_REJECT = '9';
}

// Side (tag 54)
enum class Side : uint8_t {
    BID = 1,  // Buy
    ASK = 2   // Sell
};

inline const char* to_string(Side side) {
    switch (side) {
        case Side::BID: return "BID";
        case Side::ASK: return "ASK";
        default: return "UNKNOWN";
    }
}

// Order status (tag 39)
enum class OrdStatus : uint8_t {
    NEW = 0,
    PARTIALLY_FILLED = 1,
    FILLED = 2,
    CANCELED = 4,
    REJECTED = 8
};

inline const char* to_string(OrdStatus status) {
    switch (status) {
        case OrdStatus::NEW: return "NEW";
        case OrdStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrdStatus::FILLED: return "FILLED";
        case OrdStatus::CANCELED: return "CANCELED";
        case OrdStatus::REJECTED: return "REJECTED";
        default: return "UNKNOWN";
    }
}

// Execution type (tag 150)
enum class ExecType : uint8_t {
    NEW = 0,
    PARTIAL_FILL = 1,
    FILL = 2,
    CANCELED = 4,
    REPLACED = 5,
    REJECTED = 8
};

inline const char* to_string(ExecType exec_type) {
    switch (exec_type) {
        case ExecType::NEW: return "NEW";
        case ExecType::PARTIAL_FILL: return "PARTIAL_FILL";
        case ExecType::FILL: return "FILL";
        case ExecType::CANCELED: return "CANCELED";
        case ExecType::REPLACED: return "REPLACED";
        case ExecType::REJECTED: return "REJECTED";
        default: return "UNKNOWN";
    }
}

// Cancel reject response to (tag 434)
enum class CxlRejResponseTo : uint8_t {
    ORDER_CANCEL_REQUEST = 1,
    ORDER_CANCEL_REPLACE_REQUEST = 2
};

// Order key for tracking
struct OrderKey {
    std::string cl_ord_id;

    bool operator==(const OrderKey& other) const {
        return cl_ord_id == other.cl_ord_id;
    }

    bool operator!=(const OrderKey& other) const {
        return !(*this == other);
    }
};

} // namespace fix

// Hash specialization for OrderKey
namespace std {
    template<>
    struct hash<fix::OrderKey> {
        size_t operator()(const fix::OrderKey& key) const {
            return hash<string>{}(key.cl_ord_id);
        }
    };
}
