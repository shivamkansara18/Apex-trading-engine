#include "apex/matching/matching_engine.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <random>

namespace {
apex::NewOrder request(apex::OrderId id, apex::ParticipantId participant, apex::Side side,
                       apex::Price price, apex::Quantity quantity,
                       apex::OrderType type = apex::OrderType::Limit) {
    return {id, participant, side, type, price, quantity, 0};
}
template<class T>
std::vector<T> payloads(const std::vector<apex::Event>& events) {
    std::vector<T> result;
    for (const auto& event : events) {
        if (const auto* payload = std::get_if<T>(&event.payload)) { result.push_back(*payload); }
    }
    return result;
}
void register_defaults(apex::MatchingEngine& engine) {
    for (apex::ParticipantId id = 1; id <= 5; ++id) {
        ASSERT_TRUE(engine.register_participant(id));
    }
}
void submit(apex::MatchingEngine& engine, const apex::Command& command) {
    const auto events = engine.process(command);
    ASSERT_TRUE(payloads<apex::CommandRejected>(events).empty());
    ASSERT_TRUE(engine.book().check_invariants(true));
}
}

TEST(MatchingEngine, GuideFifoScenarioUsesRestingPrice) {
    apex::MatchingEngine engine;
    register_defaults(engine);
    auto first = request(1, 1, apex::Side::Buy, 10'000, 100);
    first.client_timestamp_ns = 999;
    submit(engine, first);
    submit(engine, request(2, 2, apex::Side::Buy, 10'000, 50));
    const auto events = engine.process(request(3, 3, apex::Side::Sell, 9'900, 120));
    const auto trades = payloads<apex::Trade>(events);
    ASSERT_EQ(trades.size(), 2U);
    EXPECT_EQ(trades[0], (apex::Trade{1, 1, 3, 1, 3, 1, 10'000, 100}));
    EXPECT_EQ(trades[1], (apex::Trade{2, 2, 3, 2, 3, 2, 10'000, 20}));
    ASSERT_TRUE(engine.book().find(2));
    EXPECT_EQ(engine.book().find(2)->remaining_quantity, 30U);
    EXPECT_EQ(engine.book().find(2)->priority_sequence, 2U);
    EXPECT_FALSE(engine.book().find(1));
    EXPECT_FALSE(engine.book().best_ask());
    ASSERT_EQ(events.size(), 7U);
    EXPECT_TRUE(std::holds_alternative<apex::OrderAccepted>(events[0].payload));
    EXPECT_TRUE(std::holds_alternative<apex::Trade>(events[1].payload));
    EXPECT_EQ(std::get<apex::OrderUpdated>(events[2].payload).order.status, apex::OrderStatus::Filled);
    EXPECT_EQ(std::get<apex::OrderUpdated>(events[6].payload).order.status, apex::OrderStatus::Filled);
    EXPECT_TRUE(engine.book().check_invariants(true));
}

TEST(MatchingEngine, BetterPriceWinsBeforeEarlierArrivalOnBothSides) {
    for (const auto side : {apex::Side::Buy, apex::Side::Sell}) {
        apex::MatchingEngine engine;
        register_defaults(engine);
        const bool buying = side == apex::Side::Buy;
        const auto opposite = buying ? apex::Side::Sell : apex::Side::Buy;
        submit(engine, request(1, 1, opposite, buying ? 102 : 98, 10));
        submit(engine, request(2, 2, opposite, 100, 10));
        const auto events = engine.process(request(3, 3, side, buying ? 102 : 98, 15));
        const auto trades = payloads<apex::Trade>(events);
        ASSERT_EQ(trades.size(), 2U);
        EXPECT_EQ(trades[0].resting_order_id, 2U);
        EXPECT_EQ(trades[0].price_ticks, 100);
        EXPECT_EQ(trades[0].quantity, 10U);
        EXPECT_EQ(trades[1].resting_order_id, 1U);
        EXPECT_EQ(trades[1].quantity, 5U);
        EXPECT_TRUE(engine.book().check_invariants(true));
    }
}

TEST(MatchingEngine, NonCrossingLimitsRestAndPartialResidualKeepsIncomingPrice) {
    apex::MatchingEngine engine;
    register_defaults(engine);
    submit(engine, request(1, 1, apex::Side::Sell, 101, 10));
    submit(engine, request(2, 2, apex::Side::Buy, 100, 5));
    auto events = engine.process(request(3, 3, apex::Side::Buy, 102, 15));
    ASSERT_EQ(payloads<apex::Trade>(events).size(), 1U);
    ASSERT_TRUE(engine.book().find(3));
    EXPECT_EQ(engine.book().find(3)->price_ticks, 102);
    EXPECT_EQ(engine.book().find(3)->remaining_quantity, 5U);
    EXPECT_EQ(engine.book().find(3)->status, apex::OrderStatus::PartiallyFilled);
    EXPECT_EQ(engine.book().best_bid(), 102);
    EXPECT_TRUE(engine.book().check_invariants(true));
}

TEST(MatchingEngine, MarketOrdersIgnorePriceAndCancelResidualOnBothSides) {
    for (const auto side : {apex::Side::Buy, apex::Side::Sell}) {
        apex::MatchingEngine engine;
        register_defaults(engine);
        const auto opposite = side == apex::Side::Buy ? apex::Side::Sell : apex::Side::Buy;
        submit(engine, request(1, 1, opposite, 100, 10));
        const auto events = engine.process(request(2, 2, side, -99, 15, apex::OrderType::Market));
        ASSERT_EQ(payloads<apex::Trade>(events).size(), 1U);
        const auto cancelled = payloads<apex::OrderCancelled>(events);
        ASSERT_EQ(cancelled.size(), 1U);
        EXPECT_EQ(cancelled[0].order.remaining_quantity, 5U);
        EXPECT_EQ(cancelled[0].order.price_ticks, 0);
        EXPECT_EQ(cancelled[0].reason, apex::CancelReason::MarketRemainder);
        EXPECT_EQ(engine.book().order_count(), 0U);
        EXPECT_TRUE(engine.book().check_invariants(true));
    }
}

TEST(MatchingEngine, EmptyMarketCancelsAndExactMarketFillDoesNotCancel) {
    apex::MatchingEngine engine;
    register_defaults(engine);
    auto events = engine.process(request(1, 1, apex::Side::Buy, 0, 10, apex::OrderType::Market));
    ASSERT_EQ(payloads<apex::OrderCancelled>(events).size(), 1U);
    submit(engine, request(2, 2, apex::Side::Sell, 100, 10));
    events = engine.process(request(3, 3, apex::Side::Buy, 0, 10, apex::OrderType::Market));
    EXPECT_TRUE(payloads<apex::OrderCancelled>(events).empty());
    EXPECT_EQ(engine.book().order_count(), 0U);
}

TEST(MatchingEngine, LimitStopsAtIneligiblePrice) {
    apex::MatchingEngine engine;
    register_defaults(engine);
    submit(engine, request(1, 1, apex::Side::Sell, 100, 10));
    submit(engine, request(2, 2, apex::Side::Sell, 102, 10));
    const auto events = engine.process(request(3, 3, apex::Side::Buy, 101, 30));
    ASSERT_EQ(payloads<apex::Trade>(events).size(), 1U);
    EXPECT_EQ(engine.book().find(3)->remaining_quantity, 20U);
    EXPECT_EQ(engine.book().best_ask(), 102);
    EXPECT_TRUE(engine.book().check_invariants(true));
}

TEST(MatchingEngine, SelfTradeCancelsIncomingRemainderWithoutSkippingOwnOrder) {
    apex::MatchingEngine engine;
    register_defaults(engine);
    submit(engine, request(1, 1, apex::Side::Sell, 100, 10));
    submit(engine, request(2, 2, apex::Side::Sell, 101, 10));
    submit(engine, request(3, 3, apex::Side::Sell, 102, 10));
    const auto events = engine.process(request(4, 2, apex::Side::Buy, 103, 30));
    const auto trades = payloads<apex::Trade>(events);
    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades[0].resting_order_id, 1U);
    const auto cancelled = payloads<apex::OrderCancelled>(events);
    ASSERT_EQ(cancelled.size(), 1U);
    EXPECT_EQ(cancelled[0].reason, apex::CancelReason::SelfTradePrevention);
    EXPECT_EQ(cancelled[0].order.remaining_quantity, 20U);
    EXPECT_EQ(engine.book().find(2)->remaining_quantity, 10U);
    EXPECT_TRUE(engine.book().find(3));
    EXPECT_FALSE(engine.book().find(4));
    EXPECT_TRUE(engine.book().check_invariants(true));
}

TEST(MatchingEngine, ImmediateSelfTradeAlsoProtectsMarketSell) {
    apex::MatchingEngine engine;
    register_defaults(engine);
    submit(engine, request(1, 1, apex::Side::Buy, 100, 10));
    const auto events = engine.process(request(2, 1, apex::Side::Sell, 0, 5, apex::OrderType::Market));
    EXPECT_TRUE(payloads<apex::Trade>(events).empty());
    EXPECT_EQ(payloads<apex::OrderCancelled>(events)[0].reason, apex::CancelReason::SelfTradePrevention);
    EXPECT_EQ(engine.book().find(1)->remaining_quantity, 10U);
}

TEST(MatchingEngine, CancelReportsRemainingQuantityAndRejectsWrongOwner) {
    apex::MatchingEngine engine;
    register_defaults(engine);
    submit(engine, request(1, 1, apex::Side::Buy, 100, 10));
    auto events = engine.process(apex::CancelOrder{1, 2});
    EXPECT_EQ(payloads<apex::CommandRejected>(events)[0].reason, apex::RejectReason::WrongParticipant);
    EXPECT_TRUE(engine.book().find(1));
    events = engine.process(apex::CancelOrder{1, 1});
    EXPECT_EQ(payloads<apex::OrderCancelled>(events)[0].order.remaining_quantity, 10U);
    EXPECT_FALSE(engine.book().find(1));
    events = engine.process(apex::CancelOrder{1, 1});
    EXPECT_EQ(payloads<apex::CommandRejected>(events)[0].reason, apex::RejectReason::UnknownOrder);
}

TEST(MatchingEngine, QuantityReductionReplaceLosesPriority) {
    apex::MatchingEngine engine;
    register_defaults(engine);
    submit(engine, request(1, 1, apex::Side::Buy, 100, 10));
    submit(engine, request(2, 2, apex::Side::Buy, 100, 10));
    auto events = engine.process(apex::ReplaceOrder{1, 3, 1, 100, 5});
    ASSERT_GE(events.size(), 3U);
    EXPECT_EQ(std::get<apex::OrderCancelled>(events[0].payload).reason, apex::CancelReason::Replaced);
    EXPECT_TRUE(std::holds_alternative<apex::OrderAccepted>(events[1].payload));
    EXPECT_FALSE(engine.book().find(1));
    EXPECT_EQ(engine.book().find(3)->priority_sequence, 3U);
    events = engine.process(request(4, 4, apex::Side::Sell, 100, 12));
    const auto trades = payloads<apex::Trade>(events);
    ASSERT_EQ(trades.size(), 2U);
    EXPECT_EQ(trades[0].buy_order_id, 2U);
    EXPECT_EQ(trades[1].buy_order_id, 3U);
    EXPECT_EQ(engine.book().find(3)->remaining_quantity, 3U);
}

TEST(MatchingEngine, ReplaceCanCrossAndQuantityMeansDesiredRemaining) {
    apex::MatchingEngine engine;
    register_defaults(engine);
    submit(engine, request(1, 1, apex::Side::Buy, 99, 10));
    submit(engine, request(2, 2, apex::Side::Sell, 101, 5));
    auto events = engine.process(apex::ReplaceOrder{1, 3, 1, 101, 8});
    EXPECT_EQ(payloads<apex::Trade>(events).size(), 1U);
    EXPECT_EQ(engine.book().find(3)->remaining_quantity, 3U);
    submit(engine, apex::ReplaceOrder{3, 4, 1, 100, 2});
    EXPECT_EQ(engine.book().find(4)->original_quantity, 2U);
    EXPECT_EQ(engine.book().find(4)->remaining_quantity, 2U);
    EXPECT_TRUE(engine.book().check_invariants(true));
}

TEST(MatchingEngine, InvalidReplacementsPreserveExistingBook) {
    apex::MatchingEngine engine;
    register_defaults(engine);
    submit(engine, request(1, 1, apex::Side::Buy, 100, 10));
    submit(engine, request(2, 2, apex::Side::Buy, 100, 10));
    const auto before = engine.snapshot();
    for (const auto& replacement : {apex::ReplaceOrder{1, 3, 2, 100, 1},
             apex::ReplaceOrder{1, 1, 1, 100, 1}, apex::ReplaceOrder{1, 2, 1, 100, 1},
             apex::ReplaceOrder{1, 3, 1, 0, 1}, apex::ReplaceOrder{1, 3, 1, 100, 0},
             apex::ReplaceOrder{99, 3, 1, 100, 1}}) {
        const auto events = engine.process(replacement);
        ASSERT_EQ(events.size(), 1U);
        EXPECT_TRUE(std::holds_alternative<apex::CommandRejected>(events[0].payload));
        EXPECT_EQ(engine.snapshot().bids, before.bids);
        EXPECT_EQ(engine.snapshot().asks, before.asks);
        EXPECT_TRUE(engine.book().check_invariants(true));
    }
}

TEST(MatchingEngine, InvalidNewRequestsRejectWithoutBookMutation) {
    apex::MatchingEngine engine;
    register_defaults(engine);
    submit(engine, request(1, 1, apex::Side::Buy, 100, 10));
    const auto before = engine.snapshot();
    auto bad_side = request(2, 2, apex::Side::Sell, 100, 5);
    bad_side.side = static_cast<apex::Side>(99);
    auto bad_type = request(2, 2, apex::Side::Sell, 100, 5);
    bad_type.type = static_cast<apex::OrderType>(99);
    for (const auto& invalid : {request(1, 2, apex::Side::Sell, 100, 5),
             request(2, 2, apex::Side::Sell, 0, 5), request(2, 2, apex::Side::Sell, 100, 0),
             bad_side, bad_type}) {
        const auto events = engine.process(invalid);
        ASSERT_EQ(events.size(), 1U);
        EXPECT_TRUE(std::holds_alternative<apex::CommandRejected>(events[0].payload));
        EXPECT_EQ(engine.snapshot().bids, before.bids);
        EXPECT_EQ(engine.snapshot().asks, before.asks);
    }
}

TEST(MatchingEngine, CapacityOverflowRejectsAndReplacementReleasesOldSize) {
    apex::MatchingEngine engine;
    register_defaults(engine);
    const auto maximum = std::numeric_limits<apex::Quantity>::max();
    submit(engine, request(1, 1, apex::Side::Buy, 1, maximum));
    const auto events = engine.process(request(2, 2, apex::Side::Buy, 99, 1));
    EXPECT_EQ(payloads<apex::CommandRejected>(events)[0].reason, apex::RejectReason::ArithmeticOverflow);
    submit(engine, apex::ReplaceOrder{1, 3, 1, 1, maximum});
    EXPECT_EQ(engine.book().total_quantity(apex::Side::Buy), maximum);
    EXPECT_TRUE(engine.book().check_invariants(true));
}

TEST(MatchingEngine, RepeatedSeededStreamHasIdenticalEventsAndState) {
    apex::MatchingEngine first;
    apex::MatchingEngine second;
    register_defaults(first);
    register_defaults(second);
    std::mt19937 random(123);
    apex::EventSequence last_event = 0;
    for (apex::OrderId step = 1; step <= 1500; ++step) {
        apex::Command command;
        if (random() % 4 == 0) {
            command = apex::CancelOrder{random() % step, 1 + random() % 5};
        } else if (random() % 5 == 0) {
            command = apex::ReplaceOrder{random() % step, step, 1 + random() % 5,
                                          95 + random() % 10, 1 + random() % 50};
        } else {
            command = request(step, static_cast<apex::ParticipantId>(1 + random() % 5),
                random() % 2 == 0 ? apex::Side::Buy : apex::Side::Sell,
                95 + random() % 10, 1 + random() % 50,
                random() % 5 == 0 ? apex::OrderType::Market : apex::OrderType::Limit);
        }
        const auto events = first.process(command);
        ASSERT_EQ(events, second.process(command)) << "step=" << step;
        ASSERT_EQ(first.snapshot(), second.snapshot());
        ASSERT_TRUE(first.book().check_invariants(true));
        for (const auto& event : events) {
            EXPECT_EQ(event.event_sequence, ++last_event);
            EXPECT_EQ(event.command_sequence, step);
            if (const auto* trade = std::get_if<apex::Trade>(&event.payload)) {
                EXPECT_GT(trade->quantity, 0U);
                EXPECT_NE(trade->buyer_id, trade->seller_id);
            }
        }
    }
}
