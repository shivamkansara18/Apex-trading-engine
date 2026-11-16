#pragma once

#include "apex/core/order.hpp"

#include <vector>

namespace apex {
struct PriceLevelSnapshot {
    Price price_ticks{};
    Quantity total_quantity{};
    std::vector<Order> orders; // FIFO; owning copies, no mutable book references.
    bool operator==(const PriceLevelSnapshot&) const = default;
};

struct BookSnapshot {
    CommandSequence last_command_sequence{};
    std::vector<PriceLevelSnapshot> bids; // Descending prices.
    std::vector<PriceLevelSnapshot> asks; // Ascending prices.
    bool operator==(const BookSnapshot&) const = default;
};
} // namespace apex
