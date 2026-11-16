#pragma once

#include "apex/core/command.hpp"
#include "apex/core/order.hpp"

#include <variant>

namespace apex {
struct OrderAccepted {
    Order order; // Validated incoming order, before matching.
    bool operator==(const OrderAccepted&) const = default;
};
struct OrderUpdated {
    Order order; // State after a fill or after resting.
    bool operator==(const OrderUpdated&) const = default;
};
struct OrderCancelled {
    Order order; // Terminal state; remaining_quantity records the cancelled units.
    CancelReason reason{CancelReason::ClientRequest};
    bool operator==(const OrderCancelled&) const = default;
};
struct CommandRejected {
    Command command; // Retains both IDs for a rejected replacement.
    RejectReason reason{RejectReason::UnknownOrder};
    bool operator==(const CommandRejected&) const = default;
};

using EventPayload = std::variant<OrderAccepted, OrderUpdated, OrderCancelled,
                                  CommandRejected, Trade>;

struct Event {
    EventSequence event_sequence{};
    CommandSequence command_sequence{}; // Causal command; several events may share it.
    EventPayload payload;
    bool operator==(const Event&) const = default;
};
} // namespace apex
