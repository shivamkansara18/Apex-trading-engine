#pragma once

#include "apex/book/order_book.hpp"
#include "apex/core/command.hpp"

#include <limits>
#include <optional>
#include <unordered_map>

namespace apex {
struct RiskLimits {
    Quantity max_order_quantity{std::numeric_limits<Quantity>::max()};
    std::uint64_t max_order_notional{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t max_open_orders{std::numeric_limits<std::uint64_t>::max()};
    Price min_price_ticks{1};
    Price max_price_ticks{std::numeric_limits<Price>::max()};
    bool trading_enabled{true};
    bool operator==(const RiskLimits&) const = default;
};

struct RiskUsage {
    std::uint64_t open_orders{};
    Quantity buy_quantity{};
    Quantity sell_quantity{};
    bool operator==(const RiskUsage&) const = default;
};

// Single-writer configuration. Usage is derived from the authoritative book,
// avoiding a second lifecycle ledger. No accounts or real-money connectivity.
class RiskManager {
public:
    explicit RiskManager(RiskTraversal traversal = RiskTraversal::Direct) : traversal_(traversal) {}
    [[nodiscard]] bool register_participant(ParticipantId id, RiskLimits limits = {});
    [[nodiscard]] bool configure_participant(ParticipantId id, RiskLimits limits);
    [[nodiscard]] std::optional<RiskLimits> limits(ParticipantId id) const;
    void set_trading_enabled(bool enabled) noexcept { trading_enabled_ = enabled; }
    [[nodiscard]] bool trading_enabled() const noexcept { return trading_enabled_; }
    [[nodiscard]] RiskUsage usage(ParticipantId id, const OrderBook& book) const;

private:
    friend class MatchingEngine;
    // MatchingEngine checks enum/quantity/price/ID and replace ownership first.
    [[nodiscard]] std::optional<RejectReason> validate(
        const NewOrder& request, const OrderBook& book,
        const std::optional<Order>& replaced = std::nullopt) const;

    static void validate_limits(const RiskLimits& limits);
    std::unordered_map<ParticipantId, RiskLimits> participants_;
    bool trading_enabled_{true};
    RiskTraversal traversal_;
};
} // namespace apex
