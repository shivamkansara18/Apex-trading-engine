#include "apex/matching/matching_engine.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace apex {
MatchingEngine::MatchingEngine(EngineOptions options)
    : book_(options.price_index), risk_(options.risk_traversal), options_(options) {
    if ((options.risk_traversal != RiskTraversal::Snapshot && options.risk_traversal != RiskTraversal::Direct) ||
        (options.event_reservation != EventReservation::WorstCase && options.event_reservation != EventReservation::SmallInitial)) {
        throw std::invalid_argument("Unknown engine performance policy");
    }
}
std::vector<Event> MatchingEngine::process(const Command& command) {
    // At most one trade and two updates per existing order, plus acceptance,
    // replacement cancellation and a residual event. Reserve before mutation.
    const auto count = book_.order_count();
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (command_sequence_ == maximum || count > (maximum - 4) / 3 ||
        event_sequence_ > maximum - (3 * count + 4) || trade_id_ > maximum - count) {
        throw std::overflow_error("Exchange sequence capacity exhausted");
    }
    std::vector<Event> events;
    const auto capacity = 3 * count + 4;
    events.reserve(options_.event_reservation == EventReservation::SmallInitial ? std::min<std::size_t>(8, capacity) : capacity);
    ++command_sequence_;
    std::visit([&](const auto& request) { apply(request, events); }, command);
    return events;
}

void MatchingEngine::emit(std::vector<Event>& events, EventPayload payload) {
    events.push_back({++event_sequence_, command_sequence_, std::move(payload)});
}
void MatchingEngine::reject(std::vector<Event>& events, const Command& command, RejectReason reason) {
    emit(events, CommandRejected{command, reason});
}

std::optional<RejectReason> MatchingEngine::validate(const NewOrder& request, Quantity released) const {
    if (request.side != Side::Buy && request.side != Side::Sell) { return RejectReason::InvalidSide; }
    if (request.type != OrderType::Limit && request.type != OrderType::Market) {
        return RejectReason::InvalidOrderType;
    }
    if (request.quantity == 0) { return RejectReason::ZeroQuantity; }
    if (request.type == OrderType::Limit && request.price_ticks <= 0) { return RejectReason::InvalidPrice; }
    if (book_.find(request.client_order_id)) { return RejectReason::DuplicateOrderId; }
    // Conservative capacity admission: reserve the entire incoming limit size,
    // even if it could execute. Replacement releases the old remaining size.
    if (request.type == OrderType::Limit && request.quantity >
        std::numeric_limits<Quantity>::max() - (book_.total_quantity(request.side) - released)) {
        return RejectReason::ArithmeticOverflow;
    }
    return std::nullopt;
}

void MatchingEngine::apply(const NewOrder& request, std::vector<Event>& events) {
    if (const auto reason = validate(request)) { reject(events, request, *reason); return; }
    if (const auto reason = risk_.validate(request, book_)) { reject(events, request, *reason); return; }
    match(request, events);
}
void MatchingEngine::apply(const CancelOrder& request, std::vector<Event>& events) {
    if (!risk_.limits(request.participant_id)) {
        reject(events, request, RejectReason::UnknownParticipant); return;
    }
    auto order = book_.find(request.client_order_id);
    if (!order) { reject(events, request, RejectReason::UnknownOrder); return; }
    if (order->participant_id != request.participant_id) {
        reject(events, request, RejectReason::WrongParticipant); return;
    }
    if (book_.cancel(request.client_order_id, request.participant_id) != BookError::None) {
        throw std::logic_error("Validated cancellation failed");
    }
    order->status = OrderStatus::Cancelled;
    emit(events, OrderCancelled{*order, CancelReason::ClientRequest});
}
void MatchingEngine::apply(const ReplaceOrder& request, std::vector<Event>& events) {
    if (!risk_.limits(request.participant_id)) {
        reject(events, request, RejectReason::UnknownParticipant); return;
    }
    auto old = book_.find(request.old_client_order_id);
    if (!old) { reject(events, request, RejectReason::UnknownOrder); return; }
    if (old->participant_id != request.participant_id) {
        reject(events, request, RejectReason::WrongParticipant); return;
    }
    if (request.old_client_order_id == request.new_client_order_id) {
        reject(events, request, RejectReason::InvalidReplacement); return;
    }
    const NewOrder replacement{request.new_client_order_id, request.participant_id,
                               old->side, OrderType::Limit, request.new_price_ticks,
                               request.new_quantity, 0};
    if (const auto reason = validate(replacement, old->remaining_quantity)) {
        reject(events, request, *reason); return;
    }
    if (const auto reason = risk_.validate(replacement, book_, old)) {
        reject(events, request, *reason); return;
    }
    if (book_.cancel(old->client_order_id, old->participant_id) != BookError::None) {
        throw std::logic_error("Validated replacement cancellation failed");
    }
    old->status = OrderStatus::Cancelled;
    emit(events, OrderCancelled{*old, CancelReason::Replaced});
    match(replacement, events);
}

void MatchingEngine::match(const NewOrder& request, std::vector<Event>& events) {
    Order incoming{request.client_order_id, request.participant_id, request.side, request.type,
                   request.type == OrderType::Limit ? request.price_ticks : 0,
                   request.quantity, request.quantity, command_sequence_, OrderStatus::Accepted};
    emit(events, OrderAccepted{incoming});
    const auto opposite = incoming.side == Side::Buy ? Side::Sell : Side::Buy;
    while (incoming.remaining_quantity != 0) {
        const auto resting = book_.best_order(opposite);
        if (!resting) { break; }
        if (incoming.type == OrderType::Limit &&
            (incoming.side == Side::Buy ? resting->price_ticks > incoming.price_ticks
                                        : resting->price_ticks < incoming.price_ticks)) { break; }
        if (resting->participant_id == incoming.participant_id) {
            incoming.status = OrderStatus::Cancelled;
            emit(events, OrderCancelled{incoming, CancelReason::SelfTradePrevention});
            return;
        }
        const auto quantity = std::min(incoming.remaining_quantity, resting->remaining_quantity);
        const auto updated = book_.fill(resting->client_order_id, quantity);
        if (!updated) { throw std::logic_error("Validated execution failed"); }
        incoming.remaining_quantity -= quantity;
        incoming.status = incoming.remaining_quantity == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
        const auto& buy = incoming.side == Side::Buy ? incoming : *resting;
        const auto& sell = incoming.side == Side::Sell ? incoming : *resting;
        emit(events, Trade{++trade_id_, buy.client_order_id, sell.client_order_id,
                           buy.participant_id, sell.participant_id, resting->client_order_id,
                           resting->price_ticks, quantity});
        emit(events, OrderUpdated{*updated});
        emit(events, OrderUpdated{incoming});
    }
    if (incoming.remaining_quantity == 0) { return; }
    if (incoming.type == OrderType::Market) {
        incoming.status = OrderStatus::Cancelled;
        emit(events, OrderCancelled{incoming, CancelReason::MarketRemainder});
        return;
    }
    incoming.status = incoming.remaining_quantity == incoming.original_quantity
        ? OrderStatus::Resting : OrderStatus::PartiallyFilled;
    if (book_.insert(incoming) != BookError::None) { throw std::logic_error("Validated resting insertion failed"); }
    emit(events, OrderUpdated{incoming});
}
} // namespace apex
