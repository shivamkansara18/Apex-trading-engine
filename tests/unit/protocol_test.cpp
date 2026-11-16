#include "apex/network/protocol.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace net = apex::network;

TEST(Protocol, HeaderIsVersionedBigEndianAndAllSplitPointsDecode) {
    const auto request = net::hello(0x0102030405060708ULL, 0x11223344);
    const auto bytes = net::encode_frame(request);
    EXPECT_EQ(bytes, (net::Bytes{0,0,0,20, 0,1, 0,1, 1,2,3,4,5,6,7,8, 0x11,0x22,0x33,0x44}));
    for (std::size_t split = 0; split <= bytes.size(); ++split) {
        net::FrameDecoder decoder;
        auto first = decoder.append(std::span(bytes).first(split));
        auto second = decoder.append(std::span(bytes).subspan(split));
        first.insert(first.end(), second.begin(), second.end());
        EXPECT_EQ(first, (std::vector<net::Frame>{request}));
        EXPECT_FALSE(decoder.partial());
    }
    net::FrameDecoder bytewise;
    std::vector<net::Frame> frames;
    for (const auto byte : bytes) {
        const auto next = bytewise.append(std::span(&byte, 1));
        frames.insert(frames.end(), next.begin(), next.end());
    }
    EXPECT_EQ(frames, (std::vector<net::Frame>{request}));
}

TEST(Protocol, CoalescedFramesAndMaximumPayloadStayFramed) {
    const net::Frame empty{net::MessageType::Heartbeat, 2, {}};
    const net::Frame large{net::MessageType::Error, 3, net::Bytes(net::max_frame_size - net::header_size, 42)};
    auto bytes = net::encode_frame(net::hello(1, 1));
    for (const auto& frame : {empty, large, empty}) {
        const auto encoded = net::encode_frame(frame); bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    }
    net::FrameDecoder decoder;
    EXPECT_EQ(decoder.append(bytes), (std::vector<net::Frame>{net::hello(1, 1), empty, large, empty}));
    EXPECT_FALSE(decoder.partial());
    EXPECT_THROW((void)net::encode_frame({net::MessageType::Error, 1, net::Bytes(net::max_frame_size, 0)}), std::length_error);
}

TEST(Protocol, InvalidLengthsVersionsAndTrailingPayloadFail) {
    const auto good = net::encode_frame(net::hello(1, 1));
    for (const auto size : {0U, 15U, 65'537U, std::numeric_limits<std::uint32_t>::max()}) {
        auto bad = good;
        for (int i = 0; i < 4; ++i) { bad[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(size >> (24 - 8 * i)); }
        net::FrameDecoder decoder;
        EXPECT_THROW((void)decoder.append(bad), std::runtime_error);
    }
    auto bad = good; bad[5] = 2;
    net::FrameDecoder decoder;
    EXPECT_THROW((void)decoder.append(bad), std::runtime_error);
    EXPECT_THROW((void)net::decode_participant({}), std::runtime_error);
    EXPECT_THROW((void)net::decode_participant(net::Bytes(5, 0)), std::runtime_error);
    net::FrameDecoder partial;
    EXPECT_TRUE(partial.append(std::span(good).first(7)).empty());
    EXPECT_TRUE(partial.partial());
}

TEST(Protocol, AllCommandsRoundTripWithoutNativeStructLayout) {
    const std::vector<apex::Command> commands{
        apex::NewOrder{1, 2, apex::Side::Sell, apex::OrderType::Market, -1, 100, 999},
        apex::CancelOrder{1, 2}, apex::ReplaceOrder{1, 3, 2, 100, 50},
        apex::NewOrder{4, 2, static_cast<apex::Side>(255), static_cast<apex::OrderType>(254), 0, 0, 0}};
    for (const auto& command : commands) {
        auto frame = net::command_frame(7, command);
        EXPECT_EQ(net::decode_command(frame), command);
        frame.payload.push_back(0);
        EXPECT_THROW((void)net::decode_command(frame), std::runtime_error);
    }
    EXPECT_THROW((void)net::decode_command(net::hello(1, 1)), std::runtime_error);
}

TEST(Protocol, EveryEventPayloadRoundTripsAndMalformedReportsFail) {
    const apex::Order order{1, 2, apex::Side::Buy, apex::OrderType::Limit, 100, 10, 5, 3, apex::OrderStatus::PartiallyFilled};
    const std::vector<apex::EventPayload> payloads{
        apex::OrderAccepted{order}, apex::OrderUpdated{order}, apex::OrderCancelled{order, apex::CancelReason::SelfTradePrevention},
        apex::CommandRejected{apex::ReplaceOrder{1, 2, 3, 4, 5}, apex::RejectReason::WrongParticipant},
        apex::Trade{1, 2, 3, 4, 5, 2, 100, 10}};
    for (const auto& payload : payloads) {
        const apex::Event event{10, 8, payload};
        const auto bytes = net::encode_event(event);
        EXPECT_EQ(net::decode_event(bytes), event);
        for (std::size_t length = 0; length < bytes.size(); ++length) {
            EXPECT_THROW((void)net::decode_event(std::span(bytes).first(length)), std::runtime_error);
        }
        auto extra = bytes; extra.push_back(0);
        EXPECT_THROW((void)net::decode_event(extra), std::runtime_error);
    }
    EXPECT_THROW((void)net::decode_event(net::Bytes(32, 255)), std::runtime_error);
}

TEST(Protocol, AcknowledgementsTradeUpdatesAndErrorsAreBounded) {
    const net::CommandAck ack{5, 7, true};
    EXPECT_EQ(net::decode_ack(net::encode_ack(ack)), ack);
    EXPECT_THROW((void)net::decode_ack(net::encode_ack({1, 0, false})), std::runtime_error);
    const apex::Event event{1, 1, apex::Trade{1, 2, 3, 4, 5, 2, 100, 10}};
    const auto [sequence, trade] = net::decode_trade_update(net::encode_trade_update(9, event));
    EXPECT_EQ(sequence, 9U); EXPECT_EQ(trade, event);
    EXPECT_EQ(net::decode_sequence(net::encode_sequence(42)), 42U);
    EXPECT_THROW((void)net::decode_sequence({}), std::runtime_error);
    EXPECT_THROW((void)net::encode_trade_update(1, {1, 1, apex::OrderAccepted{}}), std::invalid_argument);
    const auto [code, text] = net::decode_error(net::encode_error(net::ErrorCode::Identity, std::string(300, 'a')));
    EXPECT_EQ(code, net::ErrorCode::Identity); EXPECT_EQ(text.size(), 256U);
}

TEST(Protocol, AggregateBookDepthIsOrderedAndDoesNotCopyOrders) {
    apex::OrderBook book;
    for (apex::OrderId id = 1; id <= 3; ++id) {
        const apex::Order order{id, 1, apex::Side::Buy, apex::OrderType::Limit, id == 3 ? 99 : 100, 10, 10, id, apex::OrderStatus::Resting};
        ASSERT_EQ(book.insert(order), apex::BookError::None);
    }
    const auto view = net::book_view(book, 3, 9, 1);
    ASSERT_EQ(view.bids.size(), 1U);
    EXPECT_EQ(view.bids[0], (net::DepthLevel{100, 20, 2}));
    EXPECT_EQ(net::decode_book(net::encode_book(view)), view);
    EXPECT_TRUE(view.asks.empty());
    EXPECT_THROW((void)net::book_view(book, 0, 0, 0), std::invalid_argument);
    auto bytes = net::encode_book(view); bytes.pop_back();
    EXPECT_THROW((void)net::decode_book(bytes), std::runtime_error);
}

TEST(Protocol, FeedTrackerDistinguishesAnchorsGapsAndDuplicates) {
    net::FeedTracker tracker;
    EXPECT_TRUE(tracker.observe(100, true));
    EXPECT_TRUE(tracker.observe(101));
    EXPECT_FALSE(tracker.observe(103));
    EXPECT_TRUE(tracker.observe(103, true));
    EXPECT_FALSE(tracker.observe(103));
    EXPECT_TRUE(tracker.observe(std::numeric_limits<std::uint64_t>::max(), true));
    EXPECT_FALSE(tracker.observe(0));
}

TEST(Protocol, PrivateOrderSnapshotRoundTripsAtCapacityWithinOneFrame) {
    net::OpenOrders snapshot{1000, 7, {}};
    for (std::uint64_t i = 0; i < 1000; ++i) {
        snapshot.orders.push_back({std::numeric_limits<apex::OrderId>::max() - i, 7,
            i % 2 ? apex::Side::Sell : apex::Side::Buy, apex::OrderType::Limit,
            100 + static_cast<apex::Price>(i), 10, 7, i + 1, apex::OrderStatus::PartiallyFilled});
    }
    const auto bytes = net::encode_open_orders(snapshot);
    EXPECT_EQ(bytes.size(), 16U + 47U * 1000U);
    EXPECT_EQ(net::decode_open_orders(bytes), snapshot);
    EXPECT_LT(net::encode_frame({net::MessageType::OpenOrdersSnapshot, 2, bytes}).size(), net::max_frame_size);
    EXPECT_EQ(net::decode_open_orders(net::encode_open_orders({0, 7, {}})), (net::OpenOrders{0, 7, {}}));
    snapshot.orders.push_back(snapshot.orders.front());
    EXPECT_THROW((void)net::encode_open_orders(snapshot), std::runtime_error);
}

TEST(Protocol, PrivateOrderSnapshotsRejectTruncationForeignDuplicateAndTerminalOrders) {
    net::OpenOrders snapshot{3, 7, {{42, 7, apex::Side::Buy, apex::OrderType::Limit, 100, 10, 5, 1, apex::OrderStatus::PartiallyFilled}}};
    const auto bytes = net::encode_open_orders(snapshot);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        EXPECT_THROW((void)net::decode_open_orders(std::span(bytes).first(length)), std::runtime_error);
    }
    auto extra = bytes; extra.push_back(0);
    EXPECT_THROW((void)net::decode_open_orders(extra), std::runtime_error);
    auto duplicate = bytes; duplicate.insert(duplicate.end(), bytes.begin() + 16, bytes.end()); duplicate[15] = 2;
    EXPECT_THROW((void)net::decode_open_orders(duplicate), std::runtime_error);
    auto large = bytes; large[14] = 3; large[15] = 233; // 1001, rejected before reading entries.
    EXPECT_THROW((void)net::decode_open_orders(large), std::runtime_error);
    snapshot.orders.front().participant_id = 8;
    EXPECT_THROW((void)net::encode_open_orders(snapshot), std::runtime_error);
    snapshot.orders.front().participant_id = 7; snapshot.orders.front().status = apex::OrderStatus::Filled;
    EXPECT_THROW((void)net::encode_open_orders(snapshot), std::runtime_error);
    snapshot.orders.front().status = apex::OrderStatus::PartiallyFilled; snapshot.orders.front().remaining_quantity = 0;
    EXPECT_THROW((void)net::encode_open_orders(snapshot), std::runtime_error);
}
