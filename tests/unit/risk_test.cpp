#include "apex/matching/matching_engine.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <random>
#include <stdexcept>

namespace {
apex::NewOrder buy(apex::OrderId id, apex::Quantity quantity = 10, apex::Price price = 100,
                    apex::ParticipantId participant = 1) {
    return {id, participant, apex::Side::Buy, apex::OrderType::Limit, price, quantity, 0};
}
apex::NewOrder sell(apex::OrderId id, apex::Quantity quantity = 10, apex::Price price = 101,
                     apex::ParticipantId participant = 2) {
    return {id, participant, apex::Side::Sell, apex::OrderType::Limit, price, quantity, 0};
}
void accepted(apex::MatchingEngine& engine, const apex::Command& command) {
    const auto events = engine.process(command);
    for (const auto& event : events) {
        ASSERT_FALSE(std::holds_alternative<apex::CommandRejected>(event.payload));
    }
    ASSERT_TRUE(engine.book().check_invariants(true));
}
void rejected(apex::MatchingEngine& engine, const apex::Command& command, apex::RejectReason reason) {
    const auto before = engine.snapshot();
    const auto usage1 = engine.risk_usage(1);
    const auto usage2 = engine.risk_usage(2);
    const auto events = engine.process(command);
    ASSERT_EQ(events.size(), 1U);
    const auto* rejection = std::get_if<apex::CommandRejected>(&events[0].payload);
    ASSERT_NE(rejection, nullptr);
    EXPECT_EQ(rejection->reason, reason);
    EXPECT_EQ(rejection->command, command);
    EXPECT_EQ(engine.snapshot().bids, before.bids);
    EXPECT_EQ(engine.snapshot().asks, before.asks);
    EXPECT_EQ(engine.risk_usage(1), usage1);
    EXPECT_EQ(engine.risk_usage(2), usage2);
    EXPECT_EQ(events[0].command_sequence, before.last_command_sequence + 1);
    EXPECT_TRUE(engine.book().check_invariants(true));
}
}

TEST(Risk, RegistrationIsRequiredAndDuplicateRegistrationKeepsLimits) {
    apex::MatchingEngine engine;
    rejected(engine, buy(1), apex::RejectReason::UnknownParticipant);
    rejected(engine, apex::CancelOrder{1, 1}, apex::RejectReason::UnknownParticipant);
    rejected(engine, apex::ReplaceOrder{1, 2, 1, 100, 1}, apex::RejectReason::UnknownParticipant);
    apex::RiskLimits limits;
    limits.max_order_quantity = 10;
    ASSERT_TRUE(engine.register_participant(1, limits));
    EXPECT_FALSE(engine.register_participant(1));
    EXPECT_EQ(engine.risk().limits(1), limits);
    EXPECT_FALSE(engine.configure_participant(2, limits));
    accepted(engine, buy(1));
}

TEST(Risk, InvalidConfigurationThrowsAndPreservesConfiguration) {
    apex::MatchingEngine engine;
    ASSERT_TRUE(engine.register_participant(1));
    const auto before = engine.risk().limits(1);
    apex::RiskLimits limits;
    limits.min_price_ticks = 0;
    EXPECT_THROW((void)engine.configure_participant(1, limits), std::invalid_argument);
    EXPECT_THROW((void)engine.register_participant(2, limits), std::invalid_argument);
    limits.min_price_ticks = 200;
    limits.max_price_ticks = 100;
    EXPECT_THROW((void)engine.configure_participant(1, limits), std::invalid_argument);
    EXPECT_EQ(engine.risk().limits(1), before);
    EXPECT_FALSE(engine.risk().limits(2));
}

TEST(Risk, QuantityLimitIncludesMarketAndReplace) {
    apex::MatchingEngine engine;
    apex::RiskLimits limits;
    limits.max_order_quantity = 10;
    ASSERT_TRUE(engine.register_participant(1, limits));
    accepted(engine, buy(1));
    rejected(engine, buy(2, 11), apex::RejectReason::QuantityLimit);
    auto market = buy(2, 11);
    market.type = apex::OrderType::Market;
    rejected(engine, market, apex::RejectReason::QuantityLimit);
    rejected(engine, apex::ReplaceOrder{1, 3, 1, 100, 11}, apex::RejectReason::QuantityLimit);
    accepted(engine, apex::ReplaceOrder{1, 3, 1, 100, 10});
}

TEST(Risk, MalformedRequestsAndOwnershipFailuresHaveSpecificReasons) {
    apex::MatchingEngine engine;
    ASSERT_TRUE(engine.register_participant(1));
    ASSERT_TRUE(engine.register_participant(2));
    accepted(engine, buy(1));
    auto invalid = buy(2);
    invalid.side = static_cast<apex::Side>(255);
    rejected(engine, invalid, apex::RejectReason::InvalidSide);
    invalid = buy(2);
    invalid.type = static_cast<apex::OrderType>(255);
    rejected(engine, invalid, apex::RejectReason::InvalidOrderType);
    rejected(engine, buy(2, 0), apex::RejectReason::ZeroQuantity);
    rejected(engine, buy(2, 1, 0), apex::RejectReason::InvalidPrice);
    rejected(engine, buy(1), apex::RejectReason::DuplicateOrderId);
    rejected(engine, apex::CancelOrder{99, 1}, apex::RejectReason::UnknownOrder);
    rejected(engine, apex::CancelOrder{1, 2}, apex::RejectReason::WrongParticipant);
    rejected(engine, apex::ReplaceOrder{1, 2, 2, 100, 1}, apex::RejectReason::WrongParticipant);
    rejected(engine, apex::ReplaceOrder{1, 1, 1, 100, 1}, apex::RejectReason::InvalidReplacement);
}

TEST(Risk, LimitNotionalBoundaryAndOverflowAreCheckedBeforeMultiplication) {
    apex::MatchingEngine engine;
    apex::RiskLimits limits;
    limits.max_order_notional = 1000;
    ASSERT_TRUE(engine.register_participant(1, limits));
    accepted(engine, buy(1, 10, 100));
    rejected(engine, buy(2, 11, 100), apex::RejectReason::NotionalLimit);
    rejected(engine, apex::ReplaceOrder{1, 3, 1, 101, 10}, apex::RejectReason::NotionalLimit);
    ASSERT_TRUE(engine.configure_participant(1, {}));
    accepted(engine, apex::CancelOrder{1, 1});
    rejected(engine, buy(2, std::numeric_limits<apex::Quantity>::max(), 2), apex::RejectReason::ArithmeticOverflow);
    accepted(engine, buy(3, std::numeric_limits<apex::Quantity>::max(), 1));
}

TEST(Risk, PriceBandsAreInclusiveAndReplaceRejectsWithoutCancelling) {
    apex::MatchingEngine engine;
    apex::RiskLimits limits;
    limits.min_price_ticks = 90;
    limits.max_price_ticks = 110;
    ASSERT_TRUE(engine.register_participant(1, limits));
    accepted(engine, buy(1, 10, 90));
    accepted(engine, buy(2, 10, 110));
    rejected(engine, buy(3, 10, 89), apex::RejectReason::PriceBand);
    rejected(engine, buy(3, 10, 111), apex::RejectReason::PriceBand);
    rejected(engine, apex::ReplaceOrder{1, 3, 1, 111, 1}, apex::RejectReason::PriceBand);
}

TEST(Risk, OpenOrderLimitCreditsReplacementAndIsReleasedByCancelAndFill) {
    apex::MatchingEngine engine;
    apex::RiskLimits limits;
    limits.max_open_orders = 1;
    ASSERT_TRUE(engine.register_participant(1, limits));
    ASSERT_TRUE(engine.register_participant(2));
    accepted(engine, buy(1));
    rejected(engine, buy(2), apex::RejectReason::OpenOrderLimit);
    accepted(engine, apex::ReplaceOrder{1, 3, 1, 100, 10});
    EXPECT_EQ(engine.risk_usage(1), (apex::RiskUsage{1, 10, 0}));
    accepted(engine, sell(4, 4, 100));
    EXPECT_EQ(engine.risk_usage(1), (apex::RiskUsage{1, 6, 0}));
    rejected(engine, buy(5), apex::RejectReason::OpenOrderLimit);
    accepted(engine, sell(6, 6, 100));
    EXPECT_EQ(engine.risk_usage(1), (apex::RiskUsage{}));
    accepted(engine, buy(7));
    accepted(engine, apex::CancelOrder{7, 1});
    accepted(engine, buy(8));
}

TEST(Risk, UsageIncludesBothSidesAndMarketDoesNotReserveOpenOrderSlot) {
    apex::MatchingEngine engine;
    apex::RiskLimits limits;
    limits.max_open_orders = 2;
    ASSERT_TRUE(engine.register_participant(1, limits));
    ASSERT_TRUE(engine.register_participant(2));
    accepted(engine, buy(1));
    accepted(engine, sell(2, 5, 102, 1));
    EXPECT_EQ(engine.risk_usage(1), (apex::RiskUsage{2, 10, 5}));
    rejected(engine, sell(3, 1, 103, 1), apex::RejectReason::OpenOrderLimit);
    accepted(engine, sell(4, 2, 101));
    auto market = buy(5, 3);
    market.type = apex::OrderType::Market;
    accepted(engine, market); // Fills other maker, then stops at own ask.
    EXPECT_EQ(engine.risk_usage(1), (apex::RiskUsage{2, 10, 5}));
    EXPECT_EQ(engine.risk_usage(2), (apex::RiskUsage{}));
}

TEST(Risk, KillSwitchesPreserveRestingOrdersAndAllowCancel) {
    apex::MatchingEngine engine;
    ASSERT_TRUE(engine.register_participant(1));
    ASSERT_TRUE(engine.register_participant(2));
    accepted(engine, buy(1));
    engine.set_trading_enabled(false);
    rejected(engine, buy(2), apex::RejectReason::TradingDisabled);
    auto market = buy(2);
    market.type = apex::OrderType::Market;
    rejected(engine, market, apex::RejectReason::TradingDisabled);
    rejected(engine, apex::ReplaceOrder{1, 3, 1, 100, 10}, apex::RejectReason::TradingDisabled);
    accepted(engine, apex::CancelOrder{1, 1});
    engine.set_trading_enabled(true);
    accepted(engine, buy(4));
    auto limits = *engine.risk().limits(1);
    limits.trading_enabled = false;
    ASSERT_TRUE(engine.configure_participant(1, limits));
    rejected(engine, buy(5), apex::RejectReason::TradingDisabled);
    rejected(engine, market, apex::RejectReason::TradingDisabled);
    rejected(engine, apex::ReplaceOrder{4, 5, 1, 101, 2}, apex::RejectReason::TradingDisabled);
    accepted(engine, sell(6, 2, 100)); // Existing maker remains executable.
    EXPECT_EQ(engine.book().find(4)->remaining_quantity, 8U);
    accepted(engine, apex::CancelOrder{4, 1});
    limits.trading_enabled = true;
    ASSERT_TRUE(engine.configure_participant(1, limits));
    accepted(engine, buy(7));
}

TEST(Risk, ZeroLimitsDenyAdmissionAndUpdatesDoNotCancelExistingOrders) {
    apex::MatchingEngine engine;
    ASSERT_TRUE(engine.register_participant(1));
    accepted(engine, buy(1));
    auto limits = *engine.risk().limits(1);
    limits.max_open_orders = 0;
    ASSERT_TRUE(engine.configure_participant(1, limits));
    rejected(engine, buy(2), apex::RejectReason::OpenOrderLimit);
    rejected(engine, apex::ReplaceOrder{1, 2, 1, 100, 1}, apex::RejectReason::OpenOrderLimit);
    EXPECT_TRUE(engine.book().find(1));
    limits.max_order_quantity = 0;
    ASSERT_TRUE(engine.configure_participant(1, limits));
    rejected(engine, buy(2, 1), apex::RejectReason::QuantityLimit);
    accepted(engine, apex::CancelOrder{1, 1});
}

TEST(Risk, MarketSweepNotionalRejectsAtomicallyAcrossLevels) {
    for (const auto side : {apex::Side::Buy, apex::Side::Sell}) {
        apex::MatchingEngine engine;
        apex::RiskLimits limits;
        limits.max_order_notional = 1000;
        ASSERT_TRUE(engine.register_participant(1, limits));
        ASSERT_TRUE(engine.register_participant(2));
        const auto opposite = side == apex::Side::Buy ? apex::Side::Sell : apex::Side::Buy;
        accepted(engine, apex::NewOrder{1, 2, opposite, apex::OrderType::Limit, 100, 5, 0});
        accepted(engine, apex::NewOrder{2, 2, opposite, apex::OrderType::Limit, 101, 5, 0});
        apex::NewOrder market{3, 1, side, apex::OrderType::Market, -999, 10, 0};
        rejected(engine, market, apex::RejectReason::NotionalLimit); // 1005 total, not just first level.
        market.quantity = 9;
        accepted(engine, market);
        EXPECT_EQ(engine.book().total_quantity(opposite), 1U);
    }
}

TEST(Risk, MarketUsesOnlyExecutableQuantityAndStopsRiskSweepAtSelfTrade) {
    apex::MatchingEngine engine;
    apex::RiskLimits limits;
    limits.max_order_notional = 500;
    ASSERT_TRUE(engine.register_participant(1, limits));
    ASSERT_TRUE(engine.register_participant(2));
    accepted(engine, sell(1, 5, 100));
    auto market = buy(2, 100);
    market.type = apex::OrderType::Market;
    accepted(engine, market); // Only 5 executable, so notional is exactly 500.
    EXPECT_EQ(engine.risk_usage(1).open_orders, 0U);
    accepted(engine, sell(3, 1, 101, 1));
    accepted(engine, sell(4, 10, 102));
    accepted(engine, market); // Own ask stops before expensive worse-priced liquidity.
    EXPECT_TRUE(engine.book().find(3));
    EXPECT_TRUE(engine.book().find(4));
}

TEST(Risk, MarketBandFailureDoesNotApplyEarlierEligibleFills) {
    apex::MatchingEngine engine;
    apex::RiskLimits limits;
    limits.min_price_ticks = 90;
    limits.max_price_ticks = 100;
    ASSERT_TRUE(engine.register_participant(1, limits));
    ASSERT_TRUE(engine.register_participant(2));
    accepted(engine, sell(1, 5, 100));
    accepted(engine, sell(2, 5, 101));
    auto market = buy(3, 6);
    market.type = apex::OrderType::Market;
    rejected(engine, market, apex::RejectReason::PriceBand);
    market.quantity = 5;
    accepted(engine, market); // Does not inspect the unreachable out-of-band level.
}

TEST(Risk, MarketCumulativeNotionalOverflowRejectsBeforeAnyTrade) {
    apex::MatchingEngine engine;
    ASSERT_TRUE(engine.register_participant(1));
    ASSERT_TRUE(engine.register_participant(2));
    const auto quantity = std::numeric_limits<apex::Quantity>::max() / 2;
    accepted(engine, sell(1, quantity, 2));
    accepted(engine, sell(2, 1, 3));
    auto market = buy(3, quantity + 1);
    market.type = apex::OrderType::Market;
    rejected(engine, market, apex::RejectReason::ArithmeticOverflow);
}

TEST(Risk, SeededCommandsPreserveUsageAndBookOnEveryRejection) {
    apex::MatchingEngine engine;
    apex::RiskLimits limits;
    limits.max_order_quantity = 20;
    limits.max_order_notional = 1500;
    limits.max_open_orders = 3;
    limits.min_price_ticks = 95;
    limits.max_price_ticks = 105;
    ASSERT_TRUE(engine.register_participant(1, limits));
    ASSERT_TRUE(engine.register_participant(2, limits));
    std::mt19937 random(456);
    for (apex::OrderId step = 1; step <= 1200; ++step) {
        apex::Command command = apex::NewOrder{step, 1 + random() % 3,
            random() % 2 == 0 ? apex::Side::Buy : apex::Side::Sell,
            random() % 4 == 0 ? apex::OrderType::Market : apex::OrderType::Limit,
            90 + random() % 20, random() % 30, 0};
        if (random() % 3 == 0) { command = apex::CancelOrder{random() % step, 1 + random() % 3}; }
        else if (random() % 4 == 0) {
            command = apex::ReplaceOrder{random() % step, step, 1 + random() % 3,
                                        90 + random() % 20, random() % 30};
        }
        const auto before = engine.snapshot();
        const auto usage1 = engine.risk_usage(1);
        const auto usage2 = engine.risk_usage(2);
        const auto events = engine.process(command);
        if (std::holds_alternative<apex::CommandRejected>(events.front().payload)) {
            EXPECT_EQ(events.size(), 1U);
            EXPECT_EQ(engine.snapshot().bids, before.bids);
            EXPECT_EQ(engine.snapshot().asks, before.asks);
            EXPECT_EQ(engine.risk_usage(1), usage1);
            EXPECT_EQ(engine.risk_usage(2), usage2);
        }
        EXPECT_TRUE(engine.book().check_invariants(true));
        // Independent lookup-by-ID accounting, rather than snapshot aggregation.
        for (apex::ParticipantId participant = 1; participant <= 2; ++participant) {
            apex::RiskUsage expected;
            for (apex::OrderId id = 0; id <= step; ++id) {
                const auto order = engine.book().find(id);
                if (!order || order->participant_id != participant) { continue; }
                ++expected.open_orders;
                (order->side == apex::Side::Buy ? expected.buy_quantity : expected.sell_quantity)
                    += order->remaining_quantity;
            }
            EXPECT_EQ(engine.risk_usage(participant), expected);
            EXPECT_LE(expected.open_orders, 3U);
        }
    }
}
