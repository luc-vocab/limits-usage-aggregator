#pragma once

#include <unordered_map>

namespace aggregation {

// Type alias for hash map to allow easy swapping of implementation
template<typename Key, typename Value>
using HashMap = std::unordered_map<Key, Value>;

} // namespace aggregation
