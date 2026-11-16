#pragma once

#include "apex/journal/journal.hpp"

namespace apex {
struct ReplayResult {
    std::uint64_t records{}; // Input records, excluding terminal seal.
    std::uint64_t commands{};
    std::uint64_t events{};
    std::uint64_t event_hash{14695981039346656037ULL};
    std::uint64_t state_hash{}; // Canonical book hash; not a risk/configuration hash.
    BookSnapshot final_book;
    bool operator==(const ReplayResult&) const = default;
};

// Strict verification: require a terminal seal and exact event/output equality.
// Throws on bad version, truncation, checksum, sequence or semantic mismatch.
[[nodiscard]] ReplayResult replay(const std::filesystem::path& path);
} // namespace apex
