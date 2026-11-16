#include "apex/book/snapshot.hpp"
#include "apex/core/event.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <type_traits>

static_assert(sizeof(apex::Price) == 8 && std::is_signed_v<apex::Price>);
static_assert(sizeof(apex::Quantity) == 8 && std::is_unsigned_v<apex::Quantity>);
static_assert(sizeof(apex::OrderId) == 8 && sizeof(apex::ParticipantId) == 4);
static_assert(sizeof(apex::CommandSequence) == 8 && sizeof(apex::EventSequence) == 8);
static_assert(apex::command_type(apex::NewOrder{}) == apex::CommandType::NewOrder);
static_assert(apex::command_type(apex::CancelOrder{}) == apex::CommandType::CancelOrder);
static_assert(apex::command_type(apex::ReplaceOrder{}) == apex::CommandType::ReplaceOrder);

TEST(Domain, CommandsPreserveFullWidthIntegerValues) {
    const apex::NewOrder request{
        .client_order_id = std::numeric_limits<apex::OrderId>::max(),
        .participant_id = std::numeric_limits<apex::ParticipantId>::max(),
        .side = apex::Side::Sell,
        .type = apex::OrderType::Market,
        .price_ticks = std::numeric_limits<apex::Price>::min(),
        .quantity = std::numeric_limits<apex::Quantity>::max(),
        .client_timestamp_ns = std::numeric_limits<apex::TimestampNs>::max()};
    const apex::Command command = request;
    EXPECT_EQ(std::get<apex::NewOrder>(command), request);
    // Transport/domain representation preserves input; validation comes later.
}

TEST(Domain, RejectedReplacementRetainsBothOrderIdsAndReason) {
    const apex::ReplaceOrder request{41, 42, 7, 10'125, 30};
    const apex::Event event{9, 6, apex::CommandRejected{
        request, apex::RejectReason::DuplicateOrderId}};
    const auto& rejection = std::get<apex::CommandRejected>(event.payload);
    EXPECT_EQ(std::get<apex::ReplaceOrder>(rejection.command), request);
    EXPECT_EQ(rejection.reason, apex::RejectReason::DuplicateOrderId);
    EXPECT_EQ(event.command_sequence, 6U);
}

TEST(Domain, TradeAndUpdatesCanShareOneCausalCommand) {
    const apex::Trade trade{1, 10, 20, 1, 2, 10, 10'000, 20};
    const apex::Order order{10, 1, apex::Side::Buy, apex::OrderType::Limit,
                            10'000, 50, 30, 1, apex::OrderStatus::PartiallyFilled};
    const apex::Event execution{3, 2, trade};
    const apex::Event update{4, 2, apex::OrderUpdated{order}};
    EXPECT_EQ(execution.command_sequence, update.command_sequence);
    EXPECT_NE(execution.event_sequence, update.event_sequence);
    EXPECT_EQ(std::get<apex::Trade>(execution.payload).quantity, 20U);
    EXPECT_EQ(std::get<apex::OrderUpdated>(update.payload).order.remaining_quantity, 30U);
}

TEST(Domain, SnapshotCopyOwnsItsOrdersAndPreservesFifo) {
    const apex::Order first{1, 1, apex::Side::Buy, apex::OrderType::Limit,
                            10'000, 100, 100, 1, apex::OrderStatus::Resting};
    const apex::Order second{2, 2, apex::Side::Buy, apex::OrderType::Limit,
                             10'000, 50, 50, 2, apex::OrderStatus::Resting};
    apex::BookSnapshot original{2, {{10'000, 150, {first, second}}}, {}};
    const auto copy = original;
    EXPECT_EQ(copy, original);
    original.bids.front().orders.front().remaining_quantity = 1;
    EXPECT_EQ(copy.bids.front().orders.front(), first);
    EXPECT_EQ(copy.bids.front().orders.back(), second);
    EXPECT_TRUE(copy.asks.empty());
    EXPECT_NE(copy, original);
}

TEST(Domain, CancellationPreservesUnexecutedQuantityAndReason) {
    const apex::Order order{3, 2, apex::Side::Sell, apex::OrderType::Market,
                            0, 100, 40, 5, apex::OrderStatus::Cancelled};
    const apex::Event event{8, 5, apex::OrderCancelled{
        order, apex::CancelReason::MarketRemainder}};
    const auto& cancellation = std::get<apex::OrderCancelled>(event.payload);
    EXPECT_EQ(cancellation.order.remaining_quantity, 40U);
    EXPECT_EQ(cancellation.reason, apex::CancelReason::MarketRemainder);
}
