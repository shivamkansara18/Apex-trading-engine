#include "apex/simulation/workload.hpp"
#include "../journal/codec.hpp"

#include <limits>
#include <memory>
#include <stdexcept>

namespace apex {
WorkloadResult run_workload(const Workload& workload, const std::filesystem::path& journal_path) {
    validate_workload(workload);
    if (!journal_path.empty() && workload.config.symbols != 1) {
        throw std::invalid_argument("Workload journal recording currently requires one symbol");
    }
    std::unique_ptr<JournaledEngine> journal;
    std::vector<std::unique_ptr<MatchingEngine>> engines;
    if (!journal_path.empty()) { journal = std::make_unique<JournaledEngine>(journal_path); }
    for (std::uint32_t symbol = 0; symbol < workload.config.symbols; ++symbol) {
        if (!journal) { engines.push_back(std::make_unique<MatchingEngine>()); }
        for (ParticipantId id = 1; id <= workload.config.participants; ++id) {
            const bool registered = journal ? journal->register_participant(id) : engines.back()->register_participant(id);
            if (!registered) { throw std::logic_error("Workload participant registration failed"); }
        }
    }
    WorkloadResult result;
    for (const auto& entry : workload.entries) {
        const auto events = journal ? journal->process(entry.command) : engines[entry.symbol]->process(entry.command);
        const auto& engine = journal ? journal->engine() : *engines[entry.symbol];
        if (!engine.book().check_invariants(true)) { throw std::logic_error("Workload book invariant failure"); }
        ++result.commands;
        result.events += events.size();
        for (const auto& event : events) {
            if (std::holds_alternative<CommandRejected>(event.payload)) { ++result.rejected; }
            if (const auto* trade = std::get_if<Trade>(&event.payload)) {
                ++result.trades;
                if (trade->quantity > std::numeric_limits<Quantity>::max() - result.executed_quantity) {
                    throw std::overflow_error("Workload executed quantity total overflow");
                }
                result.executed_quantity += trade->quantity;
            }
        }
        journal_detail::Encoder prefix;
        prefix.u32(entry.symbol);
        result.event_hash = journal_detail::hash_bytes(prefix.bytes, result.event_hash);
        result.event_hash = journal_detail::hash_bytes(journal_detail::encode_events(events), result.event_hash);
    }
    if (journal) {
        result.final_books.push_back(journal->engine().snapshot());
        journal->finish();
    } else {
        for (const auto& engine : engines) { result.final_books.push_back(engine->snapshot()); }
    }
    return result;
}
} // namespace apex
