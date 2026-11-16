#include "apex/metrics/benchmark.hpp"
#include "apex/metrics/build_info.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <iterator>
#include <numeric>
#include <stdexcept>
#include <unistd.h>

TEST(BenchmarkStatistics, NearestRankUsesCeilingWithoutInterpolation) {
    std::vector<std::uint64_t> samples(1000);
    std::iota(samples.begin(), samples.end(), 1);
    const auto result = apex::summarize_latency(samples);
    EXPECT_EQ(result.count, 1000U);
    EXPECT_EQ(result.min_ns, 1U);
    EXPECT_EQ(result.p50_ns, 500U);
    EXPECT_EQ(result.p95_ns, 950U);
    EXPECT_EQ(result.p99_ns, 990U);
    EXPECT_EQ(result.p999_ns, 999U);
    EXPECT_EQ(result.max_ns, 1000U);
    const auto small = apex::summarize_latency({10, 0, 30});
    EXPECT_EQ(small.min_ns, 0U);
    EXPECT_EQ(small.p50_ns, 10U);
    EXPECT_EQ(small.p95_ns, 30U);
    EXPECT_EQ(small.p999_ns, 30U);
    EXPECT_THROW((void)apex::summarize_latency({}), std::invalid_argument);
}

TEST(BenchmarkStatistics, MedianIncludesBothMiddleValuesForEvenCounts) {
    EXPECT_DOUBLE_EQ(apex::median({3, 1, 2}), 2);
    EXPECT_DOUBLE_EQ(apex::median({8, 2, 4, 6}), 5);
    EXPECT_THROW((void)apex::median({}), std::invalid_argument);
}

TEST(BenchmarkHarness, IndependentPassesMatchReferenceWithoutReusingOrderIds) {
    apex::WorkloadConfig config;
    config.commands = 200;
    config.symbols = 2;
    const auto workload = apex::generate_workload(config);
    const auto reference = apex::run_workload(workload);
    const auto report = apex::benchmark_workload(workload, {2, 1});
    ASSERT_EQ(report.runs.size(), 2U);
    for (const auto& run : report.runs) {
        EXPECT_EQ(run.commands, 200U);
        EXPECT_EQ(run.rejects, reference.rejected);
        EXPECT_EQ(run.trades, reference.trades);
        EXPECT_EQ(run.events, reference.events);
        EXPECT_EQ(run.event_hash, reference.event_hash);
        EXPECT_EQ(run.latency_samples_ns.size(), 200U);
        EXPECT_GT(run.throughput_elapsed_ns, 0U);
        EXPECT_TRUE(std::isfinite(run.throughput_ops_s));
        EXPECT_GT(run.throughput_ops_s, 0);
        ASSERT_EQ(run.final_book_hashes.size(), 2U);
        EXPECT_EQ(run.final_book_hashes[0], apex::book_hash(reference.final_books[0]));
        EXPECT_EQ(run.final_book_hashes[1], apex::book_hash(reference.final_books[1]));
    }
    EXPECT_EQ(report.runs[0].event_hash, report.runs[1].event_hash);
    const auto same = apex::benchmark_workload(workload, {1, 1});
    EXPECT_EQ(same.workload_hash, report.workload_hash);
    auto other = workload;
    ++other.config.seed;
    EXPECT_NE(apex::benchmark_workload(other, {1, 1}).workload_hash, report.workload_hash);
}

TEST(BenchmarkHarness, RejectedCommandsAreCountedAndExcludedFromAcceptedThroughput) {
    apex::Workload workload;
    workload.config.commands = 1;
    workload.entries = {{0, 0, apex::NewOrder{1, 1, apex::Side::Buy, apex::OrderType::Limit, 100, 0, 0}}};
    const auto result = apex::benchmark_workload(workload, {1, 1});
    EXPECT_EQ(result.runs[0].rejects, 1U);
    EXPECT_DOUBLE_EQ(result.runs[0].accepted_ops_s, 0);
    EXPECT_EQ(result.runs[0].latency.count, 1U);
}

TEST(BenchmarkHarness, InvalidOptionsAndEmptyInputFailBeforeTiming) {
    apex::WorkloadConfig config;
    config.commands = 10;
    const auto workload = apex::generate_workload(config);
    for (const auto options : {apex::BenchmarkOptions{0, 1}, apex::BenchmarkOptions{31, 1},
                               apex::BenchmarkOptions{1, 0}, apex::BenchmarkOptions{1, 11}}) {
        EXPECT_THROW((void)apex::benchmark_workload(workload, options), std::invalid_argument);
    }
    config.commands = 0;
    EXPECT_THROW((void)apex::benchmark_workload(apex::generate_workload(config)), std::invalid_argument);
}

TEST(BenchmarkMetadata, RecordsPlatformAndBuildIdentity) {
    const auto machine = apex::machine_metadata();
    EXPECT_FALSE(machine.os.empty());
    EXPECT_FALSE(machine.kernel.empty());
    EXPECT_FALSE(machine.architecture.empty());
    EXPECT_FALSE(machine.cpu.empty());
    EXPECT_EQ(machine.timestamp_utc.size(), 20U);
    EXPECT_GT(machine.logical_cpus, 0U);
    EXPECT_EQ(std::string_view(apex::build_info::source_sha256).size(), 64U);
    EXPECT_FALSE(std::string_view(apex::build_info::compiler).empty());
}

TEST(BenchmarkExport, SavesInputsRawSamplesMetadataAndSuppressesUndersampledP999) {
    auto pattern = (std::filesystem::temp_directory_path() / "apex-benchmark-test-XXXXXX").string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    const auto parent = std::filesystem::path(pattern);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{parent};
    apex::WorkloadConfig config;
    config.commands = 10;
    const auto workload = apex::generate_workload(config);
    const auto report = apex::benchmark_workload(workload, {2, 1});
    auto machine = apex::machine_metadata();
    machine.cpu = "CPU \"quoted\"\nline";
    const auto directory = parent / "results";
    apex::export_benchmark(directory, workload, report, machine);
    EXPECT_EQ(apex::load_workload(directory / "input.workload"), workload);
    std::ifstream file(directory / "results.json");
    const std::string json{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    EXPECT_NE(json.find("\"p999_ns\": null"), std::string::npos);
    EXPECT_NE(json.find("CPU \\\"quoted\\\"\\u000aline"), std::string::npos);
    std::ifstream samples(directory / "samples.csv");
    std::string line;
    std::size_t lines = 0;
    while (std::getline(samples, line)) { ++lines; }
    EXPECT_EQ(lines, 21U);
    EXPECT_THROW(apex::export_benchmark(directory, workload, report, machine), std::runtime_error);
    EXPECT_THROW(apex::export_benchmark(parent / "incomplete", workload, {}, machine), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(parent / "incomplete"));
}
