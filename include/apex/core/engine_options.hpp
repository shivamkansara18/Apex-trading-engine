#pragma once

namespace apex {
enum class PriceIndex { Tree, Flat };
enum class RiskTraversal { Snapshot, Direct };
enum class EventReservation { WorstCase, SmallInitial };

// Performance policies only: business rules and event ordering must be identical.
struct EngineOptions {
    PriceIndex price_index{PriceIndex::Tree};
    RiskTraversal risk_traversal{RiskTraversal::Direct};
    EventReservation event_reservation{EventReservation::WorstCase};
    bool operator==(const EngineOptions&) const = default;
};
[[nodiscard]] constexpr EngineOptions baseline_engine_options() {
    return {PriceIndex::Tree, RiskTraversal::Snapshot, EventReservation::WorstCase};
}
} // namespace apex
