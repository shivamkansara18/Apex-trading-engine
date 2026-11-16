#include "apex/book/order_book.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <random>
#include <unordered_map>

namespace {
apex::Order order(apex::OrderId id, apex::Side side = apex::Side::Buy,
                  apex::Price price = 100, apex::Quantity quantity = 10) {
    return {id, 1, side, apex::OrderType::Limit, price, quantity, quantity,
            id + 1, apex::OrderStatus::Resting};
}
}

TEST(OrderBook, EmptyQueries) {
    const apex::OrderBook book;
    EXPECT_FALSE(book.best_bid());
    EXPECT_FALSE(book.best_ask());
    EXPECT_FALSE(book.find(1));
    EXPECT_EQ(book.depth(apex::Side::Buy, 100), 0U);
    EXPECT_EQ(book.total_quantity(apex::Side::Sell), 0U);
    EXPECT_EQ(book.order_count(), 0U);
    EXPECT_TRUE(book.check_invariants(true));
}

TEST(OrderBook, FillsKeepPriorityAndRemoveCompletedOrders) {
    apex::OrderBook book;
    ASSERT_EQ(book.insert(order(1)), apex::BookError::None);
    ASSERT_EQ(book.insert(order(2)), apex::BookError::None);
    const auto partial = book.fill(1, 4);
    ASSERT_TRUE(partial);
    EXPECT_EQ(partial->remaining_quantity, 6U);
    EXPECT_EQ(partial->priority_sequence, 2U);
    EXPECT_EQ(partial->status, apex::OrderStatus::PartiallyFilled);
    EXPECT_EQ(book.best_order(apex::Side::Buy)->client_order_id, 1U);
    EXPECT_EQ(book.depth(apex::Side::Buy, 100), 16U);
    const auto before = book.snapshot();
    EXPECT_FALSE(book.fill(1, 0));
    EXPECT_FALSE(book.fill(1, 7));
    EXPECT_FALSE(book.fill(99, 1));
    EXPECT_EQ(book.snapshot(), before);
    const auto complete = book.fill(1, 6);
    ASSERT_TRUE(complete);
    EXPECT_EQ(complete->status, apex::OrderStatus::Filled);
    EXPECT_FALSE(book.find(1));
    EXPECT_EQ(book.best_order(apex::Side::Buy)->client_order_id, 2U);
    EXPECT_TRUE(book.fill(2, 10));
    EXPECT_FALSE(book.best_order(apex::Side::Buy));
    EXPECT_TRUE(book.check_invariants(true));
}

TEST(OrderBook, OrdersPricesAndPreservesFifo) {
    apex::OrderBook book;
    for (const auto& request : {order(1), order(2), order(3, apex::Side::Buy, 99),
             order(4, apex::Side::Sell, 103), order(5, apex::Side::Sell, 102)}) {
        ASSERT_EQ(book.insert(request), apex::BookError::None);
        ASSERT_TRUE(book.check_invariants(true));
    }
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), 102);
    EXPECT_EQ(book.depth(apex::Side::Buy, 100), 20U);
    EXPECT_EQ(book.total_quantity(apex::Side::Buy), 30U);
    EXPECT_EQ(book.total_quantity(apex::Side::Sell), 20U);
    const auto snapshot = book.snapshot(9);
    ASSERT_EQ(snapshot.bids.size(), 2U);
    ASSERT_EQ(snapshot.asks.size(), 2U);
    EXPECT_EQ(snapshot.last_command_sequence, 9U);
    EXPECT_EQ(snapshot.bids[0].orders[0].client_order_id, 1U);
    EXPECT_EQ(snapshot.bids[0].orders[1].client_order_id, 2U);
    EXPECT_EQ(snapshot.bids[1].price_ticks, 99);
    EXPECT_EQ(snapshot.asks[0].price_ticks, 102);
    EXPECT_EQ(book.top_levels(apex::Side::Buy, 1).size(), 1U);
    EXPECT_TRUE(book.top_levels(apex::Side::Sell, 0).empty());
}

TEST(OrderBook, CancelsMiddleThenRemovesEmptyLevels) {
    apex::OrderBook book;
    for (apex::OrderId id = 1; id <= 3; ++id) {
        ASSERT_EQ(book.insert(order(id)), apex::BookError::None);
    }
    ASSERT_EQ(book.cancel(2, 1), apex::BookError::None);
    EXPECT_FALSE(book.find(2));
    EXPECT_EQ(book.depth(apex::Side::Buy, 100), 20U);
    EXPECT_EQ(book.top_levels(apex::Side::Buy, 1)[0].orders.back().client_order_id, 3U);
    ASSERT_TRUE(book.check_invariants());
    ASSERT_EQ(book.cancel(1, 1), apex::BookError::None);
    ASSERT_TRUE(book.check_invariants());
    ASSERT_EQ(book.cancel(3, 1), apex::BookError::None);
    EXPECT_FALSE(book.best_bid());
    EXPECT_EQ(book.total_quantity(apex::Side::Buy), 0U);
    EXPECT_TRUE(book.check_invariants());
    // Terminal IDs can be reused after cancellation.
    EXPECT_EQ(book.insert(order(2, apex::Side::Sell, 102)), apex::BookError::None);
    EXPECT_EQ(book.cancel(2, 1), apex::BookError::None);
    EXPECT_FALSE(book.best_ask());
    EXPECT_TRUE(book.check_invariants());
}

TEST(OrderBook, FailedOperationsLeaveSnapshotUnchanged) {
    apex::OrderBook book;
    ASSERT_EQ(book.insert(order(1)), apex::BookError::None);
    const auto before = book.snapshot();
    EXPECT_EQ(book.insert(order(1, apex::Side::Sell, 101)), apex::BookError::DuplicateOrder);
    EXPECT_EQ(book.cancel(1, 99), apex::BookError::WrongParticipant);
    EXPECT_EQ(book.cancel(99, 1), apex::BookError::UnknownOrder);
    auto invalid = order(2);
    invalid.type = apex::OrderType::Market;
    EXPECT_EQ(book.insert(invalid), apex::BookError::InvalidOrder);
    invalid = order(2);
    invalid.priority_sequence = 1;
    EXPECT_EQ(book.insert(invalid), apex::BookError::InvalidOrder);
    invalid = order(2);
    invalid.remaining_quantity = 0;
    EXPECT_EQ(book.insert(invalid), apex::BookError::InvalidOrder);
    invalid = order(2);
    invalid.price_ticks = -1;
    EXPECT_EQ(book.insert(invalid), apex::BookError::InvalidOrder);
    invalid = order(2);
    invalid.side = static_cast<apex::Side>(255);
    EXPECT_EQ(book.insert(invalid), apex::BookError::InvalidOrder);
    invalid = order(2);
    invalid.remaining_quantity = invalid.original_quantity + 1;
    EXPECT_EQ(book.insert(invalid), apex::BookError::InvalidOrder);
    invalid = order(2);
    invalid.status = apex::OrderStatus::Filled;
    EXPECT_EQ(book.insert(invalid), apex::BookError::InvalidOrder);
    EXPECT_EQ(book.snapshot(), before);
    EXPECT_TRUE(book.check_invariants());
}

TEST(OrderBook, OverflowIsRejectedAcrossPriceLevels) {
    apex::OrderBook book;
    ASSERT_EQ(book.insert(order(1, apex::Side::Buy, 100,
        std::numeric_limits<apex::Quantity>::max())), apex::BookError::None);
    const auto before = book.snapshot();
    EXPECT_EQ(book.insert(order(2, apex::Side::Buy, 99, 1)), apex::BookError::QuantityOverflow);
    EXPECT_EQ(book.snapshot(), before);
    EXPECT_TRUE(book.check_invariants());
}

TEST(OrderBook, SnapshotAndLookupCannotMutateStorage) {
    apex::OrderBook book;
    ASSERT_EQ(book.insert(order(1)), apex::BookError::None);
    auto snapshot = book.snapshot();
    auto found = book.find(1);
    ASSERT_TRUE(found);
    found->remaining_quantity = 1;
    snapshot.bids[0].orders.clear();
    EXPECT_EQ(book.find(1)->remaining_quantity, 10U);
    EXPECT_TRUE(book.check_invariants());
}

TEST(OrderBook, DetectsCrossedStorageWhenRequested) {
    apex::OrderBook book;
    ASSERT_EQ(book.insert(order(1, apex::Side::Buy, 100)), apex::BookError::None);
    ASSERT_EQ(book.insert(order(2, apex::Side::Sell, 100)), apex::BookError::None);
    EXPECT_TRUE(book.check_invariants());
    EXPECT_FALSE(book.check_invariants(true));
}

TEST(OrderBook, SeededInsertCancelStreamAgreesWithIndependentTotals) {
    apex::OrderBook book;
    std::mt19937 rng(42);
    std::unordered_map<apex::OrderId, apex::Order> reference;
    for (apex::OrderId step = 1; step <= 2000; ++step) {
        if (rng() % 3 == 0) {
            const apex::OrderId id = rng() % step;
            const bool exists = reference.contains(id);
            EXPECT_EQ(book.cancel(id, 1), exists ? apex::BookError::None : apex::BookError::UnknownOrder);
            reference.erase(id);
        } else {
            const auto side = rng() % 2 == 0 ? apex::Side::Buy : apex::Side::Sell;
            const apex::Price price = (side == apex::Side::Buy ? 90 : 110) + rng() % 10;
            const auto request = order(step, side, price, 1 + rng() % 100);
            ASSERT_EQ(book.insert(request), apex::BookError::None);
            reference.emplace(step, request);
        }
        ASSERT_TRUE(book.check_invariants(true)) << "step=" << step;
        apex::Quantity buy = 0;
        apex::Quantity sell = 0;
        for (const auto& [id, request] : reference) {
            ASSERT_EQ(book.find(id), request);
            (request.side == apex::Side::Buy ? buy : sell) += request.remaining_quantity;
        }
        EXPECT_EQ(book.total_quantity(apex::Side::Buy), buy);
        EXPECT_EQ(book.total_quantity(apex::Side::Sell), sell);
        EXPECT_EQ(book.order_count(), reference.size());
    }
}
