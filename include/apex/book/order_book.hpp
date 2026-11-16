#pragma once

#include "apex/book/snapshot.hpp"
#include "apex/book/price_levels.hpp"
#include "apex/core/engine_options.hpp"

#include <cstddef>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace apex {
enum class BookError { None, InvalidOrder, DuplicateOrder, QuantityOverflow,
                       UnknownOrder, WrongParticipant };

// Storage only: caller supplies validated resting limit orders. Matching owns
// crossing decisions. Copies/moves are disabled because the index owns iterators.
class OrderBook {
public:
    explicit OrderBook(PriceIndex index = PriceIndex::Tree);
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    [[nodiscard]] BookError insert(const Order& order);
    [[nodiscard]] BookError cancel(OrderId id, ParticipantId participant);
    [[nodiscard]] std::optional<Order> find(OrderId id) const;
    [[nodiscard]] std::optional<Order> best_order(Side side) const;
    // Returns post-fill state (including Filled); invalid fills leave storage unchanged.
    [[nodiscard]] std::optional<Order> fill(OrderId id, Quantity quantity);
    [[nodiscard]] std::optional<Price> best_bid() const;
    [[nodiscard]] std::optional<Price> best_ask() const;
    [[nodiscard]] Quantity depth(Side side, Price price) const;
    [[nodiscard]] Quantity total_quantity(Side side) const;
    [[nodiscard]] std::size_t order_count() const noexcept;
    [[nodiscard]] std::vector<PriceLevelSnapshot> top_levels(Side side, std::size_t count) const;
    [[nodiscard]] BookSnapshot snapshot(CommandSequence last_command = 0) const;
    // Set require_uncrossed for a settled engine book, not intermediate storage.
    [[nodiscard]] bool check_invariants(bool require_uncrossed = false) const;

    // Read-only traversal on the owning thread. Returning false stops the scan.
    // References must not escape the callback; snapshots remain owning copies.
    template<class Visitor> void visit_orders(Side side, Visitor&& visitor) const {
        const auto visit = [&](auto first, auto last) {
            for (; first != last; ++first) {
                for (const auto& order : first->second.orders) { if (!visitor(order)) { return; } }
            }
        };
        if (side == Side::Buy) {
            std::visit([&](const auto& levels) { visit(levels.rbegin(), levels.rend()); }, bids_);
        } else if (side == Side::Sell) {
            std::visit([&](const auto& levels) { visit(levels.begin(), levels.end()); }, asks_);
        }
    }

private:
    struct Location {
        Side side;
        Price price;
        std::list<Order>::iterator order;
    };
    using Levels = detail::PriceLevels;
    Levels bids_;
    Levels asks_;
    std::unordered_map<OrderId, Location> index_;
    Quantity bid_quantity_{};
    Quantity ask_quantity_{};
};
} // namespace apex
