#pragma once

#include "apex/core/types.hpp"

#include <type_traits>
#include <variant>

namespace apex {
struct NewOrder {
    OrderId client_order_id{};
    ParticipantId participant_id{};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    Price price_ticks{}; // Ignored for market orders.
    Quantity quantity{};
    TimestampNs client_timestamp_ns{}; // Informational; never determines priority.
    bool operator==(const NewOrder&) const = default;
};

struct CancelOrder {
    OrderId client_order_id{};
    ParticipantId participant_id{};
    bool operator==(const CancelOrder&) const = default;
};

struct ReplaceOrder {
    OrderId old_client_order_id{};
    OrderId new_client_order_id{};
    ParticipantId participant_id{};
    Price new_price_ticks{};
    Quantity new_quantity{}; // Desired remaining quantity, not lifetime quantity.
    bool operator==(const ReplaceOrder&) const = default;
};

using Command = std::variant<NewOrder, CancelOrder, ReplaceOrder>;

[[nodiscard]] constexpr CommandType command_type(const Command& command) {
    return std::visit([](const auto& request) {
        using Request = std::decay_t<decltype(request)>;
        if constexpr (std::is_same_v<Request, NewOrder>) {
            return CommandType::NewOrder;
        } else if constexpr (std::is_same_v<Request, CancelOrder>) {
            return CommandType::CancelOrder;
        } else {
            return CommandType::ReplaceOrder;
        }
    }, command);
}
} // namespace apex
