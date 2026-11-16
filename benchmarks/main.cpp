#include "apex/metrics/benchmark.hpp"
#include "apex/metrics/build_info.hpp"

#include <charconv>
#include <iostream>
#include <set>
#include <stdexcept>

int main(int argc, char* argv[]) try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: apex_benchmark --workload FILE --output NEW_DIRECTORY [--repetitions 5] [--warmup 1] "
                     "[--price-index tree|flat] [--risk-traversal snapshot|direct] [--event-reservation worst-case|small]\n";
        return 0;
    }
    if (argc < 5 || (argc - 1) % 2 != 0) { throw std::invalid_argument("Use --help for benchmark options"); }
    apex::BenchmarkOptions options;
    std::filesystem::path workload_file, directory;
    std::set<std::string_view> used;
    for (int index = 1; index < argc; index += 2) {
        const std::string_view key = argv[index];
        const std::string_view value = argv[index + 1];
        if (!used.insert(key).second) { throw std::invalid_argument("Duplicate benchmark option"); }
        if (key == "--workload") { workload_file = value; }
        else if (key == "--output") { directory = value; }
        else if (key == "--price-index") {
            if (value != "tree" && value != "flat") { throw std::invalid_argument("Price index must be tree or flat"); }
            options.engine.price_index = value == "tree" ? apex::PriceIndex::Tree : apex::PriceIndex::Flat;
        } else if (key == "--risk-traversal") {
            if (value != "snapshot" && value != "direct") { throw std::invalid_argument("Risk traversal must be snapshot or direct"); }
            options.engine.risk_traversal = value == "snapshot" ? apex::RiskTraversal::Snapshot : apex::RiskTraversal::Direct;
        } else if (key == "--event-reservation") {
            if (value != "worst-case" && value != "small") { throw std::invalid_argument("Event reservation must be worst-case or small"); }
            options.engine.event_reservation = value == "worst-case" ? apex::EventReservation::WorstCase : apex::EventReservation::SmallInitial;
        }
        else if (key == "--repetitions" || key == "--warmup") {
            std::uint32_t number{};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (error != std::errc{} || end != value.data() + value.size()) { throw std::invalid_argument("Invalid benchmark integer"); }
            if (key == "--repetitions") { options.repetitions = number; } else { options.warmup_runs = number; }
        } else { throw std::invalid_argument("Unknown benchmark option: " + std::string(key)); }
    }
    if (workload_file.empty() || directory.empty() || std::filesystem::exists(directory)) {
        throw std::invalid_argument("Specify workload and a new output directory");
    }
    if (std::string_view(apex::build_info::build_type) != "Release" || apex::build_info::sanitized) {
        throw std::runtime_error("Measurements require an unsanitized Release build");
    }
    const auto workload = apex::load_workload(workload_file);
    std::cout << "Benchmarking " << workload.entries.size() << " commands, " << options.repetitions
              << " repetitions; synchronous engine scope, no network/journal.\n";
    const auto report = apex::benchmark_workload(workload, options);
    const auto machine = apex::machine_metadata();
    apex::export_benchmark(directory, workload, report, machine);
    std::vector<double> throughput, p50, p99;
    for (const auto& run : report.runs) {
        throughput.push_back(run.throughput_ops_s);
        p50.push_back(static_cast<double>(run.latency.p50_ns));
        p99.push_back(static_cast<double>(run.latency.p99_ns));
    }
    std::cout << "Median throughput: " << apex::median(throughput) << " commands/s\n"
              << "Median run p50: " << apex::median(p50) << " ns; p99: " << apex::median(p99) << " ns\n"
              << "CPU: " << machine.cpu << " (" << machine.architecture << ")\n"
              << "Results: " << directory << '\n';
    if (workload.entries.size() < 100'000) {
        std::cout << "p99.9 omitted: fewer than 100,000 samples per run.\n";
    }
    if (options.repetitions < 5) { std::cout << "Fewer than 5 repetitions: treat this as a smoke run.\n"; }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Benchmark error: " << error.what() << '\n';
    return 1;
}
