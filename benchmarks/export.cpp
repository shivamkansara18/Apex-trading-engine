#include "apex/metrics/benchmark.hpp"
#include "apex/metrics/build_info.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace apex {
namespace {
std::string json(std::string_view value) {
    std::ostringstream out;
    out << '"';
    for (const char raw : value) {
        const auto character = static_cast<unsigned char>(raw);
        if (character == '"' || character == '\\') { out << '\\' << character; }
        else if (character < 32) { out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(character) << std::dec; }
        else { out << character; }
    }
    out << '"';
    return out.str();
}
std::string csv(std::string_view value) {
    std::string result = "\"";
    for (const char character : value) { result += character; if (character == '"') { result += '"'; } }
    return result + '"';
}
std::ofstream output(const std::filesystem::path& file) {
    std::ofstream out(file);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.imbue(std::locale::classic());
    out << std::setprecision(17);
    return out;
}
}
void export_benchmark(const std::filesystem::path& directory, const Workload& workload,
                      const BenchmarkReport& report, const MachineMetadata& machine) {
    if (report.runs.empty() || report.runs.size() != report.options.repetitions) {
        throw std::invalid_argument("Cannot export incomplete benchmark report");
    }
    if (!std::filesystem::create_directory(directory)) { throw std::runtime_error("Benchmark output directory must be new"); }
    save_workload(directory / "input.workload", workload);
    auto out = output(directory / "results.json");
    auto runs = output(directory / "runs.csv");
    auto samples = output(directory / "samples.csv");
    runs << "repetition,git_commit,git_dirty,source_sha256,compiler,flags,build_type,cpu,os,architecture,workload_hash,profile,seed,commands,accepted,rejects,trades,events,elapsed_ns,throughput_ops_s,accepted_ops_s,samples,min_ns,p50_ns,p95_ns,p99_ns,p999_ns,max_ns\n";
    samples << "repetition,command_index,latency_ns\n";
    const auto& c = workload.config;
    std::vector<double> throughput, accepted, p50, p95, p99, p999;
    for (const auto& run : report.runs) {
        throughput.push_back(run.throughput_ops_s); p50.push_back(static_cast<double>(run.latency.p50_ns)); p99.push_back(static_cast<double>(run.latency.p99_ns));
        accepted.push_back(run.accepted_ops_s); p95.push_back(static_cast<double>(run.latency.p95_ns)); p999.push_back(static_cast<double>(run.latency.p999_ns));
    }
    out << "{\n  \"schema_version\": 1,\n  \"timestamp_utc\": " << json(machine.timestamp_utc)
        << ",\n  \"build\": {\"type\": " << json(build_info::build_type) << ", \"compiler\": " << json(build_info::compiler)
        << ", \"compiler_path\": " << json(build_info::compiler_path) << ", \"flags\": " << json(build_info::flags)
        << ", \"sanitized\": " << (build_info::sanitized ? "true" : "false")
        << ", \"git_commit\": " << json(build_info::git_commit) << ", \"git_dirty\": " << (build_info::git_dirty ? "true" : "false")
        << ", \"source_sha256\": " << json(build_info::source_sha256) << "},\n"
        << "  \"machine\": {\"os\": " << json(machine.os) << ", \"kernel\": " << json(machine.kernel)
        << ", \"architecture\": " << json(machine.architecture) << ", \"cpu\": " << json(machine.cpu)
        << ", \"logical_cpus\": " << machine.logical_cpus << ", \"ram_bytes\": " << machine.ram_bytes
        << ", \"governor\": " << json(machine.governor) << ", \"affinity\": " << json(machine.affinity)
        << ", \"process_peak_rss_bytes\": " << machine.process_peak_rss_bytes << "},\n"
        << "  \"engine\": {\"price_index\": " << json(report.options.engine.price_index == PriceIndex::Tree ? "tree" : "flat")
        << ", \"risk_traversal\": " << json(report.options.engine.risk_traversal == RiskTraversal::Snapshot ? "snapshot" : "direct")
        << ", \"event_reservation\": " << json(report.options.engine.event_reservation == EventReservation::WorstCase ? "worst-case" : "small") << "},\n"
        << "  \"measurement\": {\"clock\": \"std::chrono::steady_clock\", \"clock_pair_median_ns\": " << machine.clock_pair_median_ns
        << ", \"clock_pair_min_positive_ns\": " << machine.clock_pair_min_positive_ns
        << ", \"clock_pair_zero_count\": " << machine.clock_pair_zero_count << ", \"clock_pair_samples\": 1000"
        << ", \"clock_overhead_subtracted\": false, \"scope\": \"synchronous process() including validation/risk/matching/event construction; no queue/network/journal\","
        << " \"throughput_scope\": \"separate bulk pass including dispatch, loop and event destruction\","
        << " \"latency_scope\": \"separate instrumented pass; excludes result accounting/hash/destruction\","
        << " \"load_model\": \"closed loop, unpaced, schedule ignored\", \"percentile_method\": \"nearest rank ceil(p*N)\","
        << " \"warmup_strategy\": \"complete discarded passes; measured passes start with fresh empty registered books\","
        << " \"warmup_runs\": " << report.options.warmup_runs << ", \"repetitions\": " << report.options.repetitions
        << ", \"p999_min_samples\": 100000},\n"
        << "  \"workload\": {\"hash_fnv1a64\": " << json(std::to_string(report.workload_hash)) << ", \"profile\": " << json(profile_name(c.profile))
        << ", \"seed\": " << c.seed << ", \"commands\": " << c.commands << ", \"symbols\": " << c.symbols << ", \"participants\": " << c.participants
        << ", \"center\": " << c.center_price << ", \"width\": " << c.price_width << ", \"min_quantity\": " << c.min_quantity << ", \"max_quantity\": " << c.max_quantity
        << ", \"cancel_percent\": " << c.cancel_percent << ", \"replace_percent\": " << c.replace_percent << ", \"market_percent\": " << c.market_percent
        << ", \"logical_rate\": " << c.commands_per_second << ", \"burst_size\": " << c.burst_size
        << ", \"distribution\": " << json(c.distribution == PriceDistribution::Uniform ? "uniform" : "triangular") << "},\n"
        << "  \"summary\": {\"median_throughput_ops_s\": " << median(throughput) << ", \"min_throughput_ops_s\": " << *std::min_element(throughput.begin(), throughput.end())
        << ", \"max_throughput_ops_s\": " << *std::max_element(throughput.begin(), throughput.end()) << ", \"median_run_p50_ns\": " << median(p50)
        << ", \"median_accepted_ops_s\": " << median(accepted) << ", \"median_run_p95_ns\": " << median(p95)
        << ", \"median_run_p99_ns\": " << median(p99) << ", \"median_run_p999_ns\": ";
    if (workload.entries.size() >= 100'000) { out << median(p999); } else { out << "null"; }
    out << "},\n  \"runs\": [\n";
    for (std::size_t index = 0; index < report.runs.size(); ++index) {
        const auto& run = report.runs[index];
        const auto& l = run.latency;
        const auto tail = l.count >= 100'000 ? std::to_string(l.p999_ns) : "null";
        out << "    {\"repetition\": " << run.repetition << ", \"commands\": " << run.commands << ", \"rejects\": " << run.rejects
            << ", \"trades\": " << run.trades << ", \"events\": " << run.events << ", \"elapsed_ns\": " << run.throughput_elapsed_ns
            << ", \"throughput_ops_s\": " << run.throughput_ops_s << ", \"accepted_ops_s\": " << run.accepted_ops_s
            << ", \"samples\": " << l.count << ", \"min_ns\": " << l.min_ns << ", \"p50_ns\": " << l.p50_ns << ", \"p95_ns\": " << l.p95_ns
            << ", \"p99_ns\": " << l.p99_ns << ", \"p999_ns\": " << tail << ", \"max_ns\": " << l.max_ns
            << ", \"event_hash\": " << json(std::to_string(run.event_hash)) << ", \"book_hashes\": [";
        for (std::size_t symbol = 0; symbol < run.final_book_hashes.size(); ++symbol) {
            if (symbol) { out << ','; } out << json(std::to_string(run.final_book_hashes[symbol]));
        }
        out << "]}" << (index + 1 < report.runs.size() ? "," : "") << '\n';
        runs << run.repetition << ',' << csv(build_info::git_commit) << ',' << build_info::git_dirty << ',' << build_info::source_sha256 << ','
             << csv(build_info::compiler) << ',' << csv(build_info::flags) << ',' << csv(build_info::build_type) << ',' << csv(machine.cpu) << ',' << csv(machine.os) << ','
             << csv(machine.architecture) << ',' << report.workload_hash << ',' << profile_name(c.profile) << ',' << c.seed << ',' << run.commands << ','
             << run.commands - run.rejects << ',' << run.rejects << ',' << run.trades << ',' << run.events << ',' << run.throughput_elapsed_ns << ','
             << run.throughput_ops_s << ',' << run.accepted_ops_s << ',' << l.count << ',' << l.min_ns << ',' << l.p50_ns << ',' << l.p95_ns << ',' << l.p99_ns << ',';
        if (l.count >= 100'000) { runs << l.p999_ns; }
        runs << ',' << l.max_ns << '\n';
        for (std::size_t command = 0; command < run.latency_samples_ns.size(); ++command) {
            samples << run.repetition << ',' << command << ',' << run.latency_samples_ns[command] << '\n';
        }
    }
    out << "  ]\n}\n";
    out.close(); runs.close(); samples.close();
}
} // namespace apex
