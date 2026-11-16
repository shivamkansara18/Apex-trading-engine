#include "apex/network/client.hpp"

#include <array>
#include <charconv>
#include <iostream>
#include <sstream>
#include <poll.h>
#include <unistd.h>

namespace net = apex::network;
namespace {
template<class T> T number(std::string_view text) {
    T value{}; const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) { throw std::invalid_argument("Invalid integer: " + std::string(text)); }
    return value;
}
template<class T> T read_number(std::istream& input) {
    std::string text; if (!(input >> text)) { throw std::invalid_argument("Missing command field"); }
    return number<T>(text);
}
void print_event(const apex::Event& event) {
    std::cout << "EVENT " << event.event_sequence << " command=" << event.command_sequence << ' ';
    std::visit([](const auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, apex::Trade>) {
            std::cout << "TRADE buy=" << payload.buy_order_id << " sell=" << payload.sell_order_id
                      << " quantity=" << payload.quantity << " price_ticks=" << payload.price_ticks;
        } else if constexpr (std::is_same_v<T, apex::CommandRejected>) {
            constexpr std::array<const char*, 15> reasons{"invalid side", "invalid order type", "zero quantity", "invalid price",
                "duplicate live ID", "unknown order", "wrong participant", "unknown participant", "quantity limit", "notional limit",
                "open order limit", "price band", "arithmetic overflow", "invalid replacement", "trading disabled"};
            std::cout << "REJECT " << reasons.at(static_cast<std::size_t>(payload.reason));
        } else {
            if constexpr (std::is_same_v<T, apex::OrderAccepted>) { std::cout << "ACCEPT"; }
            else if constexpr (std::is_same_v<T, apex::OrderCancelled>) { std::cout << "CANCEL"; }
            else { std::cout << "UPDATE"; }
            std::cout << " order=" << payload.order.client_order_id << " remaining=" << payload.order.remaining_quantity;
        }
    }, event.payload);
    std::cout << '\n';
}
void check_error(const net::Frame& frame) {
    if (frame.type == net::MessageType::Error) {
        const auto [code, message] = net::decode_error(frame.payload);
        throw std::runtime_error("Gateway error " + std::to_string(static_cast<unsigned>(code)) + ": " + message);
    }
}
void print_book(const net::BookView& book) {
    std::cout << "BOOK feed=" << book.feed_sequence << " command=" << book.command_sequence << '\n';
    for (const auto& level : book.asks) { std::cout << "  ASK " << level.price_ticks << " quantity=" << level.quantity << " orders=" << level.orders << '\n'; }
    for (const auto& level : book.bids) { std::cout << "  BID " << level.price_ticks << " quantity=" << level.quantity << " orders=" << level.orders << '\n'; }
    if (book.bids.empty() && book.asks.empty()) { std::cout << "  empty\n"; }
}
net::Frame parse(std::string_view line, apex::ParticipantId participant, std::uint64_t sequence) {
    std::istringstream input{std::string(line)};
    std::string action; input >> action;
    net::Frame frame;
    if (action == "BUY" || action == "SELL" || action == "MARKET_BUY" || action == "MARKET_SELL") {
        const auto id = read_number<apex::OrderId>(input);
        const auto quantity = read_number<apex::Quantity>(input);
        const bool market = action.starts_with("MARKET_");
        const auto price = market ? 0 : read_number<apex::Price>(input);
        frame = net::command_frame(sequence, apex::NewOrder{id, participant,
            action == "BUY" || action == "MARKET_BUY" ? apex::Side::Buy : apex::Side::Sell,
            market ? apex::OrderType::Market : apex::OrderType::Limit, price, quantity, 0});
    } else if (action == "CANCEL") {
        frame = net::command_frame(sequence, apex::CancelOrder{read_number<apex::OrderId>(input), participant});
    } else if (action == "REPLACE") {
        const auto old = read_number<apex::OrderId>(input); const auto id = read_number<apex::OrderId>(input);
        const auto quantity = read_number<apex::Quantity>(input); const auto price = read_number<apex::Price>(input);
        frame = net::command_frame(sequence, apex::ReplaceOrder{old, id, participant, price, quantity});
    } else if (action == "BOOK") { frame = {net::MessageType::SnapshotRequest, sequence, {}}; }
    else if (action == "ORDERS") { frame = {net::MessageType::OpenOrdersRequest, sequence, {}}; }
    else if (action == "PING") { frame = {net::MessageType::Heartbeat, sequence, {}}; }
    else { throw std::invalid_argument("Unknown command; use HELP"); }
    std::string extra; if (input >> extra) { throw std::invalid_argument("Extra command fields"); }
    return frame;
}
void help() {
    std::cout << "BUY id quantity price_ticks | SELL id quantity price_ticks\n"
              << "MARKET_BUY id quantity | MARKET_SELL id quantity\n"
              << "CANCEL id | REPLACE old_id new_id new_quantity new_price_ticks\n"
              << "BOOK | ORDERS | PING | HELP | QUIT\n";
}
void execute(net::TcpClient& client, const net::Frame& request) {
    client.send(request);
    std::optional<std::uint32_t> reports;
    while (true) {
        const auto frame = client.receive(); check_error(frame);
        if (frame.type == net::MessageType::ExecutionReport) {
            print_event(net::decode_event(frame.payload));
            if (frame.request_sequence == request.request_sequence) {
                if (!reports || *reports == 0) { throw std::runtime_error("Unexpected execution report"); }
                if (--*reports == 0) { return; }
            }
        } else if (frame.request_sequence == request.request_sequence) {
            if (frame.type == net::MessageType::CommandAck) {
                const auto ack = net::decode_ack(frame.payload); reports = ack.event_count;
                std::cout << "ACK command=" << ack.command_sequence << " accepted=" << ack.accepted << " events=" << ack.event_count << '\n';
            } else if (frame.type == net::MessageType::BookSnapshot) { print_book(net::decode_book(frame.payload)); return; }
            else if (frame.type == net::MessageType::OpenOrdersSnapshot) {
                const auto snapshot = net::decode_open_orders(frame.payload);
                std::cout << "OPEN_ORDERS participant=" << snapshot.participant << " command=" << snapshot.command_sequence << " count=" << snapshot.orders.size() << '\n';
                for (const auto& order : snapshot.orders) {
                    std::cout << "  " << (order.side == apex::Side::Buy ? "BUY" : "SELL") << " id=" << order.client_order_id
                              << " price_ticks=" << order.price_ticks << " remaining=" << order.remaining_quantity << '\n';
                }
                return;
            }
            else if (frame.type == net::MessageType::HeartbeatAck && frame.payload.empty()) { std::cout << "PONG\n"; return; }
            else { throw std::runtime_error("Unexpected gateway response"); }
        }
    }
}
std::optional<std::string> console_line(net::TcpClient& client, std::string& pending, bool& eof) {
    while (pending.find('\n') == std::string::npos && !eof) {
        struct pollfd descriptors[]{{client.native_handle(), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
        if (!client.has_pending()) {
            const auto ready = ::poll(descriptors, 2, -1);
            if (ready < 0 && errno == EINTR) { continue; }
            if (ready < 0) { throw std::runtime_error("Client poll failed"); }
        }
        if (client.has_pending() || descriptors[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            const auto frame = client.receive(); check_error(frame);
            if (frame.type != net::MessageType::ExecutionReport || frame.request_sequence != 0) {
                throw std::runtime_error("Unexpected asynchronous response");
            }
            std::cout << '\n'; print_event(net::decode_event(frame.payload));
            std::cout << "apex> " << std::flush;
        }
        if (descriptors[1].revents & (POLLERR | POLLNVAL)) { eof = true; }
        if (descriptors[1].revents & (POLLIN | POLLHUP)) {
            char bytes[4096];
            const auto count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
            if (count < 0 && errno == EINTR) { continue; }
            if (count < 0) { throw std::runtime_error("Console read failed"); }
            if (count == 0) { eof = true; }
            else { pending.append(bytes, static_cast<std::size_t>(count)); }
            if (pending.size() > 65'536) { throw std::runtime_error("Console input exceeds 64 KiB limit"); }
        }
    }
    if (pending.empty() && eof) { return std::nullopt; }
    const auto newline = pending.find('\n');
    auto line = pending.substr(0, newline);
    pending.erase(0, newline == std::string::npos ? pending.size() : newline + 1);
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
    return line;
}
}
int main(int argc, char* argv[]) try {
    std::uint16_t port = 9000; apex::ParticipantId participant = 1;
    bool watch = false; std::optional<std::string> one_command;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key = argv[i];
        if (key == "--help") {
            std::cout << "Usage: apex_client [--port 9000] [--participant 1] [--command \"BUY 1 100 10000\" | --watch]\n";
            help(); return 0;
        }
        if (key == "--watch") { watch = true; continue; }
        if (++i == argc) { throw std::invalid_argument("Missing option value"); }
        if (key == "--port") { port = number<std::uint16_t>(argv[i]); }
        else if (key == "--participant") { participant = number<apex::ParticipantId>(argv[i]); }
        else if (key == "--command") { one_command = argv[i]; }
        else { throw std::invalid_argument("Unknown client option"); }
    }
    if (watch && one_command) { throw std::invalid_argument("Choose --watch or --command"); }
    net::TcpClient client(port);
    client.send(net::hello(1, participant));
    const auto hello = client.receive(); check_error(hello);
    if (hello.type != net::MessageType::HelloAck || hello.request_sequence != 1 || net::decode_participant(hello.payload) != participant) {
        throw std::runtime_error("Invalid HELLO acknowledgement");
    }
    std::cout << "Connected to 127.0.0.1:" << port << " as participant " << participant << '\n';
    std::uint64_t next = 2;
    if (watch) {
        client.send({net::MessageType::Subscribe, next++, {}});
        net::FeedTracker feed;
        bool heartbeat_pending = false;
        while (true) {
            net::Frame frame;
            try { frame = client.receive(); }
            catch (const net::SocketTimeout&) {
                if (heartbeat_pending) { throw std::runtime_error("Gateway is unresponsive"); }
                std::cout << "Feed idle; checking heartbeat.\n" << std::flush;
                client.send({net::MessageType::Heartbeat, next++, {}}); heartbeat_pending = true; continue;
            }
            check_error(frame);
            bool gap = false;
            if (frame.type == net::MessageType::SubscribeAck) { (void)feed.observe(net::decode_sequence(frame.payload), true); }
            else if (frame.type == net::MessageType::BookSnapshot) {
                const auto book = net::decode_book(frame.payload);
                gap = !feed.observe(book.feed_sequence, frame.request_sequence != 0); print_book(book);
            } else if (frame.type == net::MessageType::TradeUpdate) {
                const auto [sequence, event] = net::decode_trade_update(frame.payload);
                gap = !feed.observe(sequence); std::cout << "FEED " << sequence << ' '; print_event(event);
            } else if (frame.type == net::MessageType::ExecutionReport) { print_event(net::decode_event(frame.payload)); }
            else if (frame.type == net::MessageType::HeartbeatAck && frame.payload.empty()) { heartbeat_pending = false; }
            else { throw std::runtime_error("Unexpected feed response"); }
            if (gap) {
                std::cout << "FEED GAP; requesting snapshot.\n";
                client.send({net::MessageType::SnapshotRequest, next++, {}});
            }
            std::cout << std::flush;
        }
    }
    if (one_command) { execute(client, parse(*one_command, participant, next)); return 0; }
    help();
    std::string input;
    bool eof = false;
    while (true) {
        std::cout << "apex> " << std::flush;
        const auto line = console_line(client, input, eof);
        if (!line || *line == "QUIT") { break; }
        if (*line == "HELP" || line->empty()) { help(); continue; }
        net::Frame frame;
        try { frame = parse(*line, participant, next); }
        catch (const std::invalid_argument& error) { std::cerr << error.what() << '\n'; continue; }
        ++next; execute(client, frame);
    }
    return 0;
} catch (const std::exception& error) { std::cerr << "Client error: " << error.what() << '\n'; return 1; }
