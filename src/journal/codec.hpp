#pragma once

#include "apex/journal/journal.hpp"

#include <span>

namespace apex::journal_detail {
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint32_t max_payload = 16 * 1024 * 1024;
constexpr std::uint8_t seal_tag = 255;
inline constexpr std::uint8_t magic[] = {'A', 'P', 'E', 'X', 'J', 'N', 'L', 0};

struct Encoder {
    Bytes bytes;
    void u8(std::uint8_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void price(Price value);
};
struct Decoder {
    std::span<const std::uint8_t> bytes;
    std::size_t offset{};
    std::uint8_t u8();
    std::uint32_t u32();
    std::uint64_t u64();
    Price price();
    bool boolean();
    [[nodiscard]] std::span<const std::uint8_t> remaining() const;
};
void encode_input(Encoder& output, const JournalInput& input);
JournalInput decode_input(Decoder& input);
Bytes encode_events(const std::vector<Event>& events);
Bytes encode_result(const JournalResult& result, const BookSnapshot& snapshot);
JournalResult apply(MatchingEngine& engine, const JournalInput& input);
std::uint32_t crc32(std::span<const std::uint8_t> bytes);
std::uint64_t hash_bytes(std::span<const std::uint8_t> bytes,
                         std::uint64_t state = 14695981039346656037ULL);
} // namespace apex::journal_detail
