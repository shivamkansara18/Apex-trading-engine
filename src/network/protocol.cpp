#include "apex/network/protocol.hpp"
#include "../journal/codec.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace apex::network {
namespace {
using journal_detail::Encoder;
using journal_detail::Decoder;
void u16(Encoder& out, std::uint16_t value) {
    out.u8(static_cast<std::uint8_t>(value >> 8)); out.u8(static_cast<std::uint8_t>(value));
}
std::uint16_t u16(Decoder& in) {
    const auto high = in.u8(); return static_cast<std::uint16_t>((static_cast<unsigned>(high) << 8) | in.u8());
}
void end(const Decoder& in) { if (!in.remaining().empty()) { throw std::runtime_error("Trailing payload bytes"); } }
void encode_order(Encoder& out, const Order& order) {
    out.u64(order.client_order_id); out.u32(order.participant_id);
    out.u8(static_cast<std::uint8_t>(order.side)); out.u8(static_cast<std::uint8_t>(order.type));
    out.price(order.price_ticks); out.u64(order.original_quantity); out.u64(order.remaining_quantity);
    out.u64(order.priority_sequence); out.u8(static_cast<std::uint8_t>(order.status));
}
void validate_open_orders(const OpenOrders& snapshot) {
    if (snapshot.participant == 0 || snapshot.orders.size() > 1000) { throw std::runtime_error("Invalid private order snapshot bounds"); }
    std::set<OrderId> ids;
    for (const auto& order : snapshot.orders) {
        if (order.participant_id != snapshot.participant || order.type != OrderType::Limit ||
            (order.side != Side::Buy && order.side != Side::Sell) || order.price_ticks <= 0 ||
            order.remaining_quantity == 0 || order.remaining_quantity > order.original_quantity ||
            (order.status != OrderStatus::Resting && order.status != OrderStatus::PartiallyFilled) ||
            order.priority_sequence == 0 || order.priority_sequence > snapshot.command_sequence ||
            !ids.insert(order.client_order_id).second) { throw std::runtime_error("Invalid private open order"); }
    }
}
Order decode_order(Decoder& in) {
    Order order{in.u64(), in.u32(), static_cast<Side>(in.u8()), static_cast<OrderType>(in.u8()),
                in.price(), in.u64(), in.u64(), in.u64(), static_cast<OrderStatus>(in.u8())};
    if (static_cast<unsigned>(order.side) > 1 || static_cast<unsigned>(order.type) > 1 ||
        static_cast<unsigned>(order.status) > static_cast<unsigned>(OrderStatus::Rejected)) {
        throw std::runtime_error("Invalid event enum");
    }
    return order;
}
}
Bytes encode_frame(const Frame& frame) {
    if (frame.payload.size() > max_frame_size - header_size) { throw std::length_error("Frame exceeds maximum length"); }
    Encoder out;
    out.u32(static_cast<std::uint32_t>(header_size + frame.payload.size()));
    u16(out, protocol_version); u16(out, static_cast<std::uint16_t>(frame.type));
    out.u64(frame.request_sequence);
    out.bytes.insert(out.bytes.end(), frame.payload.begin(), frame.payload.end());
    return out.bytes;
}
std::vector<Frame> FrameDecoder::append(std::span<const std::uint8_t> bytes) {
    std::vector<Frame> result;
    while (!bytes.empty()) {
        const auto count = std::min(expected_ - buffer_.size(), bytes.size());
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(count));
        bytes = bytes.subspan(count);
        if (buffer_.size() < expected_) { break; }
        if (expected_ == header_size) {
            Decoder header{buffer_};
            const auto length = header.u32();
            if (length < header_size || length > max_frame_size) { throw std::runtime_error("Invalid frame length"); }
            if (u16(header) != protocol_version) { throw std::runtime_error("Unsupported protocol version"); }
            expected_ = length;
        }
        if (buffer_.size() == expected_) {
            Decoder in{buffer_};
            (void)in.u32(); (void)u16(in);
            const auto type = static_cast<MessageType>(u16(in));
            const auto sequence = in.u64();
            result.push_back({type, sequence, {in.remaining().begin(), in.remaining().end()}});
            buffer_.clear(); expected_ = header_size;
        }
    }
    return result;
}
Frame hello(std::uint64_t sequence, ParticipantId participant) {
    Encoder out; out.u32(participant); return {MessageType::Hello, sequence, out.bytes};
}
ParticipantId decode_participant(std::span<const std::uint8_t> payload) {
    Decoder in{payload}; const auto id = in.u32(); end(in); return id;
}
Frame command_frame(std::uint64_t sequence, const Command& command) {
    Encoder out;
    journal_detail::encode_input(out, command);
    const auto tag = out.bytes[1];
    return {static_cast<MessageType>(tag + 1), sequence, {out.bytes.begin() + 2, out.bytes.end()}};
}
Command decode_command(const Frame& frame) {
    const auto type = static_cast<std::uint16_t>(frame.type);
    if (type < 2 || type > 4) { throw std::runtime_error("Not a command request"); }
    Encoder out; out.u8(4); out.u8(static_cast<std::uint8_t>(type - 1));
    out.bytes.insert(out.bytes.end(), frame.payload.begin(), frame.payload.end());
    Decoder in{out.bytes};
    const auto input = journal_detail::decode_input(in); end(in);
    return std::get<Command>(input);
}
Bytes encode_open_orders(const OpenOrders& snapshot) {
    validate_open_orders(snapshot);
    Encoder out; out.u64(snapshot.command_sequence); out.u32(snapshot.participant);
    out.u32(static_cast<std::uint32_t>(snapshot.orders.size()));
    for (const auto& order : snapshot.orders) { encode_order(out, order); }
    return out.bytes;
}
OpenOrders decode_open_orders(std::span<const std::uint8_t> payload) {
    Decoder in{payload}; OpenOrders snapshot; snapshot.command_sequence = in.u64(); snapshot.participant = in.u32();
    const auto count = in.u32(); if (count > 1000) { throw std::runtime_error("Private snapshot exceeds bound"); }
    snapshot.orders.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) { snapshot.orders.push_back(decode_order(in)); }
    end(in); validate_open_orders(snapshot); return snapshot;
}
Bytes encode_ack(const CommandAck& ack) {
    Encoder out; out.u64(ack.command_sequence); out.u32(ack.event_count); out.u8(ack.accepted ? 1 : 0); return out.bytes;
}
CommandAck decode_ack(std::span<const std::uint8_t> payload) {
    Decoder in{payload}; const CommandAck result{in.u64(), in.u32(), in.boolean()}; end(in);
    if (result.event_count == 0 || result.event_count > 100'000) { throw std::runtime_error("Invalid report count"); }
    return result;
}
Bytes encode_event(const Event& event) { return journal_detail::encode_events({event}); }
Event decode_event(std::span<const std::uint8_t> payload) {
    Decoder in{payload};
    if (in.u64() != 1) { throw std::runtime_error("Expected one event per report"); }
    Event event; event.event_sequence = in.u64(); event.command_sequence = in.u64();
    switch (in.u8()) {
    case 1: event.payload = OrderAccepted{decode_order(in)}; break;
    case 2: event.payload = OrderUpdated{decode_order(in)}; break;
    case 3: {
        auto order = decode_order(in); const auto reason = in.u8();
        if (reason > static_cast<unsigned>(CancelReason::Replaced)) { throw std::runtime_error("Unknown cancel reason"); }
        event.payload = OrderCancelled{order, static_cast<CancelReason>(reason)}; break;
    }
    case 4: {
        // Reuse the canonical input decoder; the rejected command tag follows.
        Encoder bytes; bytes.u8(4); bytes.bytes.insert(bytes.bytes.end(), in.remaining().begin(), in.remaining().end());
        Decoder rejected{bytes.bytes}; auto input = journal_detail::decode_input(rejected);
        const auto reason = rejected.u8(); end(rejected);
        if (reason > static_cast<unsigned>(RejectReason::TradingDisabled)) { throw std::runtime_error("Unknown reject reason"); }
        event.payload = CommandRejected{std::get<Command>(input), static_cast<RejectReason>(reason)};
        return event;
    }
    case 5: event.payload = Trade{in.u64(), in.u64(), in.u64(), in.u32(), in.u32(), in.u64(), in.price(), in.u64()}; break;
    default: throw std::runtime_error("Unknown report event type");
    }
    end(in); return event;
}
Bytes encode_sequence(std::uint64_t sequence) { Encoder out; out.u64(sequence); return out.bytes; }
std::uint64_t decode_sequence(std::span<const std::uint8_t> payload) {
    Decoder in{payload}; const auto value = in.u64(); end(in); return value;
}
Bytes encode_trade_update(std::uint64_t sequence, const Event& trade) {
    if (!std::holds_alternative<Trade>(trade.payload)) { throw std::invalid_argument("Not a trade update"); }
    auto result = encode_sequence(sequence); const auto event = encode_event(trade);
    result.insert(result.end(), event.begin(), event.end()); return result;
}
std::pair<std::uint64_t, Event> decode_trade_update(std::span<const std::uint8_t> payload) {
    Decoder in{payload}; const auto sequence = in.u64(); auto event = decode_event(in.remaining());
    if (!std::holds_alternative<Trade>(event.payload)) { throw std::runtime_error("Feed event is not a trade"); }
    return {sequence, event};
}
BookView book_view(const OrderBook& book, CommandSequence command_sequence, std::uint64_t feed_sequence, std::size_t depth) {
    if (depth == 0 || depth > 64) { throw std::invalid_argument("Depth must be 1..64"); }
    BookView view{feed_sequence, command_sequence, {}, {}};
    const auto collect = [&](Side side, auto& levels) {
        book.visit_orders(side, [&](const Order& order) {
            if (levels.empty() || levels.back().price_ticks != order.price_ticks) {
                if (levels.size() == depth) { return false; }
                levels.push_back({order.price_ticks, 0, 0});
            }
            if (levels.back().orders == std::numeric_limits<std::uint32_t>::max()) { throw std::overflow_error("Depth order count overflow"); }
            levels.back().quantity += order.remaining_quantity; ++levels.back().orders; return true;
        });
    };
    collect(Side::Buy, view.bids); collect(Side::Sell, view.asks); return view;
}
Bytes encode_book(const BookView& view) {
    Encoder out; out.u64(view.feed_sequence); out.u64(view.command_sequence);
    for (const auto* levels : {&view.bids, &view.asks}) {
        if (levels->size() > 64) { throw std::length_error("Too many book levels"); }
        out.u32(static_cast<std::uint32_t>(levels->size()));
        for (const auto& level : *levels) { out.price(level.price_ticks); out.u64(level.quantity); out.u32(level.orders); }
    }
    return out.bytes;
}
BookView decode_book(std::span<const std::uint8_t> payload) {
    Decoder in{payload}; BookView view; view.feed_sequence = in.u64(); view.command_sequence = in.u64();
    for (auto* levels : {&view.bids, &view.asks}) {
        const auto count = in.u32(); if (count > 64) { throw std::runtime_error("Book depth exceeds bound"); }
        for (std::uint32_t i = 0; i < count; ++i) { levels->push_back({in.price(), in.u64(), in.u32()}); }
    }
    end(in); return view;
}
Bytes encode_error(ErrorCode code, std::string_view message) {
    Encoder out; out.u32(static_cast<std::uint32_t>(code));
    const auto count = std::min<std::size_t>(message.size(), 256);
    out.bytes.insert(out.bytes.end(), message.begin(), message.begin() + static_cast<std::ptrdiff_t>(count)); return out.bytes;
}
std::pair<ErrorCode, std::string> decode_error(std::span<const std::uint8_t> payload) {
    Decoder in{payload}; const auto code = static_cast<ErrorCode>(in.u32());
    if (in.remaining().size() > 256) { throw std::runtime_error("Error text exceeds bound"); }
    return {code, {in.remaining().begin(), in.remaining().end()}};
}
bool FeedTracker::observe(std::uint64_t sequence, bool anchor) {
    const bool valid = anchor || (anchored_ && last_ != std::numeric_limits<std::uint64_t>::max() && sequence == last_ + 1);
    last_ = sequence; anchored_ = true; return valid;
}
} // namespace apex::network
