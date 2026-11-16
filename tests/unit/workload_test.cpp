#include "apex/simulation/workload.hpp"
#include "apex/replay/replay.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace {
class Workloads : public testing::Test {
protected:
    std::filesystem::path directory;
    void SetUp() override {
        auto pattern = (std::filesystem::temp_directory_path() / "apex-workload-test-XXXXXX").string();
        ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
        directory = pattern;
    }
    void TearDown() override {
        std::error_code error;
        if (!directory.empty()) { std::filesystem::remove_all(directory, error); }
    }
};
std::string contents(const std::filesystem::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path);
    output << text;
    ASSERT_TRUE(output.good());
}
}

TEST(WorkloadGenerator, SameSeedAndConfigReproduceCommandsAndResults) {
    apex::WorkloadConfig config;
    config.commands = 300;
    config.symbols = 3;
    const auto first = apex::generate_workload(config);
    const auto second = apex::generate_workload(config);
    EXPECT_EQ(first, second);
    EXPECT_EQ(apex::run_workload(first), apex::run_workload(second));
    ++config.seed;
    EXPECT_NE(first.entries, apex::generate_workload(config).entries);
}

TEST(WorkloadGenerator, Seed42HasStableKnownCommandPrefix) {
    apex::WorkloadConfig config;
    config.commands = 4;
    const auto generated = apex::generate_workload(config);
    const std::vector<apex::ScheduledCommand> expected{
        {0, 0, apex::NewOrder{1, 5, apex::Side::Buy, apex::OrderType::Market, 0, 82, 0}},
        {100'000, 0, apex::NewOrder{2, 1, apex::Side::Buy, apex::OrderType::Limit, 9'989, 96, 100'000}},
        {200'000, 0, apex::NewOrder{3, 5, apex::Side::Sell, apex::OrderType::Market, 0, 93, 200'000}},
        {300'000, 0, apex::CancelOrder{2, 1}}};
    EXPECT_EQ(generated.entries, expected);
}

TEST(WorkloadGenerator, EveryProfileRunsWithoutRejectedCommands) {
    for (const auto profile : {apex::WorkloadProfile::Balanced, apex::WorkloadProfile::HotPrice,
             apex::WorkloadProfile::ManyLevels, apex::WorkloadProfile::HighCancel,
             apex::WorkloadProfile::MarketBurst, apex::WorkloadProfile::LargePartial}) {
        auto config = apex::workload_preset(profile);
        config.commands = 300;
        config.symbols = 2;
        const auto generated = apex::generate_workload(config);
        ASSERT_EQ(generated.entries.size(), 300U);
        const auto result = apex::run_workload(generated);
        EXPECT_EQ(result.commands, 300U) << apex::profile_name(profile);
        EXPECT_EQ(result.rejected, 0U) << apex::profile_name(profile);
        EXPECT_EQ(result.final_books.size(), 2U);
        EXPECT_EQ(profile, apex::parse_profile(apex::profile_name(profile)));
    }
}

TEST(WorkloadGenerator, RolesAndQuantityBoundsAreRespected) {
    EXPECT_EQ(apex::participant_role(1), apex::ParticipantRole::LiquidityProvider);
    EXPECT_EQ(apex::participant_role(2), apex::ParticipantRole::LiquidityTaker);
    EXPECT_EQ(apex::participant_role(3), apex::ParticipantRole::RandomFlow);
    EXPECT_THROW((void)apex::participant_role(0), std::invalid_argument);
    apex::WorkloadConfig config;
    config.commands = 500;
    config.min_quantity = 7;
    config.max_quantity = 11;
    const auto workload = apex::generate_workload(config);
    for (const auto& entry : workload.entries) {
        if (const auto* request = std::get_if<apex::NewOrder>(&entry.command)) {
            EXPECT_GE(request->quantity, 7U);
            EXPECT_LE(request->quantity, 11U);
            if (apex::participant_role(request->participant_id) == apex::ParticipantRole::LiquidityProvider) {
                EXPECT_EQ(request->type, apex::OrderType::Limit);
                EXPECT_TRUE(request->side == apex::Side::Buy ? request->price_ticks < config.center_price
                                                             : request->price_ticks > config.center_price);
            } else if (apex::participant_role(request->participant_id) == apex::ParticipantRole::LiquidityTaker) {
                EXPECT_EQ(request->type, apex::OrderType::Market);
            }
            EXPECT_EQ(request->client_timestamp_ns, entry.scheduled_ns);
        }
    }
}

TEST(WorkloadGenerator, ProvidersAddPassiveLiquidityInBalancedTraffic) {
    apex::WorkloadConfig config;
    config.commands = 500;
    const auto workload = apex::generate_workload(config);
    apex::MatchingEngine engine;
    for (apex::ParticipantId id = 1; id <= config.participants; ++id) {
        ASSERT_TRUE(engine.register_participant(id));
    }
    for (const auto& entry : workload.entries) {
        const auto events = engine.process(entry.command);
        const auto* request = std::get_if<apex::NewOrder>(&entry.command);
        if (request && apex::participant_role(request->participant_id) == apex::ParticipantRole::LiquidityProvider) {
            for (const auto& event : events) { EXPECT_FALSE(std::holds_alternative<apex::Trade>(event.payload)); }
            EXPECT_TRUE(engine.book().find(request->client_order_id));
        }
        EXPECT_TRUE(engine.book().check_invariants(true));
    }
}

TEST(WorkloadGenerator, ProfilesExerciseHotPriceCancelsBurstsAndManyLevels) {
    const auto generate = [](apex::WorkloadProfile profile) {
        auto config = apex::workload_preset(profile);
        config.commands = 1000;
        return apex::generate_workload(config);
    };
    const auto balanced = generate(apex::WorkloadProfile::Balanced);
    const auto hot = generate(apex::WorkloadProfile::HotPrice);
    for (const auto& entry : hot.entries) {
        if (const auto* request = std::get_if<apex::NewOrder>(&entry.command)) {
            if (request->type == apex::OrderType::Limit) { EXPECT_EQ(request->price_ticks, hot.config.center_price); }
        }
        if (const auto* request = std::get_if<apex::ReplaceOrder>(&entry.command)) {
            EXPECT_EQ(request->new_price_ticks, hot.config.center_price);
        }
    }
    const auto cancels = [](const apex::Workload& workload) {
        std::size_t count = 0;
        for (const auto& entry : workload.entries) { count += std::holds_alternative<apex::CancelOrder>(entry.command); }
        return count;
    };
    EXPECT_GT(cancels(generate(apex::WorkloadProfile::HighCancel)), cancels(balanced));
    const auto many = generate(apex::WorkloadProfile::ManyLevels);
    std::set<apex::Price> prices;
    for (const auto& entry : many.entries) {
        if (const auto* request = std::get_if<apex::NewOrder>(&entry.command)) {
            if (request->type == apex::OrderType::Limit) { prices.insert(request->price_ticks); }
        }
    }
    EXPECT_GT(prices.size(), 100U);
    const auto burst = generate(apex::WorkloadProfile::MarketBurst);
    EXPECT_EQ(burst.entries[0].scheduled_ns, burst.entries[24].scheduled_ns);
    EXPECT_GT(burst.entries[25].scheduled_ns, burst.entries[24].scheduled_ns);
}

TEST(WorkloadGenerator, ScriptedLargePartialConsumesSixteenOrdersAcrossLevels) {
    auto config = apex::workload_preset(apex::WorkloadProfile::LargePartial);
    config.commands = 17;
    const auto result = apex::run_workload(apex::generate_workload(config));
    EXPECT_EQ(result.trades, 16U);
    EXPECT_EQ(result.executed_quantity, 16U);
    EXPECT_TRUE(result.final_books[0].bids.empty());
    EXPECT_TRUE(result.final_books[0].asks.empty());
}

TEST(WorkloadGenerator, ScheduleUsesExactRationalRateAndBatchesWithoutSleeping) {
    apex::WorkloadConfig config;
    config.commands = 10;
    config.commands_per_second = 3;
    config.burst_size = 3;
    const auto burst = apex::generate_workload(config);
    EXPECT_EQ(burst.entries[0].scheduled_ns, 0U);
    EXPECT_EQ(burst.entries[2].scheduled_ns, 0U);
    EXPECT_EQ(burst.entries[3].scheduled_ns, 1'000'000'000U);
    EXPECT_EQ(burst.entries[9].scheduled_ns, 3'000'000'000U);
    config.burst_size = 1;
    const auto uniform = apex::generate_workload(config);
    EXPECT_EQ(uniform.entries[1].scheduled_ns, 333'333'333U);
    EXPECT_EQ(uniform.entries[3].scheduled_ns, 1'000'000'000U);
}

TEST(WorkloadGenerator, TriangularPricesAreReproducibleAndDifferFromUniform) {
    apex::WorkloadConfig config;
    config.commands = 200;
    const auto uniform = apex::generate_workload(config);
    config.distribution = apex::PriceDistribution::Triangular;
    const auto first = apex::generate_workload(config);
    EXPECT_EQ(first, apex::generate_workload(config));
    EXPECT_NE(first.entries, uniform.entries);
    EXPECT_EQ(apex::run_workload(first).rejected, 0U);
}

TEST(WorkloadGenerator, InvalidConfigurationsAreRejectedBeforeAllocation) {
    const std::vector<std::function<void(apex::WorkloadConfig&)>> changes{
        [](auto& c) { c.commands = 100'001; }, [](auto& c) { c.symbols = 0; },
        [](auto& c) { c.symbols = 33; }, [](auto& c) { c.participants = 1; },
        [](auto& c) { c.symbols = 32; c.participants = 1000; },
        [](auto& c) { c.price_width = -1; }, [](auto& c) { c.center_price = c.price_width; },
        [](auto& c) { c.price_width = std::numeric_limits<apex::Price>::max(); },
        [](auto& c) { c.min_quantity = 0; }, [](auto& c) { c.max_quantity = 0; },
        [](auto& c) { c.max_quantity = 1'000'001; }, [](auto& c) { c.cancel_percent = 101; },
        [](auto& c) { c.cancel_percent = 80; c.replace_percent = 30; },
        [](auto& c) { c.market_percent = 101; }, [](auto& c) { c.commands_per_second = 0; },
        [](auto& c) { c.burst_size = 0; }, [](auto& c) { c.profile = static_cast<apex::WorkloadProfile>(99); },
        [](auto& c) { c.distribution = static_cast<apex::PriceDistribution>(99); }};
    for (const auto& change : changes) {
        apex::WorkloadConfig config;
        change(config);
        EXPECT_THROW((void)apex::generate_workload(config), std::invalid_argument);
    }
    EXPECT_THROW((void)apex::parse_profile("unknown"), std::invalid_argument);
}

TEST(WorkloadRunner, SymbolBooksAndIdsAreIsolated) {
    apex::Workload workload;
    workload.config.commands = 3;
    workload.config.symbols = 2;
    workload.entries = {
        {0, 0, apex::NewOrder{1, 1, apex::Side::Buy, apex::OrderType::Limit, 100, 10, 0}},
        {0, 1, apex::NewOrder{1, 1, apex::Side::Buy, apex::OrderType::Limit, 100, 20, 0}},
        {1, 1, apex::NewOrder{2, 2, apex::Side::Sell, apex::OrderType::Market, 0, 5, 0}}};
    const auto result = apex::run_workload(workload);
    EXPECT_EQ(result.rejected, 0U);
    EXPECT_EQ(result.trades, 1U);
    EXPECT_EQ(result.final_books[0].bids[0].total_quantity, 10U);
    EXPECT_EQ(result.final_books[1].bids[0].total_quantity, 15U);
    EXPECT_EQ(result.final_books[0].last_command_sequence, 1U);
    EXPECT_EQ(result.final_books[1].last_command_sequence, 2U);
}

TEST(WorkloadRunner, EmptyWorkloadsAndScriptedRejectionsAreCounted) {
    apex::Workload workload;
    workload.config.commands = 0;
    EXPECT_EQ(apex::run_workload(workload).commands, 0U);
    workload.config.commands = 1;
    workload.entries = {{0, 0, apex::NewOrder{1, 1, apex::Side::Buy, apex::OrderType::Limit, 100, 0, 0}}};
    const auto result = apex::run_workload(workload);
    EXPECT_EQ(result.rejected, 1U);
    EXPECT_EQ(result.events, 1U);
    EXPECT_TRUE(result.final_books[0].bids.empty());
}

TEST_F(Workloads, SaveLoadIsLosslessAndRepeatedSavesAreByteIdentical) {
    apex::WorkloadConfig config;
    config.commands = 300;
    config.symbols = 2;
    config.distribution = apex::PriceDistribution::Triangular;
    const auto workload = apex::generate_workload(config);
    const auto first = directory / "first.workload";
    const auto second = directory / "second.workload";
    apex::save_workload(first, workload);
    apex::save_workload(second, apex::generate_workload(config));
    EXPECT_EQ(contents(first), contents(second));
    const auto loaded = apex::load_workload(first);
    EXPECT_EQ(loaded, workload);
    EXPECT_EQ(apex::run_workload(loaded), apex::run_workload(workload));
    EXPECT_THROW(apex::save_workload(first, workload), std::system_error);
    EXPECT_EQ(contents(first), contents(second));
}

TEST_F(Workloads, OptionalJournalMatchesUnjournaledRunAndPhaseFiveReplay) {
    apex::WorkloadConfig config;
    config.commands = 100;
    const auto workload = apex::generate_workload(config);
    const auto journal = directory / "session.apex";
    const auto recorded = apex::run_workload(workload, journal);
    EXPECT_EQ(recorded, apex::run_workload(workload));
    const auto replay = apex::replay(journal);
    EXPECT_EQ(replay.commands, recorded.commands);
    EXPECT_EQ(replay.events, recorded.events);
    EXPECT_EQ(replay.final_book, recorded.final_books[0]);
    EXPECT_EQ(replay.state_hash, apex::book_hash(recorded.final_books[0]));
    config.symbols = 2;
    const auto unsupported = directory / "multi.apex";
    EXPECT_THROW((void)apex::run_workload(apex::generate_workload(config), unsupported), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(unsupported));
}

TEST_F(Workloads, RejectsBadHeadersNumbersTagsAndTruncation) {
    apex::WorkloadConfig config;
    config.commands = 2;
    const auto valid_path = directory / "valid.workload";
    apex::save_workload(valid_path, apex::generate_workload(config));
    const auto valid = contents(valid_path);
    const auto damaged = directory / "damaged.workload";
    const auto replace_once = [&](std::string_view from, std::string_view to) {
        auto text = valid;
        const auto offset = text.find(from);
        ASSERT_NE(offset, std::string::npos);
        text.replace(offset, from.size(), to);
        write_text(damaged, text);
        EXPECT_THROW((void)apex::load_workload(damaged), std::exception);
    };
    replace_once("APEX_WORKLOAD 1", "APEX_WORKLOAD 2");
    replace_once("CONFIG 42", "CONFIG -1");
    replace_once("CONFIG 42", "CONFIG 18446744073709551616");
    replace_once("uniform", "gaussian");
    replace_once("NEW", "BAD");
    replace_once("BUY", "BAD_SIDE");
    replace_once("MARKET", "BAD_TYPE");
    write_text(damaged, valid + "GARBAGE\n");
    EXPECT_THROW((void)apex::load_workload(damaged), std::runtime_error);
    write_text(damaged, valid.substr(0, valid.find("END")));
    EXPECT_THROW((void)apex::load_workload(damaged), std::runtime_error);
    write_text(damaged, "APEX_WORKLOAD 1\nCONFIG 42");
    EXPECT_THROW((void)apex::load_workload(damaged), std::runtime_error);
    EXPECT_THROW((void)apex::load_workload(directory / "missing"), std::filesystem::filesystem_error);
    std::filesystem::resize_file(damaged, 65 * 1024 * 1024);
    EXPECT_THROW((void)apex::load_workload(damaged), std::runtime_error);
}

TEST_F(Workloads, InvalidSchedulesAndRegistriesFailBeforeCreatingOutput) {
    apex::WorkloadConfig config;
    config.commands = 2;
    const auto valid = apex::generate_workload(config);
    for (int variant = 0; variant < 4; ++variant) {
        auto invalid = valid;
        if (variant == 0) { invalid.entries.pop_back(); }
        if (variant == 1) { invalid.entries[1].symbol = 1; }
        if (variant == 2) { invalid.entries[0].scheduled_ns = 1'000'000; }
        if (variant == 3) { std::get<apex::NewOrder>(invalid.entries[0].command).participant_id = 99; }
        const auto path = directory / "not-created.workload";
        EXPECT_THROW(apex::save_workload(path, invalid), std::invalid_argument);
        EXPECT_THROW((void)apex::run_workload(invalid), std::invalid_argument);
        EXPECT_FALSE(std::filesystem::exists(path));
    }
}
