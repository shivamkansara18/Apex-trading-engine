#pragma once

#include "apex/core/event.hpp"
#include "apex/book/order_book.hpp"

#include <span>
#include <string>

namespace apex::network {
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint16_t protocol_version = 1;
constexpr std::uint32_t header_size = 16;
constexpr std::uint32_t max_frame_size = 65'536;

enum class MessageType : std::uint16_t {
    Hello = 1, NewOrder = 2, CancelOrder = 3, ReplaceOrder = 4,
    Subscribe = 5, SnapshotRequest = 6, Heartbeat = 7, OpenOrdersRequest = 8,
    HelloAck = 101, CommandAck = 102, ExecutionReport = 103,
    SubscribeAck = 104, HeartbeatAck = 105, OpenOrdersSnapshot = 106, Error = 199,
    TradeUpdate = 201, BookSnapshot = 202
};
enum class ErrorCode : std::uint32_t {
    Malformed = 1, Sequence = 2, Identity = 3, UnknownMessage = 4,
    Overloaded = 5, SessionInUse = 6
};
struct Frame {
    MessageType type{};
    std::uint64_t request_sequence{};
    Bytes payload;
    bool operator==(const Frame&) const = default;
};
class FrameDecoder {
public:
    [[nodiscard]] std::vector<Frame> append(std::span<const std::uint8_t> bytes);
    [[nodiscard]] bool partial() const noexcept { return !buffer_.empty(); }
private:
    Bytes buffer_;
    std::size_t expected_{header_size};
};
struct CommandAck {
    CommandSequence command_sequence{};
    std::uint32_t event_count{};
    bool accepted{};
    bool operator==(const CommandAck&) const = default;
};
struct OpenOrders {
    CommandSequence command_sequence{};
    ParticipantId participant{};
    std::vector<Order> orders;
    bool operator==(const OpenOrders&) const = default;
};
struct DepthLevel {
    Price price_ticks{};
    Quantity quantity{};
    std::uint32_t orders{};
    bool operator==(const DepthLevel&) const = default;
};
struct BookView {
    std::uint64_t feed_sequence{};
    CommandSequence command_sequence{};
    std::vector<DepthLevel> bids;
    std::vector<DepthLevel> asks;
    bool operator==(const BookView&) const = default;
};

[[nodiscard]] Bytes encode_frame(const Frame& frame);
[[nodiscard]] Frame hello(std::uint64_t sequence, ParticipantId participant);
[[nodiscard]] ParticipantId decode_participant(std::span<const std::uint8_t> payload);
[[nodiscard]] Frame command_frame(std::uint64_t sequence, const Command& command);
[[nodiscard]] Command decode_command(const Frame& frame);
[[nodiscard]] Bytes encode_open_orders(const OpenOrders& snapshot);
[[nodiscard]] OpenOrders decode_open_orders(std::span<const std::uint8_t> payload);
[[nodiscard]] Bytes encode_ack(const CommandAck& ack);
[[nodiscard]] CommandAck decode_ack(std::span<const std::uint8_t> payload);
[[nodiscard]] Bytes encode_event(const Event& event);
[[nodiscard]] Event decode_event(std::span<const std::uint8_t> payload);
[[nodiscard]] Bytes encode_sequence(std::uint64_t sequence);
[[nodiscard]] std::uint64_t decode_sequence(std::span<const std::uint8_t> payload);
[[nodiscard]] Bytes encode_trade_update(std::uint64_t sequence, const Event& trade);
[[nodiscard]] std::pair<std::uint64_t, Event> decode_trade_update(std::span<const std::uint8_t> payload);
[[nodiscard]] BookView book_view(const OrderBook& book, CommandSequence command_sequence,
                                  std::uint64_t feed_sequence, std::size_t depth);
[[nodiscard]] Bytes encode_book(const BookView& view);
[[nodiscard]] BookView decode_book(std::span<const std::uint8_t> payload);
[[nodiscard]] Bytes encode_error(ErrorCode code, std::string_view message);
[[nodiscard]] std::pair<ErrorCode, std::string> decode_error(std::span<const std::uint8_t> payload);

class FeedTracker {
public:
    // Correlated snapshots are anchors, not increments. False indicates a gap.
    [[nodiscard]] bool observe(std::uint64_t sequence, bool anchor = false);
    [[nodiscard]] std::uint64_t last_sequence() const noexcept { return last_; }
private:
    std::uint64_t last_{};
    bool anchored_{};
};
} // namespace apex::network
