#pragma once

#include <cstdint>

namespace ob {

using OrderId = std::uint64_t;
using Price   = std::int64_t;    // integer ticks, never floating point
using Qty     = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };

// GTC: whatever does not fill rests on the book.
// IOC: whatever does not fill immediately is discarded.
enum class Tif : std::uint8_t { GTC, IOC };

// One trade. The maker is the resting order, the taker is the incoming one,
// and the trade always prints at the maker's price.
struct Fill {
    OrderId maker;
    OrderId taker;
    Price   price;
    Qty     qty;

    bool operator==(const Fill&) const = default;
};

enum class Status : std::uint8_t {
    Rested,     // some or all of the order is now on the book
    Filled,     // fully filled on arrival
    Cancelled,  // IOC: the unfilled remainder was discarded
    Rejected,   // bad id, duplicate id, zero quantity, or price out of range
};

struct AddResult {
    Status status;
    Qty    filled;

    bool operator==(const AddResult&) const = default;
};

}  // namespace ob
