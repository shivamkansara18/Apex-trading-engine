#pragma once

#include "apex/book/order_book.hpp"
#include "apex/core/event.hpp"
#include "apex/risk/risk_manager.hpp"

namespace apex {
// Single writer; synchronous output only, no clock, socket or file I/O.
class MatchingEngine {
public:
    explicit MatchingEngine(EngineOptions options = {});
    [[nodiscard]] bool register_participant(ParticipantId id, RiskLimits limits = {}) {
        return risk_.register_participant(id, limits);
    }
    [[nodiscard]] bool configure_participant(ParticipantId id, RiskLimits limits) {
        return risk_.configure_participant(id, limits);
    }
    void set_trading_enabled(bool enabled) noexcept { risk_.set_trading_enabled(enabled); }
    [[nodiscard]] const RiskManager& risk() const noexcept { return risk_; }
    [[nodiscard]] RiskUsage risk_usage(ParticipantId id) const { return risk_.usage(id, book_); }
    [[nodiscard]] std::vector<Event> process(const Command& command);
    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] BookSnapshot snapshot() const { return book_.snapshot(command_sequence_); }

private:
    [[nodiscard]] std::optional<RejectReason> validate(const NewOrder& request,
                                                    Quantity released = 0) const;
    void apply(const NewOrder& request, std::vector<Event>& events);
    void apply(const CancelOrder& request, std::vector<Event>& events);
    void apply(const ReplaceOrder& request, std::vector<Event>& events);
    void match(const NewOrder& request, std::vector<Event>& events);
    void emit(std::vector<Event>& events, EventPayload payload);
    void reject(std::vector<Event>& events, const Command& command, RejectReason reason);
    OrderBook book_;
    RiskManager risk_;
    EngineOptions options_;
    CommandSequence command_sequence_{};
    EventSequence event_sequence_{};
    TradeId trade_id_{};
};
} // namespace apex
