#include "apex/network/gateway.hpp"
#include "apex/network/client.hpp"
#include "apex/replay/replay.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <mutex>
#include <set>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

namespace net = apex::network;
namespace {
class RunningGateway {
public:
    net::TcpGateway gateway;
    std::exception_ptr failure;
    std::jthread writer;
    explicit RunningGateway(net::GatewayConfig config = [] { net::GatewayConfig c; c.port = 0; return c; }())
        : gateway(std::move(config)), writer([this](std::stop_token stop) {
            try { gateway.run(stop); } catch (...) { failure = std::current_exception(); }
        }) {}
    ~RunningGateway() { if (writer.joinable()) { writer.request_stop(); writer.join(); } }
    void finish() { writer.request_stop(); writer.join(); if (failure) { std::rethrow_exception(failure); } }
};
void identify(net::TcpClient& client, apex::ParticipantId id) {
    client.send(net::hello(1, id));
    const auto frame = client.receive();
    ASSERT_EQ(frame.type, net::MessageType::HelloAck);
    ASSERT_EQ(frame.request_sequence, 1U);
    ASSERT_EQ(net::decode_participant(frame.payload), id);
}
struct Result { net::CommandAck ack; std::vector<apex::Event> events; };
Result submit(net::TcpClient& client, std::uint64_t sequence, const apex::Command& command) {
    client.send(net::command_frame(sequence, command));
    Result result;
    bool acknowledged = false;
    while (!acknowledged || result.events.size() < result.ack.event_count) {
        const auto frame = client.receive();
        if (frame.type == net::MessageType::Error) { throw std::runtime_error(net::decode_error(frame.payload).second); }
        if (frame.request_sequence != sequence) { continue; } // Unsolicited maker reports use zero.
        if (frame.type == net::MessageType::CommandAck && !acknowledged) {
            result.ack = net::decode_ack(frame.payload); acknowledged = true;
        } else if (frame.type == net::MessageType::ExecutionReport && acknowledged) {
            result.events.push_back(net::decode_event(frame.payload));
        } else { throw std::runtime_error("Unexpected command response ordering"); }
    }
    return result;
}
net::BookView snapshot(net::TcpClient& client, std::uint64_t sequence) {
    client.send({net::MessageType::SnapshotRequest, sequence, {}});
    while (true) {
        const auto frame = client.receive();
        if (frame.type == net::MessageType::Error) { throw std::runtime_error(net::decode_error(frame.payload).second); }
        if (frame.type == net::MessageType::BookSnapshot && frame.request_sequence == sequence) { return net::decode_book(frame.payload); }
    }
}
void send_raw(net::TcpClient& client, const net::Bytes& bytes, bool bytewise = false) {
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto count = bytewise ? std::size_t{1} : bytes.size() - offset;
        const auto sent = ::send(client.native_handle(), bytes.data() + offset, count,
#ifdef MSG_NOSIGNAL
                                  MSG_NOSIGNAL
#else
                                  0
#endif
        );
        if (sent < 0 && errno == EINTR) { continue; }
        if (sent <= 0) { throw std::runtime_error("Raw test send failed"); }
        offset += static_cast<std::size_t>(sent);
    }
}
}

TEST(Network, GuideScenarioReportsToTakerMakersAndSequencedSubscribers) {
    RunningGateway server;
    net::TcpClient observer(server.gateway.port()); identify(observer, 0);
    observer.send({net::MessageType::Subscribe, 2, {}});
    ASSERT_EQ(observer.receive().type, net::MessageType::SubscribeAck);
    const auto initial = net::decode_book(observer.receive().payload);
    net::FeedTracker feed;
    ASSERT_TRUE(feed.observe(initial.feed_sequence, true));
    EXPECT_TRUE(initial.bids.empty());
    net::TcpClient a(server.gateway.port()); identify(a, 1);
    net::TcpClient b(server.gateway.port()); identify(b, 2);
    net::TcpClient c(server.gateway.port()); identify(c, 3);
    EXPECT_EQ(submit(a, 2, apex::NewOrder{1, 1, apex::Side::Buy, apex::OrderType::Limit, 10'000, 100, 0}).ack.command_sequence, 1U);
    EXPECT_EQ(submit(b, 2, apex::NewOrder{2, 2, apex::Side::Buy, apex::OrderType::Limit, 10'000, 50, 0}).ack.command_sequence, 2U);
    const auto result = submit(c, 2, apex::NewOrder{3, 3, apex::Side::Sell, apex::OrderType::Limit, 9'900, 120, 0});
    EXPECT_EQ(result.ack.command_sequence, 3U);
    ASSERT_EQ(result.events.size(), 7U);
    EXPECT_EQ(std::get<apex::Trade>(result.events[1].payload).buy_order_id, 1U);
    EXPECT_EQ(std::get<apex::Trade>(result.events[4].payload).quantity, 20U);
    for (auto* maker : {&a, &b}) {
        const auto trade = maker->receive(); const auto update = maker->receive();
        EXPECT_EQ(trade.type, net::MessageType::ExecutionReport); EXPECT_EQ(trade.request_sequence, 0U);
        EXPECT_TRUE(std::holds_alternative<apex::Trade>(net::decode_event(trade.payload).payload));
        EXPECT_TRUE(std::holds_alternative<apex::OrderUpdated>(net::decode_event(update.payload).payload));
    }
    EXPECT_EQ(submit(b, 3, apex::ReplaceOrder{2, 4, 2, 10'100, 40}).ack.command_sequence, 4U);
    unsigned trades = 0; net::BookView last;
    while (last.command_sequence < 4) {
        const auto frame = observer.receive();
        if (frame.type == net::MessageType::TradeUpdate) {
            const auto [sequence, event] = net::decode_trade_update(frame.payload);
            EXPECT_TRUE(feed.observe(sequence)); EXPECT_TRUE(std::holds_alternative<apex::Trade>(event.payload)); ++trades;
        } else {
            ASSERT_EQ(frame.type, net::MessageType::BookSnapshot); last = net::decode_book(frame.payload);
            EXPECT_TRUE(feed.observe(last.feed_sequence));
        }
    }
    EXPECT_EQ(trades, 2U); EXPECT_EQ(feed.last_sequence(), 6U);
    ASSERT_EQ(last.bids.size(), 1U); EXPECT_EQ(last.bids[0].quantity, 40U); EXPECT_TRUE(last.asks.empty());
    const auto resync = snapshot(observer, 3);
    EXPECT_EQ(resync, last); // Explicit snapshots anchor without incrementing the publication sequence.
    observer.send({net::MessageType::Heartbeat, 4, {}});
    EXPECT_EQ(observer.receive().type, net::MessageType::HeartbeatAck);
    server.finish();
}

TEST(Network, FragmentedHeadersCoalescedRequestsAndWriteHalfCloseRemainValid) {
    RunningGateway server;
    net::TcpClient client(server.gateway.port());
    send_raw(client, net::encode_frame(net::hello(1, 1)), true);
    ASSERT_EQ(client.receive().type, net::MessageType::HelloAck);
    auto bytes = net::encode_frame(net::command_frame(2, apex::NewOrder{1, 1, apex::Side::Buy, apex::OrderType::Limit, 100, 10, 0}));
    const auto ping = net::encode_frame({net::MessageType::Heartbeat, 3, {}});
    bytes.insert(bytes.end(), ping.begin(), ping.end());
    send_raw(client, bytes); client.shutdown_write();
    EXPECT_EQ(net::decode_ack(client.receive().payload).command_sequence, 1U);
    EXPECT_EQ(client.receive().type, net::MessageType::ExecutionReport);
    EXPECT_EQ(client.receive().type, net::MessageType::ExecutionReport);
    EXPECT_EQ(client.receive().type, net::MessageType::HeartbeatAck);
    EXPECT_THROW((void)client.receive(), std::runtime_error);
    net::TcpClient observer(server.gateway.port()); identify(observer, 0);
    const auto book = snapshot(observer, 2);
    ASSERT_EQ(book.bids.size(), 1U); EXPECT_EQ(book.bids[0].quantity, 10U);
    server.finish();
}

TEST(Network, IdentityCannotBeForgedAndObserverCannotTrade) {
    RunningGateway server;
    for (const auto id : {1U, 0U}) {
        net::TcpClient client(server.gateway.port()); identify(client, id);
        client.send(net::command_frame(2, apex::NewOrder{1, id == 1 ? 2U : 0U, apex::Side::Buy, apex::OrderType::Limit, 100, 10, 0}));
        const auto error = client.receive();
        ASSERT_EQ(error.type, net::MessageType::Error);
        EXPECT_EQ(net::decode_error(error.payload).first, net::ErrorCode::Identity);
    }
    net::TcpClient observer(server.gateway.port()); identify(observer, 0);
    EXPECT_EQ(snapshot(observer, 2).command_sequence, 0U);
    server.finish();
}

TEST(Network, DuplicateSessionAndRepeatedHelloAreRejected) {
    RunningGateway server;
    net::TcpClient first(server.gateway.port()); identify(first, 1);
    net::TcpClient duplicate(server.gateway.port()); duplicate.send(net::hello(1, 1));
    const auto duplicate_error = duplicate.receive();
    ASSERT_EQ(duplicate_error.type, net::MessageType::Error);
    EXPECT_EQ(net::decode_error(duplicate_error.payload).first, net::ErrorCode::SessionInUse);
    first.send(net::hello(2, 2));
    const auto immutable = first.receive();
    EXPECT_EQ(net::decode_error(immutable.payload).first, net::ErrorCode::Identity);
    server.finish();
}

TEST(Network, RetryAndSequenceGapsNeverExecuteTwice) {
    RunningGateway server;
    for (const auto sequence : {1ULL, 3ULL}) {
        net::TcpClient client(server.gateway.port()); identify(client, static_cast<apex::ParticipantId>(sequence));
        client.send(net::command_frame(sequence, apex::NewOrder{sequence, static_cast<apex::ParticipantId>(sequence), apex::Side::Buy, apex::OrderType::Limit, 100, 10, 0}));
        const auto error = client.receive();
        ASSERT_EQ(error.type, net::MessageType::Error);
        EXPECT_EQ(net::decode_error(error.payload).first, net::ErrorCode::Sequence);
    }
    net::TcpClient observer(server.gateway.port()); identify(observer, 0);
    EXPECT_EQ(snapshot(observer, 2).command_sequence, 0U);
    server.finish();
}

TEST(Network, BusinessRejectsHaveReasonsAndDoNotCloseHealthySession) {
    RunningGateway server;
    net::TcpClient client(server.gateway.port()); identify(client, 1);
    const auto rejected = submit(client, 2, apex::NewOrder{1, 1, apex::Side::Buy, apex::OrderType::Limit, 100, 0, 0});
    EXPECT_FALSE(rejected.ack.accepted); ASSERT_EQ(rejected.events.size(), 1U);
    EXPECT_EQ(std::get<apex::CommandRejected>(rejected.events[0].payload).reason, apex::RejectReason::ZeroQuantity);
    EXPECT_EQ(submit(client, 3, apex::NewOrder{1, 1, apex::Side::Buy, apex::OrderType::Limit, 100, 10, 0}).ack.command_sequence, 2U);
    EXPECT_TRUE(submit(client, 4, apex::CancelOrder{1, 1}).ack.accepted);
    EXPECT_TRUE(snapshot(client, 5).bids.empty());
    server.finish();
}

TEST(Network, MalformedConnectionsDoNotAffectOtherClientsOrBook) {
    RunningGateway server;
    std::vector<net::Bytes> cases;
    auto version = net::encode_frame(net::hello(1, 1)); version[5] = 2; cases.push_back(version);
    auto length = net::encode_frame(net::hello(1, 1)); length[0] = 0xff; cases.push_back(length);
    cases.push_back(net::encode_frame({net::MessageType::NewOrder, 1, {}})); // No HELLO.
    for (const auto& bytes : cases) {
        net::TcpClient client(server.gateway.port()); send_raw(client, bytes);
        EXPECT_EQ(client.receive().type, net::MessageType::Error);
    }
    {
        net::TcpClient client(server.gateway.port()); identify(client, 1);
        send_raw(client, {0, 0, 0}); client.shutdown_write();
        const auto error = client.receive();
        EXPECT_EQ(error.type, net::MessageType::Error);
        EXPECT_EQ(net::decode_error(error.payload).first, net::ErrorCode::Malformed);
    }
    {
        net::TcpClient client(server.gateway.port()); identify(client, 2);
        client.send({static_cast<net::MessageType>(999), 2, {}});
        EXPECT_EQ(net::decode_error(client.receive().payload).first, net::ErrorCode::UnknownMessage);
    }
    net::TcpClient observer(server.gateway.port()); identify(observer, 0);
    EXPECT_EQ(snapshot(observer, 2).command_sequence, 0U);
    server.finish();
}

TEST(Network, ConnectionLimitClosesExcessPeerWithoutBlockingExistingSession) {
    net::GatewayConfig config; config.port = 0; config.max_connections = 1;
    RunningGateway server(config);
    net::TcpClient first(server.gateway.port()); identify(first, 1);
    EXPECT_THROW({ net::TcpClient excess(server.gateway.port()); excess.send(net::hello(1, 2)); (void)excess.receive(); }, std::exception);
    first.send({net::MessageType::Heartbeat, 2, {}});
    EXPECT_EQ(first.receive().type, net::MessageType::HeartbeatAck);
    server.finish();
}

TEST(Network, ConcurrentClientsHaveOneAcceptedOrderAndJournalReplaysLargeFill) {
    auto pattern = (std::filesystem::temp_directory_path() / "apex-network-test-XXXXXX").string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    const std::filesystem::path directory = pattern;
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{directory};
    net::GatewayConfig config; config.port = 0; config.journal_path = directory / "session.apex";
    RunningGateway server(config);
    std::mutex mutex;
    std::vector<apex::CommandSequence> sequences;
    std::vector<std::exception_ptr> failures;
    std::vector<std::jthread> clients;
    for (apex::ParticipantId participant = 1; participant <= 8; ++participant) {
        clients.emplace_back([&, participant] {
            try {
                net::TcpClient client(server.gateway.port()); identify(client, participant);
                for (std::uint64_t i = 0; i < 10; ++i) {
                    const auto id = participant * 100 + i;
                    const auto result = submit(client, i + 2,
                        apex::NewOrder{id, participant, apex::Side::Buy, apex::OrderType::Limit, 100, 10, 0});
                    if (!result.ack.accepted) { throw std::runtime_error("Unexpected concurrent rejection"); }
                    std::lock_guard lock(mutex); sequences.push_back(result.ack.command_sequence);
                }
            } catch (...) { std::lock_guard lock(mutex); failures.push_back(std::current_exception()); }
        });
    }
    clients.clear(); // jthread joins; clients independently submit concurrently.
    ASSERT_TRUE(failures.empty());
    ASSERT_EQ(sequences.size(), 80U);
    std::sort(sequences.begin(), sequences.end());
    for (std::size_t i = 0; i < sequences.size(); ++i) { EXPECT_EQ(sequences[i], i + 1); }
    net::TcpClient observer(server.gateway.port()); identify(observer, 0);
    observer.send({net::MessageType::Subscribe, 2, {}});
    ASSERT_EQ(observer.receive().type, net::MessageType::SubscribeAck);
    const auto before = net::decode_book(observer.receive().payload);
    ASSERT_EQ(before.bids.size(), 1U); EXPECT_EQ(before.bids[0].quantity, 800U); EXPECT_EQ(before.bids[0].orders, 80U);
    net::TcpClient taker(server.gateway.port()); identify(taker, 9);
    const auto fill = submit(taker, 2, apex::NewOrder{9999, 9, apex::Side::Sell, apex::OrderType::Market, 0, 800, 0});
    EXPECT_EQ(fill.ack.command_sequence, 81U); EXPECT_EQ(fill.events.size(), 241U); // Larger than one 16 KiB write budget.
    net::FeedTracker tracker; EXPECT_TRUE(tracker.observe(before.feed_sequence, true));
    unsigned trades = 0;
    while (true) {
        const auto frame = observer.receive();
        if (frame.type == net::MessageType::TradeUpdate) {
            const auto [sequence, event] = net::decode_trade_update(frame.payload);
            EXPECT_TRUE(tracker.observe(sequence)); EXPECT_EQ(std::get<apex::Trade>(event.payload).quantity, 10U); ++trades;
        } else {
            ASSERT_EQ(frame.type, net::MessageType::BookSnapshot);
            const auto book = net::decode_book(frame.payload);
            EXPECT_TRUE(tracker.observe(book.feed_sequence)); EXPECT_TRUE(book.bids.empty()); EXPECT_TRUE(book.asks.empty()); break;
        }
    }
    EXPECT_EQ(trades, 80U);
    server.finish();
    const auto replay = apex::replay(config.journal_path);
    EXPECT_EQ(replay.commands, 81U); EXPECT_EQ(replay.events, 401U);
    EXPECT_TRUE(replay.final_book.bids.empty()); EXPECT_TRUE(replay.final_book.asks.empty());
    EXPECT_THROW(server.gateway.run(), std::logic_error);
}

TEST(NetworkConfig, InvalidBoundsFailBeforeBinding) {
    net::GatewayConfig config;
    config.port = 0; config.inbox_capacity = 0;
    EXPECT_THROW(net::TcpGateway server(config), std::invalid_argument);
    config.inbox_capacity = 1; config.output_capacity_bytes = 1;
    EXPECT_THROW(net::TcpGateway server(config), std::invalid_argument);
    config.output_capacity_bytes = net::max_frame_size; config.limits.max_open_orders = 1001;
    EXPECT_THROW(net::TcpGateway server(config), std::invalid_argument);
}

TEST(Network, HandshakeIdleAndPartialFrameDeadlinesAreObservable) {
    for (int mode = 0; mode < 3; ++mode) {
        net::GatewayConfig config; config.port = 0;
        config.handshake_timeout = std::chrono::milliseconds(mode == 0 ? 100 : 2000);
        config.idle_timeout = std::chrono::milliseconds(mode == 1 ? 100 : 2000);
        config.partial_frame_timeout = std::chrono::milliseconds(100);
        RunningGateway server(config);
        net::TcpClient client(server.gateway.port());
        if (mode != 0) { identify(client, 1); }
        if (mode == 2) { send_raw(client, {0}); }
        EXPECT_THROW((void)client.receive(), std::runtime_error);
        server.finish();
        EXPECT_EQ(server.gateway.stats().deadline_disconnects, 1U);
    }
}

TEST(Network, OverloadDropsUnexecutedRequestsAndOtherSessionsRemainUsable) {
    net::GatewayConfig config; config.port = 0; config.inbox_capacity = 1;
    RunningGateway server(config);
    net::TcpClient client(server.gateway.port()); identify(client, 1);
    net::Bytes batch;
    for (std::uint64_t sequence = 2; sequence < 50; ++sequence) {
        const auto bytes = net::encode_frame({net::MessageType::Heartbeat, sequence, {}});
        batch.insert(batch.end(), bytes.begin(), bytes.end());
    }
    send_raw(client, batch);
    EXPECT_EQ(client.receive().type, net::MessageType::Error);
    net::TcpClient healthy(server.gateway.port()); identify(healthy, 2);
    healthy.send({net::MessageType::Heartbeat, 2, {}});
    EXPECT_EQ(healthy.receive().type, net::MessageType::HeartbeatAck);
    server.finish();
    const auto stats = server.gateway.stats();
    EXPECT_GE(stats.inbox_rejections, 1U);
    EXPECT_GE(stats.frames_discarded, 1U);
    EXPECT_EQ(stats.inbox_high_watermark, 1U);
    EXPECT_EQ(stats.inbox_size, 0U);
}

TEST(Network, OutputOverflowDisconnectsConsumerWithoutInterruptingMatching) {
    net::GatewayConfig config; config.port = 0; config.output_capacity_bytes = net::max_frame_size;
    RunningGateway server(config);
    net::TcpClient maker(server.gateway.port()); identify(maker, 1);
    for (std::uint64_t i = 0; i < 400; ++i) {
        ASSERT_TRUE(submit(maker, i + 2, apex::NewOrder{i + 1, 1, apex::Side::Sell,
            apex::OrderType::Limit, 100, 1, 0}).ack.accepted);
    }
    net::TcpClient taker(server.gateway.port()); identify(taker, 2);
    taker.send(net::command_frame(2, apex::NewOrder{1001, 2, apex::Side::Buy,
        apex::OrderType::Market, 0, 400, 0}));
    // One command creates more than the per-peer cap before the next write poll.
    // Do not read either consumer until the complete fill has been processed.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (server.gateway.stats().output_overflows == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_GE(server.gateway.stats().output_overflows, 1U);
    net::TcpClient observer(server.gateway.port()); identify(observer, 0);
    EXPECT_TRUE(snapshot(observer, 2).asks.empty());
    server.finish();
}

namespace {
net::OpenOrders private_orders(net::TcpClient& client, std::uint64_t sequence) {
    client.send({net::MessageType::OpenOrdersRequest, sequence, {}});
    while (true) {
        const auto frame = client.receive();
        if (frame.type == net::MessageType::Error) { throw std::runtime_error(net::decode_error(frame.payload).second); }
        if (frame.request_sequence == sequence && frame.type == net::MessageType::OpenOrdersSnapshot) {
            return net::decode_open_orders(frame.payload);
        }
    }
}
}
TEST(Network, PrivateSnapshotRecoversRemainingOwnedOrdersAfterDisconnectedFill) {
    RunningGateway server;
    {
        net::TcpClient original(server.gateway.port()); identify(original, 1);
        ASSERT_TRUE(submit(original, 2, apex::NewOrder{1, 1, apex::Side::Buy, apex::OrderType::Limit, 100, 10, 0}).ack.accepted);
        ASSERT_TRUE(submit(original, 3, apex::NewOrder{2, 1, apex::Side::Sell, apex::OrderType::Limit, 110, 20, 0}).ack.accepted);
        ASSERT_TRUE(submit(original, 4, apex::NewOrder{4, 1, apex::Side::Buy, apex::OrderType::Limit, 101, 2, 0}).ack.accepted);
    }
    net::TcpClient other(server.gateway.port()); identify(other, 2);
    ASSERT_TRUE(submit(other, 2, apex::NewOrder{3, 2, apex::Side::Sell, apex::OrderType::Market, 0, 6, 0}).ack.accepted);
    ASSERT_TRUE(submit(other, 3, apex::NewOrder{99, 2, apex::Side::Buy, apex::OrderType::Limit, 90, 8, 0}).ack.accepted);
    net::TcpClient restored(server.gateway.port()); identify(restored, 1);
    const auto snapshot = private_orders(restored, 2);
    ASSERT_EQ(snapshot.orders.size(), 2U); EXPECT_EQ(snapshot.command_sequence, 5U); EXPECT_EQ(snapshot.participant, 1U);
    EXPECT_EQ(snapshot.orders[0].client_order_id, 1U); EXPECT_EQ(snapshot.orders[0].remaining_quantity, 6U);
    EXPECT_EQ(snapshot.orders[1].client_order_id, 2U);
    const auto theirs = private_orders(other, 4);
    ASSERT_EQ(theirs.orders.size(), 1U); EXPECT_EQ(theirs.orders.front().client_order_id, 99U);
    ASSERT_TRUE(submit(restored, 3, apex::CancelOrder{1, 1}).ack.accepted);
    const auto refreshed = private_orders(restored, 4);
    EXPECT_EQ(refreshed.command_sequence, 6U); ASSERT_EQ(refreshed.orders.size(), 1U);
    EXPECT_EQ(refreshed.orders.front().client_order_id, 2U);
    server.finish();
}
TEST(Network, PrivateSnapshotRejectsObserversAndIdentityPayloadsWithoutEngineCommands) {
    RunningGateway server;
    net::TcpClient observer(server.gateway.port()); identify(observer, 0);
    observer.send({net::MessageType::OpenOrdersRequest, 2, {}});
    auto error = observer.receive(); ASSERT_EQ(error.type, net::MessageType::Error);
    EXPECT_EQ(net::decode_error(error.payload).first, net::ErrorCode::Identity);
    net::TcpClient spoof(server.gateway.port()); identify(spoof, 1);
    spoof.send({net::MessageType::OpenOrdersRequest, 2, net::hello(1, 2).payload});
    error = spoof.receive(); ASSERT_EQ(error.type, net::MessageType::Error);
    EXPECT_EQ(net::decode_error(error.payload).first, net::ErrorCode::Malformed);
    net::TcpClient healthy(server.gateway.port()); identify(healthy, 2);
    const auto snapshot = private_orders(healthy, 2);
    EXPECT_EQ(snapshot.command_sequence, 0U); EXPECT_TRUE(snapshot.orders.empty());
    server.finish();
}
TEST(Network, MaximumPrivateSnapshotSpansWriteBudgetsWithoutLosingOrders) {
    RunningGateway server;
    net::TcpClient client(server.gateway.port()); identify(client, 1);
    for (std::uint64_t i = 1; i <= 1000; ++i) {
        ASSERT_TRUE(submit(client, i + 1, apex::NewOrder{i, 1, apex::Side::Buy, apex::OrderType::Limit, 100, 1, 0}).ack.accepted);
    }
    const auto snapshot = private_orders(client, 1002);
    EXPECT_EQ(snapshot.command_sequence, 1000U); ASSERT_EQ(snapshot.orders.size(), 1000U);
    for (std::size_t i = 0; i < snapshot.orders.size(); ++i) { EXPECT_EQ(snapshot.orders[i].client_order_id, i + 1); }
    server.finish();
}
