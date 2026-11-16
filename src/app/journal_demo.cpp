#include "journal_demo.hpp"
#include "apex/replay/replay.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
void print_report(const apex::ReplayResult& result) {
    std::cout << "Verified records=" << result.records << " commands=" << result.commands
              << " events=" << result.events << '\n'
              << "Book hash: " << std::hex << result.state_hash << '\n'
              << "Event hash: " << result.event_hash << std::dec << '\n';
    for (const auto& level : result.final_book.asks) {
        std::cout << "ASK " << level.price_ticks << " quantity=" << level.total_quantity << '\n';
    }
    for (const auto& level : result.final_book.bids) {
        std::cout << "BID " << level.price_ticks << " quantity=" << level.total_quantity << '\n';
    }
}
}

int run_replay(const std::filesystem::path& path) {
    print_report(apex::replay(path));
    return 0;
}
int run_journal_demo(const std::filesystem::path& requested_path) {
    auto path = requested_path;
    if (path.empty()) {
        auto pattern = (std::filesystem::temp_directory_path() / "apex-demo-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) { throw std::runtime_error("Cannot create demo directory"); }
        path = std::filesystem::path(pattern) / "session.apex";
    }
    apex::BookSnapshot expected;
    {
        apex::JournaledEngine session(path);
        for (apex::ParticipantId id = 1; id <= 3; ++id) {
            if (!session.register_participant(id)) { throw std::logic_error("Demo registration failed"); }
        }
        const apex::Command commands[] = {
            apex::NewOrder{1, 1, apex::Side::Buy, apex::OrderType::Limit, 10'000, 100, 0},
            apex::NewOrder{2, 2, apex::Side::Buy, apex::OrderType::Limit, 10'000, 50, 0},
            apex::NewOrder{3, 3, apex::Side::Sell, apex::OrderType::Limit, 9'900, 120, 0},
            apex::ReplaceOrder{2, 4, 2, 10'100, 40}};
        for (const auto& command : commands) { (void)session.process(command); }
        session.set_trading_enabled(false);
        (void)session.process(apex::NewOrder{5, 1, apex::Side::Buy, apex::OrderType::Limit, 10'000, 10, 0});
        session.set_trading_enabled(true);
        expected = session.engine().snapshot();
        session.finish();
    }
    const auto first = apex::replay(path);
    const auto second = apex::replay(path);
    if (first != second || first.final_book != expected) { throw std::runtime_error("Demo replay differs"); }
    std::cout << "Journal: " << path << '\n';
    print_report(first);
    std::cout << "Replay verified twice; events and final book match.\n";
    return 0;
}
