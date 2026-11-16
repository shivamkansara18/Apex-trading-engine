#pragma once

#include <cstdint>

namespace apex {
using OrderId = std::uint64_t;
using ParticipantId = std::uint32_t;
using Price = std::int64_t;
using Quantity = std::uint64_t;
using CommandSequence = std::uint64_t;
using EventSequence = std::uint64_t;
using TradeId = std::uint64_t;
using TimestampNs = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };
enum class CommandType : std::uint8_t { NewOrder, CancelOrder, ReplaceOrder };
enum class OrderStatus : std::uint8_t {
    PendingValidation, Accepted, Resting, PartiallyFilled, Filled, Cancelled, Rejected
};
enum class RejectReason : std::uint8_t {
    InvalidSide, InvalidOrderType, ZeroQuantity, InvalidPrice, DuplicateOrderId,
    UnknownOrder, WrongParticipant, UnknownParticipant, QuantityLimit,
    NotionalLimit, OpenOrderLimit, PriceBand, ArithmeticOverflow,
    InvalidReplacement, TradingDisabled
};
enum class CancelReason : std::uint8_t {
    ClientRequest, MarketRemainder, SelfTradePrevention, Replaced
};
} // namespace apex
