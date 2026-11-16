#include "apex/risk/risk_manager.hpp"

#include <algorithm>
#include <stdexcept>

namespace apex {
void RiskManager::validate_limits(const RiskLimits& limits) {
    if (limits.min_price_ticks <= 0 || limits.max_price_ticks < limits.min_price_ticks) {
        throw std::invalid_argument("Risk price band must be positive and ordered");
    }
}
bool RiskManager::register_participant(ParticipantId id, RiskLimits limits) {
    validate_limits(limits);
    return participants_.emplace(id, limits).second;
}
bool RiskManager::configure_participant(ParticipantId id, RiskLimits limits) {
    validate_limits(limits);
    const auto found = participants_.find(id);
    if (found == participants_.end()) { return false; }
    found->second = limits;
    return true;
}
std::optional<RiskLimits> RiskManager::limits(ParticipantId id) const {
    const auto found = participants_.find(id);
    if (found == participants_.end()) { return std::nullopt; }
    return found->second;
}

RiskUsage RiskManager::usage(ParticipantId id, const OrderBook& book) const {
    RiskUsage result;
    const auto collect_order = [&](const Order& order) {
        if (order.participant_id == id) {
            ++result.open_orders;
            (order.side == Side::Buy ? result.buy_quantity : result.sell_quantity) += order.remaining_quantity;
        }
        return true;
    };
    if (traversal_ == RiskTraversal::Direct) {
        book.visit_orders(Side::Buy, collect_order);
        book.visit_orders(Side::Sell, collect_order);
    } else {
        const auto snapshot = book.snapshot();
        for (const auto& level : snapshot.bids) { for (const auto& order : level.orders) { collect_order(order); } }
        for (const auto& level : snapshot.asks) { for (const auto& order : level.orders) { collect_order(order); } }
    }
    return result;
}

std::optional<RejectReason> RiskManager::validate(
    const NewOrder& request, const OrderBook& book, const std::optional<Order>& replaced) const {
    const auto config = limits(request.participant_id);
    if (!config) { return RejectReason::UnknownParticipant; }
    if (!trading_enabled_ || !config->trading_enabled) { return RejectReason::TradingDisabled; }
    if (request.quantity > config->max_order_quantity) { return RejectReason::QuantityLimit; }

    if (request.type == OrderType::Limit) {
        if (request.price_ticks < config->min_price_ticks || request.price_ticks > config->max_price_ticks) {
            return RejectReason::PriceBand;
        }
        const auto price = static_cast<std::uint64_t>(request.price_ticks);
        if (request.quantity > std::numeric_limits<std::uint64_t>::max() / price) {
            return RejectReason::ArithmeticOverflow;
        }
        if (request.quantity * price > config->max_order_notional) { return RejectReason::NotionalLimit; }
        const auto open = usage(request.participant_id, book).open_orders - (replaced ? 1U : 0U);
        if (open >= config->max_open_orders) { return RejectReason::OpenOrderLimit; }
        return std::nullopt;
    }

    // Market notional and bands use the actual executable sweep, not a supplied
    // market price. Stop at own liquidity exactly as matching does.
    auto remaining = request.quantity;
    std::uint64_t notional = 0;
    std::optional<RejectReason> rejection;
    const auto inspect = [&](const Order& order) {
        if (remaining == 0 || order.participant_id == request.participant_id) { return false; }
        if (order.price_ticks < config->min_price_ticks || order.price_ticks > config->max_price_ticks) {
            rejection = RejectReason::PriceBand; return false;
        }
        const auto quantity = std::min(remaining, order.remaining_quantity);
        const auto price = static_cast<std::uint64_t>(order.price_ticks);
        if (quantity > (std::numeric_limits<std::uint64_t>::max() - notional) / price) {
            rejection = RejectReason::ArithmeticOverflow; return false;
        }
        notional += quantity * price;
        if (notional > config->max_order_notional) { rejection = RejectReason::NotionalLimit; return false; }
        remaining -= quantity;
        return true;
    };
    if (traversal_ == RiskTraversal::Direct) {
        book.visit_orders(request.side == Side::Buy ? Side::Sell : Side::Buy, inspect);
    } else {
        const auto snapshot = book.snapshot();
        const auto& levels = request.side == Side::Buy ? snapshot.asks : snapshot.bids;
        for (const auto& level : levels) {
            for (const auto& order : level.orders) { if (!inspect(order)) { return rejection; } }
        }
    }
    return rejection;
}
} // namespace apex
