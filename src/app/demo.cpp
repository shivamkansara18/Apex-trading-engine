#include "demo.hpp"
#include "apex/matching/matching_engine.hpp"

#include <iostream>
#include <string_view>
#include <type_traits>

namespace {
std::string_view reject_reason(apex::RejectReason reason) {
    switch (reason) {
    case apex::RejectReason::InvalidSide: return "invalid side";
    case apex::RejectReason::InvalidOrderType: return "invalid order type";
    case apex::RejectReason::ZeroQuantity: return "zero quantity";
    case apex::RejectReason::InvalidPrice: return "invalid price";
    case apex::RejectReason::DuplicateOrderId: return "duplicate order ID";
    case apex::RejectReason::UnknownOrder: return "unknown order";
    case apex::RejectReason::WrongParticipant: return "wrong participant";
    case apex::RejectReason::UnknownParticipant: return "unknown participant";
    case apex::RejectReason::QuantityLimit: return "quantity limit";
    case apex::RejectReason::NotionalLimit: return "notional limit";
    case apex::RejectReason::OpenOrderLimit: return "open order limit";
    case apex::RejectReason::PriceBand: return "price band";
    case apex::RejectReason::ArithmeticOverflow: return "arithmetic overflow";
    case apex::RejectReason::InvalidReplacement: return "invalid replacement";
    case apex::RejectReason::TradingDisabled: return "trading disabled";
    }
    return "unknown";
}
std::string_view cancel_reason(apex::CancelReason reason) {
    switch (reason) {
    case apex::CancelReason::ClientRequest: return "client request";
    case apex::CancelReason::MarketRemainder: return "market remainder";
    case apex::CancelReason::SelfTradePrevention: return "self-trade prevention";
    case apex::CancelReason::Replaced: return "replaced";
    }
    return "unknown";
}
void print_event(const apex::Event& event) {
    std::cout << "  event=" << event.event_sequence << " command=" << event.command_sequence << ' ';
    std::visit([](const auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, apex::Trade>) {
            std::cout << "TRADE buy=" << payload.buy_order_id << " sell=" << payload.sell_order_id
                      << " quantity=" << payload.quantity << " price_ticks=" << payload.price_ticks;
        } else if constexpr (std::is_same_v<T, apex::CommandRejected>) {
            std::cout << "REJECT reason=" << reject_reason(payload.reason);
        } else {
            if constexpr (std::is_same_v<T, apex::OrderAccepted>) { std::cout << "ACCEPT"; }
            else if constexpr (std::is_same_v<T, apex::OrderUpdated>) { std::cout << "UPDATE"; }
            else { std::cout << "CANCEL reason=" << cancel_reason(payload.reason); }
            std::cout << " order=" << payload.order.client_order_id
                      << " remaining=" << payload.order.remaining_quantity;
        }
    }, event.payload);
    std::cout << '\n';
}
}

int run_trading_demo() {
    apex::MatchingEngine engine;
    for (apex::ParticipantId id = 1; id <= 5; ++id) {
        if (!engine.register_participant(id)) { return 1; }
    }
    const auto submit = [&](std::string_view description, const apex::Command& command) {
        std::cout << '\n' << description << '\n';
        for (const auto& event : engine.process(command)) { print_event(event); }
        const auto snapshot = engine.snapshot();
        std::cout << "  BOOK (integer ticks)\n";
        for (const auto& level : snapshot.asks) {
            std::cout << "  ASK " << level.price_ticks << " quantity=" << level.total_quantity << '\n';
        }
        for (const auto& level : snapshot.bids) {
            std::cout << "  BID " << level.price_ticks << " quantity=" << level.total_quantity << '\n';
        }
        if (snapshot.bids.empty() && snapshot.asks.empty()) { std::cout << "  empty\n"; }
    };
    submit("A: BUY 100 @ 10000", apex::NewOrder{1, 1, apex::Side::Buy, apex::OrderType::Limit, 10'000, 100, 0});
    submit("B: BUY 50 @ 10000", apex::NewOrder{2, 2, apex::Side::Buy, apex::OrderType::Limit, 10'000, 50, 0});
    submit("C: SELL 120 @ 9900 (FIFO fills A then B)",
           apex::NewOrder{3, 3, apex::Side::Sell, apex::OrderType::Limit, 9'900, 120, 0});
    submit("B: REPLACE order 2 with order 4, remaining 40 @ 10100", apex::ReplaceOrder{2, 4, 2, 10'100, 40});
    submit("D: SELL 60 @ 10200", apex::NewOrder{5, 4, apex::Side::Sell, apex::OrderType::Limit, 10'200, 60, 0});
    submit("E: MARKET BUY 80 (fills 60; cancels remaining 20)",
           apex::NewOrder{6, 5, apex::Side::Buy, apex::OrderType::Market, 0, 80, 0});
    submit("B: CANCEL order 4", apex::CancelOrder{4, 2});
    if (!engine.book().check_invariants(true)) { return 1; }
    std::cout << "\nTrading demo complete; book invariants hold.\n";
    return 0;
}

int run_risk_demo() {
    apex::MatchingEngine engine;
    apex::RiskLimits limits;
    limits.max_order_quantity = 100;
    limits.max_order_notional = 10'000;
    limits.max_open_orders = 1;
    limits.min_price_ticks = 90;
    limits.max_price_ticks = 110;
    if (!engine.register_participant(1, limits)) { return 1; }
    std::cout << "Risk demo: participant 1; max quantity=100, notional=10000 tick-units,\n"
              << "max open orders=1, price band=[90,110].\n";
    const auto submit = [&](std::string_view label, const apex::Command& command,
                            std::optional<apex::RejectReason> expected = std::nullopt) {
        std::cout << '\n' << label << '\n';
        const auto events = engine.process(command);
        std::optional<apex::RejectReason> actual;
        for (const auto& event : events) {
            print_event(event);
            if (const auto* rejection = std::get_if<apex::CommandRejected>(&event.payload)) {
                actual = rejection->reason;
            }
        }
        const auto usage = engine.risk_usage(1);
        std::cout << "  USAGE open=" << usage.open_orders << " buy=" << usage.buy_quantity
                  << " sell=" << usage.sell_quantity << '\n';
        return actual == expected && engine.book().check_invariants(true);
    };
    const auto buy = [](apex::OrderId id, apex::Quantity quantity, apex::Price price,
                        apex::ParticipantId participant = 1) {
        return apex::NewOrder{id, participant, apex::Side::Buy, apex::OrderType::Limit, price, quantity, 0};
    };
    if (!submit("Unknown participant", buy(1, 10, 100, 99), apex::RejectReason::UnknownParticipant) ||
        !submit("Rest valid order 1", buy(1, 10, 100)) ||
        !submit("Oversized order", buy(2, 101, 100), apex::RejectReason::QuantityLimit) ||
        !submit("Excess notional", buy(2, 100, 110), apex::RejectReason::NotionalLimit) ||
        !submit("Outside price band", buy(2, 1, 111), apex::RejectReason::PriceBand) ||
        !submit("Second resting order", buy(2, 1, 100), apex::RejectReason::OpenOrderLimit) ||
        !submit("Invalid replace preserves order 1", apex::ReplaceOrder{1, 2, 1, 100, 101},
                apex::RejectReason::QuantityLimit)) { return 1; }
    engine.set_trading_enabled(false);
    if (!submit("Global kill switch blocks new orders", buy(2, 1, 100), apex::RejectReason::TradingDisabled) ||
        !submit("Cancel remains available", apex::CancelOrder{1, 1})) { return 1; }
    engine.set_trading_enabled(true);
    if (!submit("Resume trading", buy(3, 1, 100))) { return 1; }
    std::cout << "\nRisk demo complete; rejection checks and book invariants hold.\n";
    return 0;
}
