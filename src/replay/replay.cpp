#include "apex/replay/replay.hpp"
#include "../journal/codec.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace apex {
namespace {
journal_detail::Bytes read_exact(std::ifstream& file, std::size_t count) {
    journal_detail::Bytes bytes(count);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count));
    if (!file || file.gcount() != static_cast<std::streamsize>(count)) {
        throw std::runtime_error("Truncated or unreadable journal");
    }
    return bytes;
}
}
ReplayResult replay(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { throw std::runtime_error("Cannot open journal for replay"); }
    const auto header = read_exact(file, 12);
    if (!std::equal(std::begin(journal_detail::magic), std::end(journal_detail::magic), header.begin())) {
        throw std::runtime_error("Invalid journal magic");
    }
    journal_detail::Decoder header_decoder{std::span(header).subspan(8)};
    if (header_decoder.u32() != 1) { throw std::runtime_error("Unsupported journal version"); }
    MatchingEngine engine;
    ReplayResult report;
    std::uint64_t last_record = 0;
    while (true) {
        const auto length_bytes = read_exact(file, 4); // EOF without seal is an error.
        journal_detail::Decoder length_decoder{length_bytes};
        const auto length = length_decoder.u32();
        if (length == 0 || length > journal_detail::max_payload) {
            throw std::runtime_error("Invalid journal record length");
        }
        const auto body = read_exact(file, static_cast<std::size_t>(length) + 8);
        const auto checksum_bytes = read_exact(file, 4);
        journal_detail::Decoder checksum{checksum_bytes};
        if (checksum.u32() != journal_detail::crc32(body)) { throw std::runtime_error("Journal checksum mismatch"); }
        journal_detail::Decoder decoder{body};
        const auto sequence = decoder.u64();
        if (last_record == std::numeric_limits<std::uint64_t>::max() || sequence != last_record + 1) {
            throw std::runtime_error("Journal record sequence mismatch");
        }
        last_record = sequence;
        if (decoder.remaining().front() == journal_detail::seal_tag) {
            if (decoder.remaining().size() != 1) { throw std::runtime_error("Malformed journal seal"); }
            if (file.peek() != std::char_traits<char>::eof() || file.bad()) {
                throw std::runtime_error("Trailing data or read failure after journal seal");
            }
            break;
        }
        const auto input = journal_detail::decode_input(decoder);
        const auto result = journal_detail::apply(engine, input);
        const auto expected = journal_detail::encode_result(result, engine.snapshot());
        if (!std::equal(expected.begin(), expected.end(), decoder.remaining().begin(), decoder.remaining().end())) {
            throw std::runtime_error("Journal output/state mismatch at record " + std::to_string(sequence));
        }
        if (!engine.book().check_invariants(true)) { throw std::runtime_error("Replay book invariant failure"); }
        ++report.records;
        if (std::holds_alternative<Command>(input)) {
            ++report.commands;
            report.events += result.events.size();
            report.event_hash = journal_detail::hash_bytes(journal_detail::encode_events(result.events), report.event_hash);
        }
    }
    report.final_book = engine.snapshot();
    report.state_hash = book_hash(report.final_book);
    return report;
}
} // namespace apex
