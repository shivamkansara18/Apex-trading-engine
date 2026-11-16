#include "apex/metrics/benchmark.hpp"
#include "../../src/journal/codec.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>

namespace apex {
LatencySummary summarize_latency(const std::vector<std::uint64_t>& samples) {
    if (samples.empty()) { throw std::invalid_argument("Latency samples must not be empty"); }
    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&](std::size_t numerator, std::size_t denominator) {
        const auto rank = (sorted.size() / denominator) * numerator +
                          ((sorted.size() % denominator) * numerator + denominator - 1) / denominator;
        return sorted[rank - 1];
    };
    return {sorted.size(), sorted.front(), percentile(50, 100), percentile(95, 100),
            percentile(99, 100), percentile(999, 1000), sorted.back()};
}
double median(std::vector<double> values) {
    if (values.empty()) { throw std::invalid_argument("Median requires values"); }
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    return values.size() % 2 == 0 ? values[middle - 1] / 2 + values[middle] / 2 : values[middle];
}

namespace {
using Clock = std::chrono::steady_clock;
using Engines = std::vector<std::unique_ptr<MatchingEngine>>;
Engines setup(const WorkloadConfig& config, EngineOptions options) {
    Engines engines;
    for (std::uint32_t symbol = 0; symbol < config.symbols; ++symbol) {
        auto engine = std::make_unique<MatchingEngine>(options);
        for (ParticipantId id = 1; id <= config.participants; ++id) {
            if (!engine->register_participant(id)) { throw std::logic_error("Benchmark registration failed"); }
        }
        engines.push_back(std::move(engine));
    }
    return engines;
}
void verify_books(const Engines& engines, const WorkloadResult& expected) {
    for (std::size_t symbol = 0; symbol < engines.size(); ++symbol) {
        if (engines[symbol]->snapshot() != expected.final_books[symbol] ||
            !engines[symbol]->book().check_invariants(true)) {
            throw std::logic_error("Benchmark final state differs from correctness run");
        }
    }
}
std::uint64_t fingerprint(const Workload& workload) {
    const auto& c = workload.config;
    journal_detail::Encoder bytes;
    bytes.u64(c.seed); bytes.u64(c.commands); bytes.u32(c.symbols); bytes.u32(c.participants);
    bytes.price(c.center_price); bytes.price(c.price_width); bytes.u64(c.min_quantity); bytes.u64(c.max_quantity);
    bytes.u32(c.cancel_percent); bytes.u32(c.replace_percent); bytes.u32(c.market_percent);
    bytes.u64(c.commands_per_second); bytes.u32(c.burst_size);
    bytes.u8(static_cast<std::uint8_t>(c.distribution)); bytes.u8(static_cast<std::uint8_t>(c.profile));
    auto hash = journal_detail::hash_bytes(bytes.bytes);
    for (const auto& entry : workload.entries) {
        journal_detail::Encoder command;
        command.u64(entry.scheduled_ns); command.u32(entry.symbol);
        journal_detail::encode_input(command, entry.command);
        hash = journal_detail::hash_bytes(command.bytes, hash);
    }
    return hash;
}
}

BenchmarkReport benchmark_workload(const Workload& workload, BenchmarkOptions options) {
    validate_workload(workload);
    if (workload.entries.empty() || options.repetitions == 0 || options.repetitions > 30 ||
        options.warmup_runs == 0 || options.warmup_runs > 10) {
        throw std::invalid_argument("Benchmark needs nonempty input, 1..30 repetitions and 1..10 warm-up runs");
    }
    // Full correctness/reference pass and all warm-up passes are untimed and
    // discarded. Each measured pass starts with a fresh empty registered book.
    const auto expected = run_workload(workload);
    for (std::uint32_t warm = 0; warm < options.warmup_runs; ++warm) {
        auto engines = setup(workload.config, options.engine);
        for (const auto& entry : workload.entries) { (void)engines[entry.symbol]->process(entry.command); }
        verify_books(engines, expected);
    }
    BenchmarkReport report{options, fingerprint(workload), {}};
    for (std::uint32_t repetition = 1; repetition <= options.repetitions; ++repetition) {
        BenchmarkRun run;
        run.repetition = repetition;
        run.commands = workload.entries.size();
        auto bulk_engines = setup(workload.config, options.engine);
        const auto start = Clock::now();
        for (const auto& entry : workload.entries) {
            // Returned event vector destruction is included in bulk timing.
            const auto events = bulk_engines[entry.symbol]->process(entry.command);
            (void)events;
        }
        const auto stop = Clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
        if (elapsed <= 0) { throw std::runtime_error("Nonpositive benchmark duration"); }
        run.throughput_elapsed_ns = static_cast<std::uint64_t>(elapsed);
        run.throughput_ops_s = static_cast<double>(run.commands) * 1e9 / static_cast<double>(elapsed);
        run.accepted_ops_s = static_cast<double>(run.commands - expected.rejected) * 1e9 / static_cast<double>(elapsed);
        verify_books(bulk_engines, expected);

        auto engines = setup(workload.config, options.engine);
        run.latency_samples_ns.resize(workload.entries.size());
        run.event_hash = 14695981039346656037ULL;
        for (std::size_t index = 0; index < workload.entries.size(); ++index) {
            const auto& entry = workload.entries[index];
            auto& engine = *engines[entry.symbol];
            const auto before = Clock::now();
            const auto events = engine.process(entry.command);
            const auto after = Clock::now();
            const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count();
            if (duration < 0) { throw std::runtime_error("Monotonic clock moved backwards"); }
            run.latency_samples_ns[index] = static_cast<std::uint64_t>(duration);
            // Accounting, hashing and event destruction are outside each latency interval.
            run.events += events.size();
            for (const auto& event : events) {
                if (std::holds_alternative<CommandRejected>(event.payload)) { ++run.rejects; }
                if (std::holds_alternative<Trade>(event.payload)) { ++run.trades; }
            }
            journal_detail::Encoder prefix;
            prefix.u32(entry.symbol);
            run.event_hash = journal_detail::hash_bytes(prefix.bytes, run.event_hash);
            run.event_hash = journal_detail::hash_bytes(journal_detail::encode_events(events), run.event_hash);
        }
        verify_books(engines, expected);
        if (run.events != expected.events || run.rejects != expected.rejected || run.trades != expected.trades ||
            run.event_hash != expected.event_hash) {
            throw std::logic_error("Benchmark outputs differ from correctness run");
        }
        run.latency = summarize_latency(run.latency_samples_ns);
        for (const auto& snapshot : expected.final_books) { run.final_book_hashes.push_back(book_hash(snapshot)); }
        report.runs.push_back(std::move(run));
    }
    return report;
}
} // namespace apex
