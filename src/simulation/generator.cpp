#include "apex/simulation/workload.hpp"

#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>

namespace apex {
std::string_view profile_name(WorkloadProfile profile) {
    switch (profile) {
    case WorkloadProfile::Balanced: return "balanced";
    case WorkloadProfile::HotPrice: return "hot-price";
    case WorkloadProfile::ManyLevels: return "many-levels";
    case WorkloadProfile::HighCancel: return "high-cancel";
    case WorkloadProfile::MarketBurst: return "market-burst";
    case WorkloadProfile::LargePartial: return "large-partial";
    }
    throw std::invalid_argument("Unknown workload profile");
}
WorkloadProfile parse_profile(std::string_view name) {
    for (const auto profile : {WorkloadProfile::Balanced, WorkloadProfile::HotPrice,
             WorkloadProfile::ManyLevels, WorkloadProfile::HighCancel,
             WorkloadProfile::MarketBurst, WorkloadProfile::LargePartial}) {
        if (name == profile_name(profile)) { return profile; }
    }
    throw std::invalid_argument("Unknown workload profile: " + std::string(name));
}
WorkloadConfig workload_preset(WorkloadProfile profile) {
    (void)profile_name(profile);
    WorkloadConfig config;
    config.profile = profile;
    if (profile == WorkloadProfile::HotPrice) { config.price_width = 0; }
    if (profile == WorkloadProfile::ManyLevels) { config.price_width = 1000; }
    if (profile == WorkloadProfile::HighCancel) {
        config.cancel_percent = 80; config.replace_percent = 0;
    }
    if (profile == WorkloadProfile::MarketBurst) {
        config.cancel_percent = 0; config.replace_percent = 0;
        config.market_percent = 85; config.burst_size = 25;
    }
    if (profile == WorkloadProfile::LargePartial) {
        config.cancel_percent = 0; config.replace_percent = 0;
    }
    return config;
}
ParticipantRole participant_role(ParticipantId id) {
    if (id == 0) { throw std::invalid_argument("Simulator participant IDs start at 1"); }
    switch ((id - 1) % 3) {
    case 0: return ParticipantRole::LiquidityProvider;
    case 1: return ParticipantRole::LiquidityTaker;
    default: return ParticipantRole::RandomFlow;
    }
}
void validate_workload_config(const WorkloadConfig& config) {
    (void)profile_name(config.profile);
    if (config.commands > 100'000 || config.symbols == 0 || config.symbols > 32 ||
        config.participants < 2 || config.participants > 1000 ||
        static_cast<std::uint64_t>(config.symbols) * config.participants > 10'000 ||
        config.price_width < 0 || config.price_width > 1'000'000 ||
        config.center_price <= config.price_width + 1 ||
        config.center_price > 1'000'000'000 ||
        config.min_quantity == 0 || config.max_quantity < config.min_quantity ||
        config.max_quantity > 1'000'000 || config.cancel_percent > 100 ||
        config.replace_percent > 100 || config.cancel_percent + config.replace_percent > 100 ||
        config.market_percent > 100 || config.commands_per_second == 0 ||
        config.commands_per_second > 1'000'000'000 || config.burst_size == 0 ||
        config.burst_size > 100'000 ||
        (config.distribution != PriceDistribution::Uniform && config.distribution != PriceDistribution::Triangular)) {
        throw std::invalid_argument("Invalid workload configuration or resource bound exceeded");
    }
}

namespace {
// Stable rejection sampling avoids standard-library distribution differences.
class Random {
public:
    explicit Random(std::uint64_t seed) : engine_(seed) {}
    std::uint64_t below(std::uint64_t bound) {
        const auto threshold = (std::uint64_t{0} - bound) % bound;
        std::uint64_t value;
        do { value = engine_(); } while (value < threshold);
        return value % bound;
    }
private:
    std::mt19937_64 engine_;
};
struct SymbolState {
    MatchingEngine engine;
    std::map<OrderId, Order> live; // Stable iteration for cancel/replace selection.
    std::uint64_t steps{};
};
void update_live(SymbolState& state, const std::vector<Event>& events) {
    for (const auto& event : events) {
        if (const auto* updated = std::get_if<OrderUpdated>(&event.payload)) {
            if (updated->order.remaining_quantity == 0) { state.live.erase(updated->order.client_order_id); }
            else { state.live.insert_or_assign(updated->order.client_order_id, updated->order); }
        } else if (const auto* cancelled = std::get_if<OrderCancelled>(&event.payload)) {
            state.live.erase(cancelled->order.client_order_id);
        }
    }
}
}

Workload generate_workload(const WorkloadConfig& config) {
    validate_workload_config(config);
    Workload result{config, {}};
    result.entries.reserve(static_cast<std::size_t>(config.commands));
    Random random(config.seed);
    std::vector<std::unique_ptr<SymbolState>> states;
    for (std::uint32_t symbol = 0; symbol < config.symbols; ++symbol) {
        auto state = std::make_unique<SymbolState>();
        for (ParticipantId id = 1; id <= config.participants; ++id) {
            if (!state->engine.register_participant(id)) { throw std::logic_error("Simulator registration failed"); }
        }
        states.push_back(std::move(state));
    }
    const auto price_offset = [&] {
        const auto bound = static_cast<std::uint64_t>(config.price_width) + 1;
        auto value = random.below(bound);
        if (config.distribution == PriceDistribution::Triangular) { value = (value + random.below(bound)) / 2; }
        return static_cast<Price>(value);
    };
    for (std::uint64_t step = 0; step < config.commands; ++step) {
        const auto symbol = static_cast<std::uint32_t>(random.below(config.symbols));
        auto& state = *states[symbol];
        const auto local_step = state.steps++;
        const auto id = step + 1;
        const auto scheduled = (step / config.burst_size) * config.burst_size * 1'000'000'000ULL /
                               config.commands_per_second;
        Command command;
        if (config.profile == WorkloadProfile::LargePartial) {
            // Scripted 16 small asks then one large market buy, per symbol.
            const auto phase = local_step % 17;
            command = NewOrder{id, phase == 16 ? 2U : 1U,
                phase == 16 ? Side::Buy : Side::Sell,
                phase == 16 ? OrderType::Market : OrderType::Limit,
                phase == 16 ? 0 : config.center_price + 1 + static_cast<Price>(phase % 4),
                phase == 16 ? config.max_quantity : config.min_quantity, scheduled};
        } else {
            const auto action = random.below(100);
            if (!state.live.empty() && action < config.cancel_percent + config.replace_percent) {
                auto found = state.live.begin();
                std::advance(found, static_cast<std::ptrdiff_t>(random.below(state.live.size())));
                const auto& old = found->second;
                if (action < config.cancel_percent) { command = CancelOrder{old.client_order_id, old.participant_id}; }
                else {
                    const auto price = config.profile == WorkloadProfile::HotPrice ? config.center_price :
                        config.center_price + (old.side == Side::Buy ? -price_offset() : price_offset());
                    command = ReplaceOrder{old.client_order_id, id, old.participant_id, price,
                        config.min_quantity + random.below(config.max_quantity - config.min_quantity + 1)};
                }
            } else {
                const auto participant = static_cast<ParticipantId>(1 + random.below(config.participants));
                const auto role = participant_role(participant);
                const auto side = random.below(2) == 0 ? Side::Buy : Side::Sell;
                auto type = role == ParticipantRole::LiquidityTaker ? OrderType::Market : OrderType::Limit;
                if (role == ParticipantRole::RandomFlow && random.below(100) < config.market_percent) {
                    type = OrderType::Market;
                }
                Price price = 0;
                if (type == OrderType::Limit) {
                    if (config.profile == WorkloadProfile::HotPrice) { price = config.center_price; }
                    else if (role == ParticipantRole::LiquidityProvider) {
                        price = config.center_price + (side == Side::Buy ? -1 - price_offset() : 1 + price_offset());
                        if (side == Side::Buy) {
                            if (const auto ask = state.engine.book().best_ask(); ask && price >= *ask) { price = *ask - 1; }
                        } else {
                            if (const auto bid = state.engine.book().best_bid(); bid && price <= *bid) { price = *bid + 1; }
                        }
                    } else { price = config.center_price + price_offset() - config.price_width / 2; }
                }
                command = NewOrder{id, participant, side, type, price,
                    config.min_quantity + random.below(config.max_quantity - config.min_quantity + 1), scheduled};
            }
        }
        const auto events = state.engine.process(command);
        for (const auto& event : events) {
            if (std::holds_alternative<CommandRejected>(event.payload)) {
                throw std::logic_error("Generator produced an invalid command");
            }
        }
        update_live(state, events);
        result.entries.push_back({scheduled, symbol, command});
    }
    return result;
}
} // namespace apex
