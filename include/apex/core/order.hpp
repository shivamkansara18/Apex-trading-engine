#pragma once

#include "apex/core/types.hpp"

namespace apex {
struct Order {
    OrderId client_order_id{};
    ParticipantId participant_id{};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    Price price_ticks{};
    Quantity original_quantity{};
    Quantity remaining_quantity{};
    CommandSequence priority_sequence{};
    OrderStatus status{OrderStatus::PendingValidation};
    bool operator==(const Order&) const = default;
};

struct Trade {
    TradeId trade_id{};
    OrderId buy_order_id{};
    OrderId sell_order_id{};
    ParticipantId buyer_id{};
    ParticipantId seller_id{};
    OrderId resting_order_id{};
    Price price_ticks{};
    Quantity quantity{}; // One shared executed quantity for both counterparties.
    bool operator==(const Trade&) const = default;
};
} // namespace apex
