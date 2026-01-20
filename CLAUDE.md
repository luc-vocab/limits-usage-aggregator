# Pre-Trade Risk Check Aggregation Engine

## Project Overview

A high-performance aggregation engine for pre-trade risk checks that processes FIX messages and maintains real-time aggregate metrics. The engine uses a generic, template-based aggregation core that can be configured at compile time to track various metrics at different grouping levels.

## Build System

### Compiler Requirements
- **Standard**: C++17
- **Compiler**: GCC 13.2.0
- **Build Environment**: Docker (required - exact GCC version not available locally)

### Docker Build Commands
```bash
# Build the project
docker run --rm -v $(pwd):/src -w /src gcc:13.2.0 g++ -std=c++17 -O2 -Wall -Wextra -o bin/aggregator src/*.cpp

# Run regression tests
docker run --rm -v $(pwd):/src -w /src gcc:13.2.0 ./bin/test_runner
```

### Project Structure
```
/
├── CLAUDE.md
├── Dockerfile
├── Makefile
├── src/
│   ├── fix/
│   │   ├── fix_types.hpp       # FIX tag definitions
│   │   ├── fix_messages.hpp    # FIX message structures
│   │   └── fix_parser.hpp      # FIX message parsing
│   ├── aggregation/
│   │   ├── aggregation_core.hpp    # Generic aggregation engine
│   │   ├── aggregation_traits.hpp  # Compile-time customization
│   │   └── grouping.hpp            # Grouping level definitions
│   ├── metrics/
│   │   ├── delta_metrics.hpp       # Gross/net delta tracking
│   │   ├── order_count_metrics.hpp # Order counting metrics
│   │   └── notional_metrics.hpp    # Notional value tracking
│   └── main.cpp
├── tests/
│   ├── test_runner.cpp
│   ├── fix_message_tests.cpp
│   ├── aggregation_tests.cpp
│   └── integration_tests.cpp
└── bin/
```

## FIX Message Types

### Outgoing Messages (Orders)
| Message Type | FIX MsgType | Description |
|--------------|-------------|-------------|
| NewOrderSingle | D (35=D) | Insert new order request (bid or ask) |
| OrderCancelReplaceRequest | G (35=G) | Update/modify order request |
| OrderCancelRequest | F (35=F) | Cancel order request |

### Incoming Messages (Responses)
| Message Type | FIX MsgType | OrdStatus (39) | Description |
|--------------|-------------|----------------|-------------|
| InsertAck | 8 | 0 (New) | Order accepted |
| InsertNack | 8 | 8 (Rejected) | Order rejected |
| UpdateAck | 8 | 0/1 (New/PartialFill) | Modification accepted |
| UpdateNack | 9 | - | Modification rejected |
| CancelAck | 8 | 4 (Canceled) | Cancel accepted |
| CancelNack | 9 | - | Cancel rejected |
| FullFill | 8 | 2 (Filled) | Order completely filled |
| PartialFill | 8 | 1 (PartiallyFilled) | Order partially filled |
| UnsolicitedCancel | 8 | 4 (Canceled) | Exchange-initiated cancel |

### Core FIX Tags Used
```cpp
// Standard FIX tags
constexpr int TAG_MSG_TYPE = 35;
constexpr int TAG_CL_ORD_ID = 11;
constexpr int TAG_ORDER_ID = 37;
constexpr int TAG_SYMBOL = 55;
constexpr int TAG_SIDE = 54;           // 1=Buy, 2=Sell
constexpr int TAG_ORDER_QTY = 38;
constexpr int TAG_PRICE = 44;
constexpr int TAG_ORD_STATUS = 39;
constexpr int TAG_EXEC_TYPE = 150;
constexpr int TAG_LEAVES_QTY = 151;
constexpr int TAG_CUM_QTY = 14;
constexpr int TAG_UNDERLYING_SYMBOL = 311;
constexpr int TAG_SECURITY_TYPE = 167;
```

### C++ Message Structures
```cpp
enum class Side : uint8_t { Bid = 1, Ask = 2 };
enum class OrdStatus : uint8_t { New = 0, PartialFill = 1, Filled = 2, Canceled = 4, Rejected = 8 };

struct OrderKey {
    std::string cl_ord_id;
    // Hash and equality for O(1) lookups
};

struct NewOrderSingle {
    OrderKey key;
    std::string symbol;
    std::string underlyer;
    std::string strategy_id;
    std::string portfolio_id;
    Side side;
    double price;
    double quantity;
    double delta;  // For options: delta per contract
};

struct ExecutionReport {
    OrderKey key;
    std::string order_id;
    OrdStatus status;
    double leaves_qty;
    double cum_qty;
    double last_qty;
    double last_px;
};
```

## Aggregation Engine Design

### Core Principles
1. **O(1) Update Complexity**: All metric updates on message processing must be constant time
2. **Compile-Time Configuration**: Metrics and grouping levels defined via templates
3. **Type Safety**: Strong typing for aggregation keys and values

### Aggregation Core Template
```cpp
template<typename Key, typename Value, typename Combiner>
class AggregationBucket {
    // O(1) insert, update, remove operations
    // Key: grouping key (e.g., underlyer, instrument, strategy)
    // Value: aggregated value type
    // Combiner: defines how values are combined/uncombined
};

template<typename... Aggregations>
class AggregationEngine {
    // Variadic template holding multiple aggregation buckets
    // Each aggregation independently updated on message events
};
```

### Supported Metrics

| Metric | Grouping Level | Value Type | Update Trigger |
|--------|---------------|------------|----------------|
| Gross Delta | Global, Per-Underlyer | double | Order insert/fill/cancel |
| Net Delta | Global, Per-Underlyer | double | Order insert/fill/cancel |
| Bid Order Count | Per-Instrument | int64_t | Order lifecycle events |
| Ask Order Count | Per-Instrument | int64_t | Order lifecycle events |
| Quoted Instrument Count | Per-Underlyer | int64_t | First/last order per instrument |
| Open Order Notional | Per-Strategy | double | Order insert/fill/cancel |

### Grouping Keys
```cpp
struct GlobalKey {};                           // Singleton key for global aggregates
struct UnderlyerKey { std::string underlyer; };
struct InstrumentKey { std::string symbol; };
struct StrategyKey { std::string strategy_id; };
struct PortfolioKey { std::string portfolio_id; };
```

### Combiners
```cpp
struct SumCombiner {
    static double combine(double current, double delta) { return current + delta; }
    static double uncombine(double current, double delta) { return current - delta; }
};

struct CountCombiner {
    static int64_t combine(int64_t current, int64_t delta) { return current + delta; }
    static int64_t uncombine(int64_t current, int64_t delta) { return current - delta; }
};
```

## Order State Machine

```
NewOrderSingle sent
       │
       ▼
   [Pending]
       │
  ┌────┴────┐
  ▼         ▼
[Ack]    [Nack]
  │         │
  ▼         ▼
[Open]   [Rejected]
  │
  ├──► PartialFill ──► [Open, reduced qty]
  │
  ├──► FullFill ──► [Filled]
  │
  ├──► CancelAck ──► [Canceled]
  │
  └──► UnsolicitedCancel ──► [Canceled]
```

## Testing Requirements

### Regression Test Coverage
All tests run via Docker to ensure consistent GCC 13.2.0 environment.

1. **FIX Message Tests**
   - Parse/serialize all outgoing message types
   - Parse all incoming message types
   - Validate all required FIX tags present

2. **Aggregation Core Tests**
   - O(1) complexity verification (timing tests)
   - Correct combine/uncombine operations
   - Edge cases: first insert, last remove, duplicate keys

3. **Metric-Specific Tests**
   - Delta calculations (gross and net, global and per-underlyer)
   - Order count accuracy per instrument and side
   - Quoted instrument counting per underlyer
   - Strategy notional tracking

4. **Integration Tests**
   - Full message flow scenarios
   - State consistency after mixed message sequences
   - Recovery from nack scenarios

### Running Tests
```bash
# Run all tests
make docker-test

# Run specific test suite
docker run --rm -v $(pwd):/src -w /src gcc:13.2.0 ./bin/test_runner --filter="aggregation"
```

## Code Style

- Use `snake_case` for variables and functions
- Use `PascalCase` for types and classes
- Use `UPPER_CASE` for constants and compile-time values
- Prefer `constexpr` over `const` where possible
- Use `std::optional` for nullable values
- Use `std::string_view` for non-owning string references
- Header-only templates in `.hpp` files
- Implementation in `.cpp` files where possible

## Performance Considerations

- Use `std::unordered_map` with custom hash for O(1) key lookups
- Avoid allocations in hot paths (use object pools or pre-allocation)
- Consider cache-friendly data layout for frequently accessed metrics
- Profile with representative message volumes before optimizing
