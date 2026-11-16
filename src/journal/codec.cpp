#include "codec.hpp"

#include <bit>
#include <stdexcept>
#include <type_traits>

namespace apex::journal_detail {
void Encoder::u8(std::uint8_t value) { bytes.push_back(value); }
void Encoder::u32(std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) { u8(static_cast<std::uint8_t>(value >> shift)); }
}
void Encoder::u64(std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) { u8(static_cast<std::uint8_t>(value >> shift)); }
}
void Encoder::price(Price value) { u64(std::bit_cast<std::uint64_t>(value)); }
std::uint8_t Decoder::u8() {
    if (offset == bytes.size()) { throw std::runtime_error("Truncated journal payload"); }
    return bytes[offset++];
}
std::uint32_t Decoder::u32() {
    std::uint32_t result = 0;
    for (int i = 0; i < 4; ++i) { result = (result << 8) | u8(); }
    return result;
}
std::uint64_t Decoder::u64() {
    std::uint64_t result = 0;
    for (int i = 0; i < 8; ++i) { result = (result << 8) | u8(); }
    return result;
}
Price Decoder::price() { return std::bit_cast<Price>(u64()); }
bool Decoder::boolean() {
    const auto value = u8();
    if (value > 1) { throw std::runtime_error("Invalid journal boolean"); }
    return value == 1;
}
std::span<const std::uint8_t> Decoder::remaining() const { return bytes.subspan(offset); }

namespace {
void encode_limits(Encoder& out, const RiskLimits& limits) {
    out.u64(limits.max_order_quantity);
    out.u64(limits.max_order_notional);
    out.u64(limits.max_open_orders);
    out.price(limits.min_price_ticks);
    out.price(limits.max_price_ticks);
    out.u8(limits.trading_enabled ? 1 : 0);
}
RiskLimits decode_limits(Decoder& in) {
    return {in.u64(), in.u64(), in.u64(), in.price(), in.price(), in.boolean()};
}
void encode_command(Encoder& out, const Command& command) {
    std::visit([&](const auto& request) {
        using T = std::decay_t<decltype(request)>;
        if constexpr (std::is_same_v<T, NewOrder>) {
            out.u8(1); out.u64(request.client_order_id); out.u32(request.participant_id);
            out.u8(static_cast<std::uint8_t>(request.side));
            out.u8(static_cast<std::uint8_t>(request.type));
            out.price(request.price_ticks); out.u64(request.quantity); out.u64(request.client_timestamp_ns);
        } else if constexpr (std::is_same_v<T, CancelOrder>) {
            out.u8(2); out.u64(request.client_order_id); out.u32(request.participant_id);
        } else {
            out.u8(3); out.u64(request.old_client_order_id); out.u64(request.new_client_order_id);
            out.u32(request.participant_id); out.price(request.new_price_ticks); out.u64(request.new_quantity);
        }
    }, command);
}
Command decode_command(Decoder& in) {
    switch (in.u8()) {
    case 1: return NewOrder{in.u64(), in.u32(), static_cast<Side>(in.u8()),
                            static_cast<OrderType>(in.u8()), in.price(), in.u64(), in.u64()};
    case 2: return CancelOrder{in.u64(), in.u32()};
    case 3: return ReplaceOrder{in.u64(), in.u64(), in.u32(), in.price(), in.u64()};
    default: throw std::runtime_error("Unknown journal command type");
    }
}
void encode_order(Encoder& out, const Order& order) {
    out.u64(order.client_order_id); out.u32(order.participant_id);
    out.u8(static_cast<std::uint8_t>(order.side)); out.u8(static_cast<std::uint8_t>(order.type));
    out.price(order.price_ticks); out.u64(order.original_quantity); out.u64(order.remaining_quantity);
    out.u64(order.priority_sequence); out.u8(static_cast<std::uint8_t>(order.status));
}
}

void encode_input(Encoder& out, const JournalInput& input) {
    std::visit([&](const auto& action) {
        using T = std::decay_t<decltype(action)>;
        if constexpr (std::is_same_v<T, RegisterParticipant>) {
            out.u8(1); out.u32(action.id); encode_limits(out, action.limits);
        } else if constexpr (std::is_same_v<T, ConfigureParticipant>) {
            out.u8(2); out.u32(action.id); encode_limits(out, action.limits);
        } else if constexpr (std::is_same_v<T, SetTradingEnabled>) {
            out.u8(3); out.u8(action.enabled ? 1 : 0);
        } else {
            out.u8(4); encode_command(out, action);
        }
    }, input);
}
JournalInput decode_input(Decoder& in) {
    switch (in.u8()) {
    case 1: return RegisterParticipant{in.u32(), decode_limits(in)};
    case 2: return ConfigureParticipant{in.u32(), decode_limits(in)};
    case 3: return SetTradingEnabled{in.boolean()};
    case 4: return decode_command(in);
    default: throw std::runtime_error("Unknown journal input type");
    }
}

Bytes encode_events(const std::vector<Event>& events) {
    Encoder out;
    out.u64(events.size());
    for (const auto& event : events) {
        out.u64(event.event_sequence); out.u64(event.command_sequence);
        std::visit([&](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, OrderAccepted>) {
                out.u8(1); encode_order(out, payload.order);
            } else if constexpr (std::is_same_v<T, OrderUpdated>) {
                out.u8(2); encode_order(out, payload.order);
            } else if constexpr (std::is_same_v<T, OrderCancelled>) {
                out.u8(3); encode_order(out, payload.order); out.u8(static_cast<std::uint8_t>(payload.reason));
            } else if constexpr (std::is_same_v<T, CommandRejected>) {
                out.u8(4); encode_command(out, payload.command); out.u8(static_cast<std::uint8_t>(payload.reason));
            } else {
                out.u8(5); out.u64(payload.trade_id); out.u64(payload.buy_order_id); out.u64(payload.sell_order_id);
                out.u32(payload.buyer_id); out.u32(payload.seller_id); out.u64(payload.resting_order_id);
                out.price(payload.price_ticks); out.u64(payload.quantity);
            }
        }, event.payload);
    }
    return out.bytes;
}
Bytes encode_result(const JournalResult& result, const BookSnapshot& snapshot) {
    Encoder out;
    out.u8(result.admin_result ? 1 : 0);
    auto events = encode_events(result.events);
    out.bytes.insert(out.bytes.end(), events.begin(), events.end());
    out.u64(book_hash(snapshot));
    return out.bytes;
}
JournalResult apply(MatchingEngine& engine, const JournalInput& input) {
    JournalResult result;
    std::visit([&](const auto& action) {
        using T = std::decay_t<decltype(action)>;
        if constexpr (std::is_same_v<T, RegisterParticipant>) {
            result.admin_result = engine.register_participant(action.id, action.limits);
        } else if constexpr (std::is_same_v<T, ConfigureParticipant>) {
            result.admin_result = engine.configure_participant(action.id, action.limits);
        } else if constexpr (std::is_same_v<T, SetTradingEnabled>) {
            engine.set_trading_enabled(action.enabled);
        } else {
            result.events = engine.process(action);
        }
    }, input);
    return result;
}
std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t result = 0xffffffffU;
    for (const auto byte : bytes) {
        result ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            result = (result >> 1) ^ ((result & 1U) ? 0xedb88320U : 0U);
        }
    }
    return result ^ 0xffffffffU;
}
std::uint64_t hash_bytes(std::span<const std::uint8_t> bytes, std::uint64_t state) {
    for (const auto byte : bytes) { state = (state ^ byte) * 1099511628211ULL; }
    return state;
}
} // namespace apex::journal_detail

namespace apex {
std::uint64_t book_hash(const BookSnapshot& snapshot) {
    journal_detail::Encoder out;
    out.u64(snapshot.last_command_sequence);
    const auto encode_levels = [&](const auto& levels) {
        out.u64(levels.size());
        for (const auto& level : levels) {
            out.price(level.price_ticks); out.u64(level.total_quantity); out.u64(level.orders.size());
            for (const auto& order : level.orders) {
                journal_detail::encode_order(out, order);
            }
        }
    };
    encode_levels(snapshot.bids); encode_levels(snapshot.asks);
    return journal_detail::hash_bytes(out.bytes);
}
} // namespace apex
