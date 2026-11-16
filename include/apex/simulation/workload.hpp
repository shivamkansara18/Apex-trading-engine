#pragma once

#include "apex/journal/journal.hpp"

#include <string_view>

namespace apex {
enum class WorkloadProfile { Balanced, HotPrice, ManyLevels, HighCancel, MarketBurst, LargePartial };
enum class PriceDistribution { Uniform, Triangular };
enum class ParticipantRole { LiquidityProvider, LiquidityTaker, RandomFlow };

struct WorkloadConfig {
    std::uint64_t seed{42};
    std::uint64_t commands{1000};
    std::uint32_t symbols{1};
    std::uint32_t participants{6}; // Per independent symbol engine.
    Price center_price{10'000};
    Price price_width{20};
    Quantity min_quantity{1};
    Quantity max_quantity{100};
    std::uint32_t cancel_percent{20};
    std::uint32_t replace_percent{10};
    std::uint32_t market_percent{30}; // Among random-flow new orders.
    std::uint64_t commands_per_second{10'000}; // Logical schedule only.
    std::uint32_t burst_size{1};
    PriceDistribution distribution{PriceDistribution::Uniform};
    WorkloadProfile profile{WorkloadProfile::Balanced};
    bool operator==(const WorkloadConfig&) const = default;
};

struct ScheduledCommand {
    TimestampNs scheduled_ns{};
    std::uint32_t symbol{}; // Zero based; routes to a distinct engine.
    Command command;
    bool operator==(const ScheduledCommand&) const = default;
};
struct Workload {
    WorkloadConfig config;
    std::vector<ScheduledCommand> entries;
    bool operator==(const Workload&) const = default;
};
struct WorkloadResult {
    std::uint64_t commands{};
    std::uint64_t rejected{};
    std::uint64_t events{};
    std::uint64_t trades{};
    Quantity executed_quantity{};
    std::uint64_t event_hash{14695981039346656037ULL};
    std::vector<BookSnapshot> final_books; // Indexed by symbol.
    bool operator==(const WorkloadResult&) const = default;
};

[[nodiscard]] std::string_view profile_name(WorkloadProfile profile);
[[nodiscard]] WorkloadProfile parse_profile(std::string_view name);
[[nodiscard]] WorkloadConfig workload_preset(WorkloadProfile profile);
[[nodiscard]] ParticipantRole participant_role(ParticipantId id);
void validate_workload_config(const WorkloadConfig& config);
void validate_workload(const Workload& workload);
[[nodiscard]] Workload generate_workload(const WorkloadConfig& config);
void save_workload(const std::filesystem::path& path, const Workload& workload);
[[nodiscard]] Workload load_workload(const std::filesystem::path& path);
// Execute as fast as possible; no sleeps or performance measurement. Optional
// journaling currently supports one symbol and uses the Phase 5 wrapper.
[[nodiscard]] WorkloadResult run_workload(const Workload& workload,
                                         const std::filesystem::path& journal_path = {});
} // namespace apex
