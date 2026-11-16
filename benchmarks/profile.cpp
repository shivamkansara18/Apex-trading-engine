#include "apex/simulation/workload.hpp"

#include <charconv>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unistd.h>

int main(int argc, char* argv[]) try {
    if (argc != 3 && argc != 4) { throw std::invalid_argument("Usage: apex_profile SAVED_WORKLOAD SECONDS [baseline|default]"); }
    const std::string_view policy = argc == 4 ? argv[3] : "baseline";
    if (policy != "baseline" && policy != "default") { throw std::invalid_argument("Profile policy must be baseline or default"); }
    const auto options = policy == "baseline" ? apex::baseline_engine_options() : apex::EngineOptions{};
    const std::string_view text = argv[2];
    unsigned seconds{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (error != std::errc{} || end != text.data() + text.size() || seconds == 0 || seconds > 60) {
        throw std::invalid_argument("Profile duration must be 1..60 seconds");
    }
    const auto workload = apex::load_workload(argv[1]);
    if (workload.entries.empty()) { throw std::invalid_argument("Profile requires commands"); }
    std::cout << "Profiling PID=" << ::getpid() << " policy=" << policy << "; repeated fresh-book passes, no latency hashing.\n" << std::flush;
    const auto stop = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::uint64_t passes = 0;
    do {
        std::vector<std::unique_ptr<apex::MatchingEngine>> engines;
        for (std::uint32_t symbol = 0; symbol < workload.config.symbols; ++symbol) {
            auto engine = std::make_unique<apex::MatchingEngine>(options);
            for (apex::ParticipantId id = 1; id <= workload.config.participants; ++id) {
                if (!engine->register_participant(id)) { throw std::logic_error("Registration failed"); }
            }
            engines.push_back(std::move(engine));
        }
        for (const auto& entry : workload.entries) { (void)engines[entry.symbol]->process(entry.command); }
        ++passes;
    } while (std::chrono::steady_clock::now() < stop);
    std::cout << "Completed passes=" << passes << " (profiling run, not benchmark evidence).\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Profile error: " << error.what() << '\n';
    return 1;
}
