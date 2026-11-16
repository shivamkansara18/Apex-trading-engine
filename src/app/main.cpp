#include "apex/core/version.hpp"
#include "apex/book/order_book.hpp"
#include "demo.hpp"
#include "journal_demo.hpp"
#include "workload_cli.hpp"

#include <iostream>

int main(int argc, char* argv[]) try {
    std::cout << "Apex " << apex::version()
              << " - deterministic exchange simulator\n";
    if (argc == 1) {
        std::cout << "Core ready: matching, risk, journaling and participant workloads.\n"
                  << "Run --workload-demo, --journal-demo, --risk-demo, --trading-demo or --book-demo.\n";
        return 0;
    }
    if (argc >= 2 && (std::string_view(argv[1]) == "--workload-demo" ||
        std::string_view(argv[1]) == "--generate-workload" || std::string_view(argv[1]) == "--run-workload")) {
        return run_workload_cli(argc, argv);
    }
    if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--journal-demo") {
        return run_journal_demo(argc == 3 ? argv[2] : "");
    }
    if (argc == 3 && std::string_view(argv[1]) == "--replay") { return run_replay(argv[2]); }
    if (argc == 2 && std::string_view(argv[1]) == "--trading-demo") { return run_trading_demo(); }
    if (argc == 2 && std::string_view(argv[1]) == "--risk-demo") { return run_risk_demo(); }
    if (argc != 2 || std::string_view(argv[1]) != "--book-demo") {
        std::cerr << "Usage: apex [--book-demo | --trading-demo | --risk-demo | "
                     "--journal-demo [NEW_FILE] | --replay FILE | --workload-demo | "
                     "--generate-workload NEW_FILE [options] | --run-workload FILE [--journal NEW_FILE]]\n";
        return 1;
    }
    apex::OrderBook book;
    const apex::Order orders[] = {
        {1, 1, apex::Side::Buy, apex::OrderType::Limit, 10'000, 100, 100, 1, apex::OrderStatus::Resting},
        {2, 2, apex::Side::Buy, apex::OrderType::Limit, 10'000, 50, 50, 2, apex::OrderStatus::Resting},
        {3, 3, apex::Side::Sell, apex::OrderType::Limit, 10'100, 80, 80, 3, apex::OrderStatus::Resting}};
    for (const auto& order : orders) {
        if (book.insert(order) != apex::BookError::None) { return 1; }
    }
    std::cout << "Storage demo (integer ticks; no matching yet)\n";
    const auto print = [&book] {
        const auto snapshot = book.snapshot();
        std::cout << "SIDE  PRICE_TICKS  QUANTITY  ORDERS\n";
        for (const auto& level : snapshot.asks) {
            std::cout << "ASK   " << level.price_ticks << "        " << level.total_quantity
                      << "        " << level.orders.size() << '\n';
        }
        for (const auto& level : snapshot.bids) {
            std::cout << "BID   " << level.price_ticks << "        " << level.total_quantity
                      << "        " << level.orders.size() << '\n';
        }
    };
    print();
    if (book.cancel(1, 1) != apex::BookError::None) { return 1; }
    std::cout << "Cancelled order 1; order 2 remains at the bid.\n";
    print();
    return book.check_invariants(true) ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << "Apex error: " << error.what() << '\n';
    return 1;
}
