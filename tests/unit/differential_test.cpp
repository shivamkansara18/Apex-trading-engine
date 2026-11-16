#include "../reference/linear_matcher.hpp"
#include "apex/matching/matching_engine.hpp"
#include <gtest/gtest.h>
#include <random>
#include <sstream>
#include <type_traits>

namespace {
class IndependentMatcher : public testing::TestWithParam<unsigned> {};
std::string describe(const apex::Command& command) {
    std::ostringstream text;
    std::visit([&](const auto& request) {
        using T = std::decay_t<decltype(request)>;
        if constexpr (std::is_same_v<T,apex::NewOrder>) { text << "NEW " << request.client_order_id << ' ' << request.participant_id << ' ' << static_cast<unsigned>(request.side) << ' ' << static_cast<unsigned>(request.type) << ' ' << request.price_ticks << ' ' << request.quantity; }
        else if constexpr (std::is_same_v<T,apex::CancelOrder>) { text << "CANCEL " << request.client_order_id << ' ' << request.participant_id; }
        else { text << "REPLACE " << request.old_client_order_id << ' ' << request.new_client_order_id << ' ' << request.participant_id << ' ' << request.new_price_ticks << ' ' << request.new_quantity; }
    },command); return text.str();
}
apex::Command generate(std::mt19937& random,const apex::test_reference::LinearMatcher& model) {
    const auto kind = random() % 100;
    auto participant = static_cast<apex::ParticipantId>(1 + random() % 5);
    const auto id = static_cast<apex::OrderId>(random() % 128);
    const auto quantity = static_cast<apex::Quantity>(random() % 31);
    const auto price = random() % 20 == 0 ? 0 : static_cast<apex::Price>(95 + random() % 11);
    if (kind < 55) {
        auto side = random() % 2 == 0 ? apex::Side::Buy : apex::Side::Sell;
        auto type = random() % 3 == 0 ? apex::OrderType::Market : apex::OrderType::Limit;
        if (random() % 40 == 0) { side = static_cast<apex::Side>(2); }
        if (random() % 40 == 0) { type = static_cast<apex::OrderType>(2); }
        return apex::NewOrder{id,participant,side,type,price,quantity,0};
    }
    auto target = static_cast<apex::OrderId>(random() % 128);
    if (!model.live.empty() && random() % 100 < 75) {
        const auto& order = model.live[random() % model.live.size()]; target = order.client_order_id;
        if (random() % 100 < 80) { participant = order.participant_id; }
    }
    if (kind < 75) { return apex::CancelOrder{target,participant}; }
    return apex::ReplaceOrder{target,id,participant,price,quantity};
}
}
TEST_P(IndependentMatcher, MixedCommandsMatchLinearModelAfterEveryInput) {
    apex::test_reference::LinearMatcher model;
    apex::MatchingEngine normal;
    const auto seed = GetParam();
    apex::EngineOptions options{seed & 1 ? apex::PriceIndex::Flat : apex::PriceIndex::Tree,
        seed & 2 ? apex::RiskTraversal::Snapshot : apex::RiskTraversal::Direct,
        seed & 4 ? apex::EventReservation::SmallInitial : apex::EventReservation::WorstCase};
    apex::MatchingEngine alternative(options);
    for (apex::ParticipantId participant = 1; participant <= 4; ++participant) {
        ASSERT_TRUE(normal.register_participant(participant)); ASSERT_TRUE(alternative.register_participant(participant));
    }
    std::mt19937 random(seed);
    apex::EventSequence event_sequence = 0;
    std::size_t trades = 0, rejected = 0, self_cancels = 0;
    for (unsigned step = 0; step < 1000; ++step) {
        const auto command = generate(random,model);
        SCOPED_TRACE("seed=" + std::to_string(seed) + " step=" + std::to_string(step) + " " + describe(command));
        const auto before = normal.book().total_quantity(apex::Side::Buy) + normal.book().total_quantity(apex::Side::Sell);
        const auto expected = model.process(command);
        const auto events = normal.process(command);
        ASSERT_EQ(events,alternative.process(command)); // all intermediate events across policies
        std::optional<apex::RejectReason> reason;
        std::vector<apex::Trade> actual_trades;
        std::vector<apex::test_reference::Cancellation> cancellations;
        for (const auto& event : events) {
            ASSERT_EQ(event.command_sequence,step+1); ASSERT_EQ(event.event_sequence,++event_sequence);
            if (const auto* trade = std::get_if<apex::Trade>(&event.payload)) { actual_trades.push_back(*trade); }
            if (const auto* rejection = std::get_if<apex::CommandRejected>(&event.payload)) {
                ASSERT_FALSE(reason); reason = rejection->reason; ASSERT_EQ(rejection->command,command);
            }
            if (const auto* cancellation = std::get_if<apex::OrderCancelled>(&event.payload)) {
                cancellations.push_back({cancellation->order.client_order_id,cancellation->reason,cancellation->order.remaining_quantity});
                if (cancellation->reason == apex::CancelReason::SelfTradePrevention) { ++self_cancels; }
            }
        }
        ASSERT_EQ(reason,expected.rejected); ASSERT_EQ(actual_trades,expected.trades); ASSERT_EQ(cancellations,expected.cancellations);
        trades += actual_trades.size(); rejected += reason.has_value();
        const auto admitted = reason ? 0 : std::visit([](const auto& request) -> apex::Quantity {
            using T = std::decay_t<decltype(request)>;
            if constexpr (std::is_same_v<T,apex::NewOrder>) { return request.quantity; }
            else if constexpr (std::is_same_v<T,apex::ReplaceOrder>) { return request.new_quantity; }
            else { return 0; }
        },command);
        apex::Quantity removed = 0;
        for (const auto& trade : actual_trades) { removed += 2 * trade.quantity; }
        for (const auto& cancellation : cancellations) { removed += cancellation.remaining; }
        ASSERT_GE(before + admitted,removed);
        ASSERT_EQ(normal.book().total_quantity(apex::Side::Buy) + normal.book().total_quantity(apex::Side::Sell),before + admitted - removed);
        std::vector<apex::Order> actual;
        const auto snapshot = normal.snapshot(); ASSERT_EQ(snapshot.last_command_sequence,step+1);
        for (const auto* levels : {&snapshot.bids,&snapshot.asks}) {
            for (const auto& level : *levels) {
                apex::Quantity sum = 0;
                for (const auto& order : level.orders) { actual.push_back(order); sum += order.remaining_quantity; }
                ASSERT_EQ(level.total_quantity,sum);
            }
        }
        ASSERT_EQ(actual,model.ordered()); ASSERT_EQ(snapshot,alternative.snapshot());
        ASSERT_TRUE(normal.book().check_invariants(true)); ASSERT_TRUE(alternative.book().check_invariants(true));
        for (apex::ParticipantId participant = 1; participant <= 4; ++participant) {
            apex::RiskUsage usage;
            for (const auto& order : model.live) {
                if (order.participant_id != participant) { continue; }
                ++usage.open_orders; (order.side == apex::Side::Buy ? usage.buy_quantity : usage.sell_quantity) += order.remaining_quantity;
            }
            ASSERT_EQ(usage,normal.risk_usage(participant)); ASSERT_EQ(usage,alternative.risk_usage(participant));
        }
    }
    EXPECT_GT(trades,0U); EXPECT_GT(rejected,0U); EXPECT_GT(self_cancels,0U);
}
INSTANTIATE_TEST_SUITE_P(Seeds,IndependentMatcher,testing::Range(0U,32U),
    [](const testing::TestParamInfo<unsigned>& info){return "Seed"+std::to_string(info.param);});

TEST(LinearReference, HandCalculatedFifoReplacementAndSelfTradeScenario) {
    apex::test_reference::LinearMatcher model;
    const auto buy = [&](apex::OrderId id, apex::ParticipantId owner, apex::Quantity size) {
        return model.process(apex::NewOrder{id,owner,apex::Side::Buy,apex::OrderType::Limit,100,size,0});
    };
    EXPECT_FALSE(buy(1,1,10).rejected); EXPECT_FALSE(buy(2,2,10).rejected);
    EXPECT_FALSE(model.process(apex::ReplaceOrder{1,4,1,100,5}).rejected);
    const auto fill = model.process(apex::NewOrder{3,3,apex::Side::Sell,apex::OrderType::Market,0,12,0});
    ASSERT_EQ(fill.trades.size(),2U);
    EXPECT_EQ(fill.trades[0], (apex::Trade{1,2,3,2,3,2,100,10}));
    EXPECT_EQ(fill.trades[1], (apex::Trade{2,4,3,1,3,4,100,2}));
    const auto prevented = model.process(apex::NewOrder{5,1,apex::Side::Sell,apex::OrderType::Market,0,2,0});
    EXPECT_TRUE(prevented.trades.empty());
    EXPECT_EQ(prevented.cancellations,(std::vector<apex::test_reference::Cancellation>{{5,apex::CancelReason::SelfTradePrevention,2}}));
    ASSERT_EQ(model.live.size(),1U); EXPECT_EQ(model.live.front().remaining_quantity,3U);
    const auto remainder = model.process(apex::NewOrder{6,4,apex::Side::Sell,apex::OrderType::Market,0,5,0});
    ASSERT_EQ(remainder.trades.size(),1U); EXPECT_EQ(remainder.trades.front().quantity,3U);
    EXPECT_EQ(remainder.cancellations,(std::vector<apex::test_reference::Cancellation>{{6,apex::CancelReason::MarketRemainder,2}}));
    EXPECT_TRUE(model.live.empty());
}
