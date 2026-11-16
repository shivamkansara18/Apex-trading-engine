#pragma once

#include "apex/matching/matching_engine.hpp"

#include <filesystem>
#include <variant>

namespace apex {
struct RegisterParticipant { ParticipantId id{}; RiskLimits limits; };
struct ConfigureParticipant { ParticipantId id{}; RiskLimits limits; };
struct SetTradingEnabled { bool enabled{true}; };
using JournalInput = std::variant<RegisterParticipant, ConfigureParticipant, SetTradingEnabled, Command>;

struct JournalResult {
    bool admin_result{true};
    std::vector<Event> events;
};

[[nodiscard]] std::uint64_t book_hash(const BookSnapshot& snapshot);

// New-file-only journal; all inputs pass through this single-writer facade.
// Call finish() before replay. Destruction closes but never silently seals a log.
class JournaledEngine {
public:
    explicit JournaledEngine(const std::filesystem::path& path, EngineOptions options = {});
    ~JournaledEngine();
    JournaledEngine(const JournaledEngine&) = delete;
    JournaledEngine& operator=(const JournaledEngine&) = delete;
    [[nodiscard]] JournalResult execute(const JournalInput& input);
    [[nodiscard]] std::vector<Event> process(const Command& command) { return execute(command).events; }
    [[nodiscard]] bool register_participant(ParticipantId id, RiskLimits limits = {}) {
        return execute(RegisterParticipant{id, limits}).admin_result;
    }
    [[nodiscard]] bool configure_participant(ParticipantId id, RiskLimits limits) {
        return execute(ConfigureParticipant{id, limits}).admin_result;
    }
    void set_trading_enabled(bool enabled) { (void)execute(SetTradingEnabled{enabled}); }
    void finish();
    [[nodiscard]] const MatchingEngine& engine() const noexcept { return engine_; }

private:
    void append(const std::vector<std::uint8_t>& payload);
    int fd_{-1};
    bool failed_{};
    bool finished_{};
    std::uint64_t record_sequence_{};
    MatchingEngine engine_;
};
} // namespace apex
