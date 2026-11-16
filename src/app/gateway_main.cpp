#include "apex/network/gateway.hpp"

#include <atomic>
#include <charconv>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
template<class T> T number(std::string_view text) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) { throw std::invalid_argument("Invalid numeric option"); }
    return value;
}
}
int main(int argc, char* argv[]) try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: apex_gateway [--port 9000] [--participants 16] [--journal NEW_FILE]\n"; return 0;
    }
    if ((argc - 1) % 2 != 0) { throw std::invalid_argument("Use --help for gateway options"); }
    apex::network::GatewayConfig config;
    for (int i = 1; i < argc; i += 2) {
        const std::string_view key = argv[i];
        if (key == "--port") { config.port = number<std::uint16_t>(argv[i + 1]); }
        else if (key == "--participants") { config.participants = number<std::uint32_t>(argv[i + 1]); }
        else if (key == "--journal") { config.journal_path = argv[i + 1]; }
        else { throw std::invalid_argument("Unknown gateway option"); }
    }
    apex::network::TcpGateway gateway(config);
    std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt);
    std::atomic<bool> done{false};
    std::exception_ptr failure;
    std::jthread writer([&](std::stop_token stop) {
        try { gateway.run(stop); } catch (...) { failure = std::current_exception(); }
        done.store(true);
    });
    std::cout << "Apex gateway listening on 127.0.0.1:" << gateway.port()
              << "; participants 1.." << config.participants << "; observer 0.\n"
              << "Single writer; orders persist after disconnect. Ctrl-C to stop.\n" << std::flush;
    while (!interrupted && !done.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    writer.request_stop(); writer.join();
    if (failure) { std::rethrow_exception(failure); }
    std::cout << "Gateway stopped; queued inputs drained" << (config.journal_path.empty() ? ".\n" : "; journal sealed.\n");
    const auto stats = gateway.stats();
    std::cout << "Frames processed=" << stats.frames_processed << " discarded=" << stats.frames_discarded
              << " inbox_high=" << stats.inbox_high_watermark << " overloads=" << stats.inbox_rejections
              << " output_overflows=" << stats.output_overflows << " deadlines=" << stats.deadline_disconnects << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Gateway error: " << error.what() << '\n'; return 1;
}
