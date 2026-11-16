#pragma once

#include "apex/simulation/workload.hpp"

#include <string>

namespace apex {
struct LatencySummary {
    std::uint64_t count{};
    std::uint64_t min_ns{};
    std::uint64_t p50_ns{};
    std::uint64_t p95_ns{};
    std::uint64_t p99_ns{};
    std::uint64_t p999_ns{};
    std::uint64_t max_ns{};
};
// Nearest-rank quantiles on a sorted copy, with ranks ceil(p * N).
[[nodiscard]] LatencySummary summarize_latency(const std::vector<std::uint64_t>& samples);
[[nodiscard]] double median(std::vector<double> values);

struct BenchmarkOptions {
    std::uint32_t repetitions{5};
    std::uint32_t warmup_runs{1};
    EngineOptions engine{};
};
struct BenchmarkRun {
    std::uint32_t repetition{};
    std::uint64_t commands{};
    std::uint64_t rejects{};
    std::uint64_t trades{};
    std::uint64_t events{};
    std::uint64_t throughput_elapsed_ns{};
    double throughput_ops_s{};
    double accepted_ops_s{};
    LatencySummary latency;
    std::vector<std::uint64_t> latency_samples_ns;
    std::vector<std::uint64_t> final_book_hashes;
    std::uint64_t event_hash{};
};
struct BenchmarkReport {
    BenchmarkOptions options;
    std::uint64_t workload_hash{};
    std::vector<BenchmarkRun> runs;
};
struct MachineMetadata {
    std::string timestamp_utc;
    std::string os;
    std::string kernel;
    std::string architecture;
    std::string cpu;
    std::string governor;
    std::string affinity;
    std::uint64_t ram_bytes{};
    std::uint32_t logical_cpus{};
    std::uint64_t process_peak_rss_bytes{};
    std::uint64_t clock_pair_median_ns{};
    std::uint64_t clock_pair_min_positive_ns{};
    std::uint64_t clock_pair_zero_count{};
};
[[nodiscard]] MachineMetadata machine_metadata();
[[nodiscard]] BenchmarkReport benchmark_workload(const Workload& workload, BenchmarkOptions options = {});
// New directory only. Includes input.workload, results.json, runs.csv and samples.csv.
void export_benchmark(const std::filesystem::path& directory, const Workload& workload,
                      const BenchmarkReport& report, const MachineMetadata& machine);
} // namespace apex
