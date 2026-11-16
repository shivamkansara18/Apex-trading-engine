#pragma once

#include "apex/core/order.hpp"

#include <algorithm>
#include <list>
#include <map>
#include <variant>
#include <vector>

namespace apex::detail {
struct PriceLevel {
    Quantity total{};
    std::list<Order> orders;
    PriceLevel() = default;
    PriceLevel(const PriceLevel&) = delete;
    PriceLevel& operator=(const PriceLevel&) = delete;
    PriceLevel(PriceLevel&&) noexcept = default;
    PriceLevel& operator=(PriceLevel&&) noexcept = default;
};

// Sorted contiguous price index, with FIFO list nodes kept stable as levels move.
// Search O(log P); level creation/removal O(P). No dense price-range assumption.
class FlatPriceLevels {
    using Storage = std::vector<std::pair<Price, PriceLevel>>;
    Storage values_;
public:
    auto begin() { return values_.begin(); }
    auto end() { return values_.end(); }
    auto begin() const { return values_.begin(); }
    auto end() const { return values_.end(); }
    auto rbegin() const { return values_.rbegin(); }
    auto rend() const { return values_.rend(); }
    auto rbegin() { return values_.rbegin(); }
    auto rend() { return values_.rend(); }
    bool empty() const { return values_.empty(); }
    std::size_t size() const { return values_.size(); }
    auto find(Price price) {
        auto found = std::lower_bound(begin(), end(), price, [](const auto& level, Price value) { return level.first < value; });
        return found != end() && found->first == price ? found : end();
    }
    auto find(Price price) const {
        auto found = std::lower_bound(begin(), end(), price, [](const auto& level, Price value) { return level.first < value; });
        return found != end() && found->first == price ? found : end();
    }
    auto try_emplace(Price price) {
        auto found = std::lower_bound(begin(), end(), price, [](const auto& level, Price value) { return level.first < value; });
        if (found != end() && found->first == price) { return std::pair{found, false}; }
        found = values_.insert(found, std::pair<Price, PriceLevel>{price, PriceLevel{}});
        return std::pair{found, true};
    }
    auto erase(Storage::iterator iterator) { return values_.erase(iterator); }
};
using TreePriceLevels = std::map<Price, PriceLevel>;
using PriceLevels = std::variant<TreePriceLevels, FlatPriceLevels>;
} // namespace apex::detail
