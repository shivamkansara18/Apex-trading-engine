#include "apex/simulation/workload.hpp"

#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <fstream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <unistd.h>

namespace apex {
void validate_workload(const Workload& workload) {
    validate_workload_config(workload.config);
    if (workload.entries.size() != workload.config.commands) {
        throw std::invalid_argument("Workload entry count differs from metadata");
    }
    TimestampNs previous = 0;
    for (const auto& entry : workload.entries) {
        if (entry.symbol >= workload.config.symbols || entry.scheduled_ns < previous) {
            throw std::invalid_argument("Invalid workload symbol or decreasing schedule");
        }
        previous = entry.scheduled_ns;
        std::visit([&](const auto& command) {
            if (command.participant_id == 0 || command.participant_id > workload.config.participants) {
                throw std::invalid_argument("Workload participant outside configured registry");
            }
            using T = std::decay_t<decltype(command)>;
            if constexpr (std::is_same_v<T, NewOrder>) {
                if ((command.side != Side::Buy && command.side != Side::Sell) ||
                    (command.type != OrderType::Limit && command.type != OrderType::Market)) {
                    throw std::invalid_argument("Invalid workload side/type");
                }
            }
        }, entry.command);
    }
}
namespace {
std::string token(std::istream& input) {
    std::string result;
    if (!(input >> result)) { throw std::runtime_error("Truncated workload file"); }
    return result;
}
void expect(std::istream& input, std::string_view expected) {
    if (token(input) != expected) { throw std::runtime_error("Unexpected workload token; expected " + std::string(expected)); }
}
template<class T> T number(std::istream& input) {
    const auto text = token(input);
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error("Invalid or out-of-range workload integer: " + text);
    }
    return value;
}
}

void save_workload(const std::filesystem::path& path, const Workload& workload) {
    validate_workload(workload);
    const auto& c = workload.config;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "APEX_WORKLOAD 1\nCONFIG " << c.seed << ' ' << c.commands << ' ' << c.symbols << ' '
        << c.participants << ' ' << c.center_price << ' ' << c.price_width << ' '
        << c.min_quantity << ' ' << c.max_quantity << ' ' << c.cancel_percent << ' '
        << c.replace_percent << ' ' << c.market_percent << ' ' << c.commands_per_second << ' '
        << c.burst_size << ' ' << (c.distribution == PriceDistribution::Uniform ? "uniform" : "triangular")
        << ' ' << profile_name(c.profile) << '\n';
    for (const auto& entry : workload.entries) {
        out << entry.scheduled_ns << ' ' << entry.symbol << ' ';
        std::visit([&](const auto& command) {
            using T = std::decay_t<decltype(command)>;
            if constexpr (std::is_same_v<T, NewOrder>) {
                out << "NEW " << command.client_order_id << ' ' << command.participant_id << ' '
                    << (command.side == Side::Buy ? "BUY" : "SELL") << ' '
                    << (command.type == OrderType::Limit ? "LIMIT" : "MARKET") << ' '
                    << command.price_ticks << ' ' << command.quantity << ' ' << command.client_timestamp_ns;
            } else if constexpr (std::is_same_v<T, CancelOrder>) {
                out << "CANCEL " << command.client_order_id << ' ' << command.participant_id;
            } else {
                out << "REPLACE " << command.old_client_order_id << ' ' << command.new_client_order_id << ' '
                    << command.participant_id << ' ' << command.new_price_ticks << ' ' << command.new_quantity;
            }
        }, entry.command);
        out << '\n';
    }
    out << "END\n";
    const auto contents = out.str();
    const auto fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) { throw std::system_error(errno, std::generic_category(), "Cannot create new workload file"); }
    try {
        std::size_t offset = 0;
        while (offset < contents.size()) {
            const auto written = ::write(fd, contents.data() + offset, contents.size() - offset);
            if (written < 0 && errno == EINTR) { continue; }
            if (written < 0) { throw std::system_error(errno, std::generic_category(), "Workload write failed"); }
            if (written == 0) { throw std::runtime_error("Workload write made no progress"); }
            offset += static_cast<std::size_t>(written);
        }
    } catch (...) { ::close(fd); throw; }
    if (::close(fd) < 0) { throw std::system_error(errno, std::generic_category(), "Workload close failed"); }
}

Workload load_workload(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > 64 * 1024 * 1024) {
        throw std::runtime_error("Workload file exceeds 64 MiB bound");
    }
    std::ifstream input(path);
    if (!input) { throw std::runtime_error("Cannot read workload file"); }
    expect(input, "APEX_WORKLOAD");
    if (number<std::uint32_t>(input) != 1) { throw std::runtime_error("Unsupported workload version"); }
    expect(input, "CONFIG");
    Workload result;
    auto& c = result.config;
    c.seed = number<std::uint64_t>(input); c.commands = number<std::uint64_t>(input);
    c.symbols = number<std::uint32_t>(input); c.participants = number<std::uint32_t>(input);
    c.center_price = number<Price>(input); c.price_width = number<Price>(input);
    c.min_quantity = number<Quantity>(input); c.max_quantity = number<Quantity>(input);
    c.cancel_percent = number<std::uint32_t>(input); c.replace_percent = number<std::uint32_t>(input);
    c.market_percent = number<std::uint32_t>(input); c.commands_per_second = number<std::uint64_t>(input);
    c.burst_size = number<std::uint32_t>(input);
    const auto distribution = token(input);
    if (distribution != "uniform" && distribution != "triangular") { throw std::runtime_error("Unknown price distribution"); }
    c.distribution = distribution == "uniform" ? PriceDistribution::Uniform : PriceDistribution::Triangular;
    c.profile = parse_profile(token(input));
    validate_workload_config(c);
    result.entries.reserve(static_cast<std::size_t>(c.commands));
    for (std::uint64_t i = 0; i < c.commands; ++i) {
        ScheduledCommand entry;
        entry.scheduled_ns = number<TimestampNs>(input); entry.symbol = number<std::uint32_t>(input);
        const auto type = token(input);
        if (type == "NEW") {
            NewOrder request;
            request.client_order_id = number<OrderId>(input); request.participant_id = number<ParticipantId>(input);
            const auto side = token(input); const auto order_type = token(input);
            if ((side != "BUY" && side != "SELL") || (order_type != "LIMIT" && order_type != "MARKET")) {
                throw std::runtime_error("Unknown workload side/order type");
            }
            request.side = side == "BUY" ? Side::Buy : Side::Sell;
            request.type = order_type == "LIMIT" ? OrderType::Limit : OrderType::Market;
            request.price_ticks = number<Price>(input); request.quantity = number<Quantity>(input);
            request.client_timestamp_ns = number<TimestampNs>(input);
            entry.command = request;
        } else if (type == "CANCEL") {
            entry.command = CancelOrder{number<OrderId>(input), number<ParticipantId>(input)};
        } else if (type == "REPLACE") {
            entry.command = ReplaceOrder{number<OrderId>(input), number<OrderId>(input), number<ParticipantId>(input),
                                          number<Price>(input), number<Quantity>(input)};
        } else { throw std::runtime_error("Unknown workload command type"); }
        result.entries.push_back(entry);
    }
    expect(input, "END");
    std::string extra;
    if (input >> extra || input.bad()) { throw std::runtime_error("Trailing data or workload read failure"); }
    validate_workload(result);
    return result;
}
} // namespace apex
