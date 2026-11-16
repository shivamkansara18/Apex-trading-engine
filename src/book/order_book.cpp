#include "apex/book/order_book.hpp"

#include <iterator>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace apex {
namespace {
bool valid_side(Side side) { return side == Side::Buy || side == Side::Sell; }
}

OrderBook::OrderBook(PriceIndex index) {
    if (index == PriceIndex::Flat) {
        bids_.emplace<detail::FlatPriceLevels>();
        asks_.emplace<detail::FlatPriceLevels>();
    } else if (index != PriceIndex::Tree) { throw std::invalid_argument("Unknown price index"); }
}

BookError OrderBook::insert(const Order& order) {
    if (!valid_side(order.side) || order.type != OrderType::Limit ||
        order.price_ticks <= 0 || order.remaining_quantity == 0 ||
        order.remaining_quantity > order.original_quantity || order.priority_sequence == 0 ||
        (order.status != OrderStatus::Resting && order.status != OrderStatus::PartiallyFilled)) {
        return BookError::InvalidOrder;
    }
    if (index_.contains(order.client_order_id)) { return BookError::DuplicateOrder; }
    return std::visit([&](auto& levels) -> BookError {
        auto& total = order.side == Side::Buy ? bid_quantity_ : ask_quantity_;
        if (order.remaining_quantity > std::numeric_limits<Quantity>::max() - total) {
            return BookError::QuantityOverflow;
        }
        const auto existing = levels.find(order.price_ticks);
        if (existing != levels.end() &&
            existing->second.orders.back().priority_sequence >= order.priority_sequence) {
            return BookError::InvalidOrder;
        }

        auto [level_it, created] = levels.try_emplace(order.price_ticks);
        auto& level = level_it->second;
        // Roll back allocations if insertion fails; never leave an orphan list/index.
        try {
            level.orders.push_back(order);
            try {
                index_.emplace(order.client_order_id,
                               Location{order.side, order.price_ticks, std::prev(level.orders.end())});
            } catch (...) {
                level.orders.pop_back();
                throw;
            }
        } catch (...) {
            if (created) { levels.erase(level_it); }
            throw;
        }
        level.total += order.remaining_quantity;
        total += order.remaining_quantity;
        return BookError::None;
    }, order.side == Side::Buy ? bids_ : asks_);
}

BookError OrderBook::cancel(OrderId id, ParticipantId participant) {
    const auto found = index_.find(id);
    if (found == index_.end()) { return BookError::UnknownOrder; }
    const auto& location = found->second;
    if (location.order->participant_id != participant) { return BookError::WrongParticipant; }
    return std::visit([&](auto& levels) -> BookError {
        auto& total = location.side == Side::Buy ? bid_quantity_ : ask_quantity_;
        const auto level_it = levels.find(location.price);
        auto& level = level_it->second;
        const auto quantity = location.order->remaining_quantity;
        level.total -= quantity;
        total -= quantity;
        level.orders.erase(location.order);
        index_.erase(found);
        if (level.orders.empty()) { levels.erase(level_it); }
        return BookError::None;
    }, location.side == Side::Buy ? bids_ : asks_);
}

std::optional<Order> OrderBook::find(OrderId id) const {
    const auto found = index_.find(id);
    if (found == index_.end()) { return std::nullopt; }
    return *found->second.order;
}

std::optional<Order> OrderBook::best_order(Side side) const {
    if (!valid_side(side)) { return std::nullopt; }
    return std::visit([&](const auto& levels) -> std::optional<Order> {
        if (levels.empty()) { return std::nullopt; }
        return side == Side::Buy ? levels.rbegin()->second.orders.front() : levels.begin()->second.orders.front();
    }, side == Side::Buy ? bids_ : asks_);
}

std::optional<Order> OrderBook::fill(OrderId id, Quantity quantity) {
    const auto found = index_.find(id);
    if (found == index_.end() || quantity == 0 ||
        quantity > found->second.order->remaining_quantity) { return std::nullopt; }
    auto& location = found->second;
    auto& order = *location.order;
    return std::visit([&](auto& levels) -> std::optional<Order> {
        auto& total = location.side == Side::Buy ? bid_quantity_ : ask_quantity_;
        const auto level = levels.find(location.price);
        order.remaining_quantity -= quantity;
        order.status = order.remaining_quantity == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
        level->second.total -= quantity;
        total -= quantity;
        const Order result = order;
        if (order.remaining_quantity == 0) {
            level->second.orders.erase(location.order);
            index_.erase(found);
            if (level->second.orders.empty()) { levels.erase(level); }
        }
        return result;
    }, location.side == Side::Buy ? bids_ : asks_);
}

std::optional<Price> OrderBook::best_bid() const {
    return std::visit([](const auto& levels) -> std::optional<Price> {
        if (levels.empty()) { return std::nullopt; }
        return levels.rbegin()->first;
    }, bids_);
}
std::optional<Price> OrderBook::best_ask() const {
    return std::visit([](const auto& levels) -> std::optional<Price> {
        if (levels.empty()) { return std::nullopt; }
        return levels.begin()->first;
    }, asks_);
}
Quantity OrderBook::depth(Side side, Price price) const {
    if (!valid_side(side)) { return 0; }
    return std::visit([&](const auto& levels) -> Quantity {
        const auto found = levels.find(price);
        return found == levels.end() ? 0 : found->second.total;
    }, side == Side::Buy ? bids_ : asks_);
}
Quantity OrderBook::total_quantity(Side side) const {
    if (!valid_side(side)) { return 0; }
    return side == Side::Buy ? bid_quantity_ : ask_quantity_;
}
std::size_t OrderBook::order_count() const noexcept { return index_.size(); }

std::vector<PriceLevelSnapshot> OrderBook::top_levels(Side side, std::size_t count) const {
    std::vector<PriceLevelSnapshot> result;
    const auto append = [&](auto first, auto last) {
        while (first != last && result.size() < count) {
            const auto& [price, level] = *first++;
            result.push_back({price, level.total, {level.orders.begin(), level.orders.end()}});
        }
    };
    if (side == Side::Buy) { std::visit([&](const auto& levels) { append(levels.rbegin(), levels.rend()); }, bids_); }
    else if (side == Side::Sell) { std::visit([&](const auto& levels) { append(levels.begin(), levels.end()); }, asks_); }
    return result;
}
BookSnapshot OrderBook::snapshot(CommandSequence last_command) const {
    return {last_command, top_levels(Side::Buy, index_.size()), top_levels(Side::Sell, index_.size())};
}

bool OrderBook::check_invariants(bool require_uncrossed) const {
    std::unordered_set<OrderId> seen;
    const auto check = [&](const auto& levels, Side side, Quantity expected_total) {
        Quantity side_total = 0;
        std::optional<Price> previous_price;
        for (const auto& [price, level] : levels) {
            if (previous_price && price <= *previous_price) { return false; }
            previous_price = price;
            if (price <= 0 || level.orders.empty()) { return false; }
            Quantity level_total = 0;
            CommandSequence previous = 0;
            for (auto it = level.orders.begin(); it != level.orders.end(); ++it) {
                const auto& order = *it;
                if (order.side != side || order.type != OrderType::Limit ||
                    order.price_ticks != price || order.remaining_quantity == 0 ||
                    order.remaining_quantity > order.original_quantity ||
                    order.priority_sequence <= previous ||
                    (order.status != OrderStatus::Resting && order.status != OrderStatus::PartiallyFilled) ||
                    !seen.insert(order.client_order_id).second) { return false; }
                const auto entry = index_.find(order.client_order_id);
                if (entry == index_.end() || entry->second.side != side ||
                    entry->second.price != price || entry->second.order != it) { return false; }
                if (order.remaining_quantity > std::numeric_limits<Quantity>::max() - level_total) {
                    return false;
                }
                level_total += order.remaining_quantity;
                previous = order.priority_sequence;
            }
            if (level_total != level.total ||
                level_total > std::numeric_limits<Quantity>::max() - side_total) { return false; }
            side_total += level_total;
        }
        return side_total == expected_total;
    };
    if (!std::visit([&](const auto& levels) { return check(levels, Side::Buy, bid_quantity_); }, bids_) ||
        !std::visit([&](const auto& levels) { return check(levels, Side::Sell, ask_quantity_); }, asks_) ||
        seen.size() != index_.size()) { return false; }
    return !require_uncrossed || !best_bid() || !best_ask() || *best_bid() < *best_ask();
}
} // namespace apex
