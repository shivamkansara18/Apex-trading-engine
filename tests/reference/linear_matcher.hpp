#pragma once
// Test-only specification model. No OrderBook, MatchingEngine, RiskManager,
// price-level/index helpers, production codecs or workload generators are used.
#include "apex/core/event.hpp"
#include <algorithm>
#include <optional>
#include <cstddef>
#include <vector>

namespace apex::test_reference {
struct Cancellation {
    OrderId id; CancelReason reason; Quantity remaining;
    bool operator==(const Cancellation&) const = default;
};
struct Outcome {
    std::optional<RejectReason> rejected;
    std::vector<Trade> trades;
    std::vector<Cancellation> cancellations;
};
class LinearMatcher {
public:
    std::vector<Order> live;
    CommandSequence sequence{};
    TradeId trades{};
    Outcome process(const Command& command) {
        ++sequence;
        return std::visit([&](const auto& request) { return apply(request); }, command);
    }
    std::vector<Order> ordered() const {
        auto result = live;
        std::sort(result.begin(), result.end(), [](const Order& a, const Order& b) {
            if (a.side != b.side) { return a.side == Side::Buy; }
            if (a.price_ticks != b.price_ticks) { return a.side == Side::Buy ? a.price_ticks > b.price_ticks : a.price_ticks < b.price_ticks; }
            return a.priority_sequence < b.priority_sequence;
        });
        return result;
    }
private:
    bool known(ParticipantId participant) const { return participant >= 1 && participant <= 4; }
    std::optional<std::size_t> locate(OrderId id) const {
        for (std::size_t i = 0; i < live.size(); ++i) { if (live[i].client_order_id == id) { return i; } }
        return std::nullopt;
    }
    std::optional<RejectReason> valid(const NewOrder& order) const {
        if (order.side != Side::Buy && order.side != Side::Sell) { return RejectReason::InvalidSide; }
        if (order.type != OrderType::Limit && order.type != OrderType::Market) { return RejectReason::InvalidOrderType; }
        if (!order.quantity) { return RejectReason::ZeroQuantity; }
        if (order.type == OrderType::Limit && order.price_ticks <= 0) { return RejectReason::InvalidPrice; }
        if (locate(order.client_order_id)) { return RejectReason::DuplicateOrderId; }
        if (!known(order.participant_id)) { return RejectReason::UnknownParticipant; }
        return std::nullopt;
    }
    Outcome sweep(const NewOrder& request) {
        Outcome result;
        Order incoming{request.client_order_id,request.participant_id,request.side,request.type,
            request.type == OrderType::Limit ? request.price_ticks : 0, request.quantity, request.quantity,sequence,OrderStatus::Accepted};
        while (incoming.remaining_quantity) {
            // Rebuild the eligible set and sort every execution: deliberately slow,
            // structurally different from indexed tree/list production matching.
            std::vector<std::size_t> eligible;
            for (std::size_t i = 0; i < live.size(); ++i) {
                const auto& maker = live[i];
                if (maker.side == incoming.side) { continue; }
                if (request.type == OrderType::Limit &&
                    (incoming.side == Side::Buy ? maker.price_ticks > incoming.price_ticks : maker.price_ticks < incoming.price_ticks)) { continue; }
                eligible.push_back(i);
            }
            if (eligible.empty()) { break; }
            std::sort(eligible.begin(), eligible.end(), [&](std::size_t x, std::size_t y) {
                if (live[x].price_ticks == live[y].price_ticks) { return live[x].priority_sequence < live[y].priority_sequence; }
                return incoming.side == Side::Buy ? live[x].price_ticks < live[y].price_ticks : live[x].price_ticks > live[y].price_ticks;
            });
            const auto index = eligible.front(); auto& maker = live[index];
            if (maker.participant_id == incoming.participant_id) {
                result.cancellations.push_back({incoming.client_order_id,CancelReason::SelfTradePrevention,incoming.remaining_quantity});
                return result;
            }
            const auto quantity = std::min(maker.remaining_quantity,incoming.remaining_quantity);
            const auto& buy = incoming.side == Side::Buy ? incoming : maker;
            const auto& sell = incoming.side == Side::Sell ? incoming : maker;
            result.trades.push_back({++trades,buy.client_order_id,sell.client_order_id,buy.participant_id,sell.participant_id,
                maker.client_order_id,maker.price_ticks,quantity});
            incoming.remaining_quantity -= quantity; maker.remaining_quantity -= quantity;
            if (!maker.remaining_quantity) { live.erase(live.begin() + static_cast<std::ptrdiff_t>(index)); }
            else { maker.status = OrderStatus::PartiallyFilled; }
        }
        if (incoming.remaining_quantity) {
            if (incoming.type == OrderType::Market) {
                result.cancellations.push_back({incoming.client_order_id,CancelReason::MarketRemainder,incoming.remaining_quantity});
            } else {
                incoming.status = incoming.remaining_quantity == incoming.original_quantity ? OrderStatus::Resting : OrderStatus::PartiallyFilled;
                live.push_back(incoming);
            }
        }
        return result;
    }
    Outcome apply(const NewOrder& request) {
        if (const auto reason = valid(request)) { return {reason,{},{}}; }
        return sweep(request);
    }
    Outcome apply(const CancelOrder& request) {
        if (!known(request.participant_id)) { return {RejectReason::UnknownParticipant,{},{}}; }
        const auto index = locate(request.client_order_id);
        if (!index) { return {RejectReason::UnknownOrder,{},{}}; }
        const auto old = live[*index];
        if (old.participant_id != request.participant_id) { return {RejectReason::WrongParticipant,{},{}}; }
        live.erase(live.begin() + static_cast<std::ptrdiff_t>(*index));
        return {std::nullopt,{},{{old.client_order_id,CancelReason::ClientRequest,old.remaining_quantity}}};
    }
    Outcome apply(const ReplaceOrder& request) {
        if (!known(request.participant_id)) { return {RejectReason::UnknownParticipant,{},{}}; }
        const auto index = locate(request.old_client_order_id);
        if (!index) { return {RejectReason::UnknownOrder,{},{}}; }
        const auto old = live[*index];
        if (old.participant_id != request.participant_id) { return {RejectReason::WrongParticipant,{},{}}; }
        if (request.old_client_order_id == request.new_client_order_id) { return {RejectReason::InvalidReplacement,{},{}}; }
        const NewOrder next{request.new_client_order_id,request.participant_id,old.side,OrderType::Limit,request.new_price_ticks,request.new_quantity,0};
        if (const auto reason = valid(next)) { return {reason,{},{}}; }
        live.erase(live.begin() + static_cast<std::ptrdiff_t>(*index));
        auto result = sweep(next);
        result.cancellations.insert(result.cancellations.begin(),{old.client_order_id,CancelReason::Replaced,old.remaining_quantity});
        return result;
    }
};
} // namespace apex::test_reference
