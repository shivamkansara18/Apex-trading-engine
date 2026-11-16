#include "workload_cli.hpp"
#include "apex/simulation/workload.hpp"

#include <charconv>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
template<class T> T numeric(std::string_view text) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::invalid_argument("Invalid workload option integer: " + std::string(text));
    }
    return value;
}
void print_result(const apex::WorkloadResult& result) {
    std::cout << "Commands=" << result.commands << " rejected=" << result.rejected
              << " trades=" << result.trades << " executed_quantity=" << result.executed_quantity
              << " events=" << result.events << '\n';
    std::cout << "Event hash: " << std::hex << result.event_hash << std::dec << '\n';
    for (std::size_t symbol = 0; symbol < result.final_books.size(); ++symbol) {
        const auto& book = result.final_books[symbol];
        std::cout << "Symbol " << symbol << " bid_levels=" << book.bids.size()
                  << " ask_levels=" << book.asks.size() << " book_hash=" << std::hex
                  << apex::book_hash(book) << std::dec << '\n';
    }
    std::cout << "Logical schedule only; no latency or throughput measured.\n";
}
int demo() {
    auto pattern = (std::filesystem::temp_directory_path() / "apex-workload-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) { throw std::runtime_error("Cannot create workload demo directory"); }
    const auto file = std::filesystem::path(pattern) / "balanced.workload";
    auto config = apex::workload_preset(apex::WorkloadProfile::Balanced);
    config.commands = 300;
    const auto generated = apex::generate_workload(config);
    if (generated != apex::generate_workload(config)) { throw std::logic_error("Generator differs for same seed"); }
    apex::save_workload(file, generated);
    const auto loaded = apex::load_workload(file);
    if (loaded != generated) { throw std::logic_error("Saved workload differs"); }
    const auto first = apex::run_workload(loaded);
    if (first != apex::run_workload(loaded)) { throw std::logic_error("Workload run differs"); }
    std::cout << "Workload: " << file << '\n';
    print_result(first);
    std::cout << "Workload verified: same seed, saved inputs, events and final books match.\n";
    return 0;
}
}

int run_workload_cli(int argc, char* argv[]) {
    const std::string_view mode = argv[1];
    if (mode == "--workload-demo" && argc == 2) { return demo(); }
    if (mode == "--run-workload" && (argc == 3 || (argc == 5 && std::string_view(argv[3]) == "--journal"))) {
        const auto workload = apex::load_workload(argv[2]);
        print_result(apex::run_workload(workload, argc == 5 ? argv[4] : ""));
        if (argc == 5) { std::cout << "Sealed journal: " << argv[4] << '\n'; }
        return 0;
    }
    if (mode == "--generate-workload" && argc >= 3 && (argc - 3) % 2 == 0) {
        auto profile = apex::WorkloadProfile::Balanced;
        for (int i = 3; i < argc; i += 2) {
            if (std::string_view(argv[i]) == "--profile") { profile = apex::parse_profile(argv[i + 1]); }
        }
        auto config = apex::workload_preset(profile);
        std::set<std::string_view> used;
        for (int i = 3; i < argc; i += 2) {
            const std::string_view key = argv[i];
            const std::string_view value = argv[i + 1];
            if (!used.insert(key).second) { throw std::invalid_argument("Duplicate workload option: " + std::string(key)); }
            if (key == "--profile") { continue; }
            if (key == "--seed") { config.seed = numeric<std::uint64_t>(value); }
            else if (key == "--commands") { config.commands = numeric<std::uint64_t>(value); }
            else if (key == "--symbols") { config.symbols = numeric<std::uint32_t>(value); }
            else if (key == "--participants") { config.participants = numeric<std::uint32_t>(value); }
            else if (key == "--center") { config.center_price = numeric<apex::Price>(value); }
            else if (key == "--width") { config.price_width = numeric<apex::Price>(value); }
            else if (key == "--min-quantity") { config.min_quantity = numeric<apex::Quantity>(value); }
            else if (key == "--max-quantity") { config.max_quantity = numeric<apex::Quantity>(value); }
            else if (key == "--cancel-percent") { config.cancel_percent = numeric<std::uint32_t>(value); }
            else if (key == "--replace-percent") { config.replace_percent = numeric<std::uint32_t>(value); }
            else if (key == "--market-percent") { config.market_percent = numeric<std::uint32_t>(value); }
            else if (key == "--rate") { config.commands_per_second = numeric<std::uint64_t>(value); }
            else if (key == "--burst-size") { config.burst_size = numeric<std::uint32_t>(value); }
            else if (key == "--distribution") {
                if (value != "uniform" && value != "triangular") { throw std::invalid_argument("Distribution must be uniform or triangular"); }
                config.distribution = value == "uniform" ? apex::PriceDistribution::Uniform : apex::PriceDistribution::Triangular;
            } else { throw std::invalid_argument("Unknown workload option: " + std::string(key)); }
        }
        apex::save_workload(argv[2], apex::generate_workload(config));
        std::cout << "Saved " << config.commands << " commands; seed=" << config.seed
                  << " profile=" << apex::profile_name(config.profile) << " symbols=" << config.symbols
                  << " to " << argv[2] << '\n';
        return 0;
    }
    throw std::invalid_argument("Usage: --workload-demo | --generate-workload NEW_FILE [--option value ...] | "
                                "--run-workload FILE [--journal NEW_FILE]");
}
