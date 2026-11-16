#include "apex/metrics/benchmark.hpp"
#include "apex/replay/replay.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <random>
#include <unistd.h>

namespace {
const std::vector<apex::EngineOptions> policies = [] {
    std::vector<apex::EngineOptions> result;
    for (const auto index : {apex::PriceIndex::Tree, apex::PriceIndex::Flat}) {
        for (const auto traversal : {apex::RiskTraversal::Snapshot, apex::RiskTraversal::Direct}) {
            for (const auto reservation : {apex::EventReservation::WorstCase, apex::EventReservation::SmallInitial}) {
                result.push_back({index, traversal, reservation});
            }
        }
    }
    return result;
}();
class OptimizationPolicies : public testing::TestWithParam<apex::EngineOptions> {};
}

TEST_P(OptimizationPolicies, MixedCommandsRiskChangesAndEventsMatchReferenceAfterEveryInput) {
    apex::MatchingEngine reference(apex::baseline_engine_options());
    apex::MatchingEngine candidate(GetParam());
    apex::RiskLimits limits;
    limits.max_order_quantity = 25;
    limits.max_order_notional = 2500;
    limits.max_open_orders = 5;
    limits.min_price_ticks = 95;
    limits.max_price_ticks = 105;
    for (apex::ParticipantId id = 1; id <= 4; ++id) {
        ASSERT_TRUE(reference.register_participant(id, limits));
        ASSERT_TRUE(candidate.register_participant(id, limits));
    }
    std::mt19937 random(789);
    for (apex::OrderId step = 1; step <= 1500; ++step) {
        if (step % 100 == 0) {
            const bool enabled = step % 200 == 0;
            reference.set_trading_enabled(enabled); candidate.set_trading_enabled(enabled);
            auto config = limits;
            config.trading_enabled = step % 300 != 0;
            EXPECT_EQ(reference.configure_participant(1, config), candidate.configure_participant(1, config));
        }
        apex::Command command = apex::NewOrder{step, 1 + random() % 5,
            random() % 2 == 0 ? apex::Side::Buy : apex::Side::Sell,
            random() % 4 == 0 ? apex::OrderType::Market : apex::OrderType::Limit,
            90 + random() % 20, random() % 35, 0};
        if (random() % 3 == 0) { command = apex::CancelOrder{random() % step, 1 + random() % 5}; }
        else if (random() % 4 == 0) {
            command = apex::ReplaceOrder{random() % step, step, 1 + random() % 5,
                                        90 + random() % 20, random() % 35};
        }
        ASSERT_EQ(reference.process(command), candidate.process(command)) << "step=" << step;
        ASSERT_EQ(reference.snapshot(), candidate.snapshot());
        ASSERT_TRUE(candidate.book().check_invariants(true));
        for (apex::ParticipantId id = 1; id <= 4; ++id) { EXPECT_EQ(reference.risk_usage(id), candidate.risk_usage(id)); }
    }
}

TEST_P(OptimizationPolicies, AllWorkloadProfilesAndLargeEventBuffersMatchBaseline) {
    for (const auto profile : {apex::WorkloadProfile::Balanced, apex::WorkloadProfile::HotPrice,
             apex::WorkloadProfile::ManyLevels, apex::WorkloadProfile::HighCancel,
             apex::WorkloadProfile::MarketBurst, apex::WorkloadProfile::LargePartial}) {
        auto config = apex::workload_preset(profile);
        config.commands = 200;
        const auto workload = apex::generate_workload(config);
        apex::MatchingEngine reference(apex::baseline_engine_options());
        apex::MatchingEngine candidate(GetParam());
        for (apex::ParticipantId id = 1; id <= config.participants; ++id) {
            ASSERT_TRUE(reference.register_participant(id)); ASSERT_TRUE(candidate.register_participant(id));
        }
        for (const auto& entry : workload.entries) {
            ASSERT_EQ(reference.process(entry.command), candidate.process(entry.command));
            ASSERT_EQ(reference.snapshot(), candidate.snapshot());
            ASSERT_TRUE(candidate.book().check_invariants(true));
        }
    }
}

TEST_P(OptimizationPolicies, JournalBytesAndReplayAreIdenticalAcrossPhysicalRepresentations) {
    auto pattern = (std::filesystem::temp_directory_path() / "apex-policy-test-XXXXXX").string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    const std::filesystem::path directory = pattern;
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(directory, error); }
    } cleanup{directory};
    apex::WorkloadConfig config;
    config.commands = 100;
    const auto workload = apex::generate_workload(config);
    const auto record = [&](const std::filesystem::path& path, apex::EngineOptions options) {
        apex::JournaledEngine session(path, options);
        for (apex::ParticipantId id = 1; id <= config.participants; ++id) { ASSERT_TRUE(session.register_participant(id)); }
        for (const auto& entry : workload.entries) { (void)session.process(entry.command); }
        session.finish();
    };
    const auto baseline = directory / "baseline.apex";
    const auto candidate = directory / "candidate.apex";
    record(baseline, apex::baseline_engine_options()); record(candidate, GetParam());
    const auto read = [](const auto& path) {
        std::ifstream input(path, std::ios::binary);
        return std::vector<char>{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    };
    EXPECT_EQ(read(baseline), read(candidate));
    EXPECT_EQ(apex::replay(baseline), apex::replay(candidate));
}

TEST_P(OptimizationPolicies, BenchmarkRetainsPolicyAndMatchesReferenceEvents) {
    apex::WorkloadConfig config;
    config.commands = 30;
    const auto workload = apex::generate_workload(config);
    const auto report = apex::benchmark_workload(workload, {1, 1, GetParam()});
    EXPECT_EQ(report.options.engine, GetParam());
    EXPECT_EQ(report.runs[0].event_hash, apex::run_workload(workload).event_hash);
}
INSTANTIATE_TEST_SUITE_P(AllEightCombinations, OptimizationPolicies, testing::ValuesIn(policies),
    [](const testing::TestParamInfo<apex::EngineOptions>& info) {
        return std::string(info.param.price_index == apex::PriceIndex::Tree ? "Tree" : "Flat") +
            (info.param.risk_traversal == apex::RiskTraversal::Snapshot ? "Snapshot" : "Direct") +
            (info.param.event_reservation == apex::EventReservation::WorstCase ? "WorstCase" : "Small");
    });

TEST(FlatPriceLevels, ReallocationAndMiddleErasurePreserveEveryIndexedOrderIterator) {
    apex::OrderBook reference(apex::PriceIndex::Tree);
    apex::OrderBook candidate(apex::PriceIndex::Flat);
    for (apex::OrderId id = 1; id <= 600; ++id) {
        const auto side = id % 2 == 0 ? apex::Side::Buy : apex::Side::Sell;
        const apex::Price price = (side == apex::Side::Buy ? 1000 : 2000) + static_cast<apex::Price>((id * 137) % 601);
        const apex::Order order{id, 1, side, apex::OrderType::Limit, price, 10, 10, id, apex::OrderStatus::Resting};
        ASSERT_EQ(reference.insert(order), candidate.insert(order));
        ASSERT_EQ(reference.snapshot(), candidate.snapshot());
        ASSERT_TRUE(candidate.check_invariants(true));
    }
    for (apex::OrderId step = 1; step <= 600; ++step) {
        const auto id = (step * 137) % 601;
        if (step % 3 == 0) {
            EXPECT_EQ(reference.fill(id, 3), candidate.fill(id, 3));
            EXPECT_EQ(reference.fill(id, 7), candidate.fill(id, 7));
        } else { EXPECT_EQ(reference.cancel(id, 1), candidate.cancel(id, 1)); }
        ASSERT_EQ(reference.snapshot(), candidate.snapshot());
        ASSERT_TRUE(candidate.check_invariants(true));
    }
    EXPECT_EQ(candidate.order_count(), 0U);
}

TEST(BookTraversal, VisitsPriceTimeOrderAndStopsWithoutCopying) {
    for (const auto index : {apex::PriceIndex::Tree, apex::PriceIndex::Flat}) {
        apex::OrderBook book(index);
        for (apex::OrderId id = 1; id <= 4; ++id) {
            const apex::Order order{id, 1, apex::Side::Buy, apex::OrderType::Limit,
                id == 1 ? 99 : 100, 10, 10, id, apex::OrderStatus::Resting};
            ASSERT_EQ(book.insert(order), apex::BookError::None);
        }
        std::vector<apex::OrderId> seen;
        book.visit_orders(apex::Side::Buy, [&](const apex::Order& order) {
            seen.push_back(order.client_order_id); return seen.size() < 2;
        });
        EXPECT_EQ(seen, (std::vector<apex::OrderId>{2, 3}));
        EXPECT_TRUE(book.check_invariants(true));
    }
}

TEST(EnginePolicies, InvalidPoliciesFailBeforeProcessing) {
    auto invalid = apex::baseline_engine_options();
    invalid.price_index = static_cast<apex::PriceIndex>(99);
    EXPECT_THROW(apex::MatchingEngine engine(invalid), std::invalid_argument);
    invalid = apex::baseline_engine_options(); invalid.risk_traversal = static_cast<apex::RiskTraversal>(99);
    EXPECT_THROW(apex::MatchingEngine engine(invalid), std::invalid_argument);
    invalid = apex::baseline_engine_options(); invalid.event_reservation = static_cast<apex::EventReservation>(99);
    EXPECT_THROW(apex::MatchingEngine engine(invalid), std::invalid_argument);
}
