#include "apex/replay/replay.hpp"
#include "../../src/journal/codec.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <csignal>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <system_error>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using Bytes = apex::journal_detail::Bytes;
Bytes read_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void write_bytes(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(file.good());
}
std::uint32_t record_length(const Bytes& bytes, std::size_t offset) {
    apex::journal_detail::Decoder decoder{std::span(bytes).subspan(offset, 4)};
    return decoder.u32();
}
void repair_checksum(Bytes& bytes, std::size_t offset) {
    const auto length = record_length(bytes, offset);
    const auto checksum = apex::journal_detail::crc32(std::span(bytes).subspan(offset + 4, length + 8));
    apex::journal_detail::Encoder encoder;
    encoder.u32(checksum);
    std::copy(encoder.bytes.begin(), encoder.bytes.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 12 + length));
}
class Journal : public testing::Test {
protected:
    std::filesystem::path directory;
    std::filesystem::path path;
    void SetUp() override {
        auto pattern = (std::filesystem::temp_directory_path() / "apex-journal-XXXXXX").string();
        auto* created = ::mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        directory = created;
        path = directory / "session.apex";
    }
    void TearDown() override {
        std::error_code error;
        if (!directory.empty()) { std::filesystem::remove_all(directory, error); }
    }
    void create_small() {
        apex::JournaledEngine session(path);
        ASSERT_TRUE(session.register_participant(1));
        const auto events = session.process(apex::NewOrder{1, 1, apex::Side::Buy, apex::OrderType::Limit, 100, 10, 0});
        ASSERT_EQ(events.size(), 2U);
        session.finish();
    }
};
}

TEST(JournalCodec, UsesBigEndianAndPreservesSignedBits) {
    apex::journal_detail::Encoder encoder;
    encoder.u32(0x12345678);
    encoder.price(std::numeric_limits<apex::Price>::min());
    EXPECT_EQ(encoder.bytes, (Bytes{0x12, 0x34, 0x56, 0x78, 0x80, 0, 0, 0, 0, 0, 0, 0}));
    apex::journal_detail::Decoder decoder{encoder.bytes};
    EXPECT_EQ(decoder.u32(), 0x12345678U);
    EXPECT_EQ(decoder.price(), std::numeric_limits<apex::Price>::min());
    EXPECT_TRUE(decoder.remaining().empty());
    EXPECT_THROW((void)decoder.u8(), std::runtime_error);
    const std::string sample = "123456789";
    EXPECT_EQ(apex::journal_detail::crc32(std::span(reinterpret_cast<const std::uint8_t*>(sample.data()), sample.size())),
              0xcbf43926U);
}

TEST(JournalCodec, EveryCommandRoundTripsIncludingInvalidEnums) {
    const std::vector<apex::Command> commands{
        apex::NewOrder{std::numeric_limits<apex::OrderId>::max(), 42, static_cast<apex::Side>(255),
                       static_cast<apex::OrderType>(254), -99, 0, 123},
        apex::CancelOrder{11, 12}, apex::ReplaceOrder{1, 2, 3, 4, 5}};
    for (const auto& command : commands) {
        apex::journal_detail::Encoder encoder;
        apex::journal_detail::encode_input(encoder, command);
        apex::journal_detail::Decoder decoder{encoder.bytes};
        const auto decoded = apex::journal_detail::decode_input(decoder);
        EXPECT_EQ(std::get<apex::Command>(decoded), command);
        EXPECT_TRUE(decoder.remaining().empty());
    }
}

TEST_F(Journal, ReplaysCommandsAdminTransitionsAndExactEventsTwice) {
    apex::BookSnapshot expected;
    std::uint64_t event_count = 0;
    {
        apex::JournaledEngine session(path);
        ASSERT_TRUE(session.register_participant(1));
        ASSERT_TRUE(session.register_participant(2));
        EXPECT_FALSE(session.register_participant(1));
        EXPECT_FALSE(session.configure_participant(99, {}));
        const auto process = [&](const apex::Command& command) {
            const auto events = session.process(command);
            event_count += events.size();
        };
        process(apex::NewOrder{1, 1, apex::Side::Buy, apex::OrderType::Limit, 100, 10, 99});
        process(apex::NewOrder{2, 2, apex::Side::Sell, apex::OrderType::Limit, 99, 4, 0});
        process(apex::ReplaceOrder{1, 3, 1, 101, 8});
        auto limits = *session.engine().risk().limits(1);
        limits.max_order_quantity = 1;
        ASSERT_TRUE(session.configure_participant(1, limits));
        process(apex::NewOrder{4, 1, apex::Side::Buy, apex::OrderType::Limit, 100, 2, 0});
        limits.trading_enabled = false;
        ASSERT_TRUE(session.configure_participant(1, limits));
        process(apex::ReplaceOrder{3, 4, 1, 100, 1});
        session.set_trading_enabled(false);
        process(apex::CancelOrder{3, 1});
        session.set_trading_enabled(true);
        process(apex::NewOrder{5, 2, apex::Side::Sell, apex::OrderType::Market, -10, 7, 0});
        process(apex::NewOrder{6, 99, apex::Side::Buy, apex::OrderType::Limit, 100, 1, 0});
        process(apex::NewOrder{7, 2, static_cast<apex::Side>(255), apex::OrderType::Limit, 100, 1, 0});
        expected = session.engine().snapshot();
        session.finish();
    }
    const auto first = apex::replay(path);
    const auto second = apex::replay(path);
    EXPECT_EQ(first, second);
    EXPECT_EQ(first.final_book, expected);
    EXPECT_EQ(first.commands, 9U);
    EXPECT_EQ(first.records, 17U);
    EXPECT_EQ(first.events, event_count);
    EXPECT_EQ(first.state_hash, apex::book_hash(expected));
}

TEST_F(Journal, EmptySealedSessionReplays) {
    apex::JournaledEngine session(path);
    session.finish();
    const auto result = apex::replay(path);
    EXPECT_EQ(result.records, 0U);
    EXPECT_EQ(result.events, 0U);
    EXPECT_EQ(result.final_book, apex::BookSnapshot{});
}

TEST_F(Journal, NeverOverwritesExistingFileAndRefusesMissingDirectory) {
    create_small();
    const auto before = read_bytes(path);
    EXPECT_THROW(apex::JournaledEngine duplicate(path), std::system_error);
    EXPECT_EQ(read_bytes(path), before);
    EXPECT_THROW(apex::JournaledEngine missing(directory / "missing" / "session"), std::system_error);
}

TEST_F(Journal, RejectsMissingFileAndEveryTruncatedPrefix) {
    EXPECT_THROW((void)apex::replay(path), std::runtime_error);
    create_small();
    const auto valid = read_bytes(path);
    const auto damaged = directory / "damaged.apex";
    // Includes cuts at complete record boundaries and throughout the seal.
    for (std::size_t length = 0; length < valid.size(); ++length) {
        write_bytes(damaged, Bytes(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(length)));
        EXPECT_THROW((void)apex::replay(damaged), std::runtime_error) << "length=" << length;
    }
}

TEST_F(Journal, UnfinishedAndFailedSessionsCannotBeMistakenForCompleteLogs) {
    {
        apex::JournaledEngine session(path);
        ASSERT_TRUE(session.register_participant(1));
    }
    EXPECT_THROW((void)apex::replay(path), std::runtime_error);
    const auto failed_path = directory / "failed.apex";
    {
        apex::JournaledEngine session(failed_path);
        apex::RiskLimits invalid;
        invalid.min_price_ticks = 0;
        EXPECT_THROW((void)session.register_participant(1, invalid), std::invalid_argument);
        EXPECT_THROW((void)session.register_participant(2), std::logic_error);
        EXPECT_THROW(session.finish(), std::logic_error);
    }
    EXPECT_THROW((void)apex::replay(failed_path), std::runtime_error);
}

TEST_F(Journal, FinishedSessionRefusesFurtherMutation) {
    apex::JournaledEngine session(path);
    session.finish();
    const auto before = read_bytes(path);
    EXPECT_THROW((void)session.register_participant(1), std::logic_error);
    EXPECT_THROW(session.set_trading_enabled(false), std::logic_error);
    EXPECT_THROW(session.finish(), std::logic_error);
    EXPECT_EQ(read_bytes(path), before);
}

TEST_F(Journal, WriteFailureStopsSessionBeforeReturningSuccess) {
    const auto child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        try {
            apex::JournaledEngine session(path);
            struct rlimit limit {};
            if (::getrlimit(RLIMIT_FSIZE, &limit) != 0) { ::_exit(10); }
            limit.rlim_cur = 12; // Header fits; first record cannot be written.
            std::signal(SIGXFSZ, SIG_IGN);
            if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) { ::_exit(11); }
            try { (void)session.register_participant(1); ::_exit(12); }
            catch (const std::system_error&) {}
            try { (void)session.register_participant(2); ::_exit(13); }
            catch (const std::logic_error&) {}
            try { session.finish(); ::_exit(14); }
            catch (const std::logic_error&) {}
            ::_exit(0);
        } catch (...) { ::_exit(15); }
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_THROW((void)apex::replay(path), std::runtime_error);
}

TEST_F(Journal, SeededRunProducesIdenticalFilesAndReplayReports) {
    const auto generate = [&](const std::filesystem::path& output) {
        apex::JournaledEngine session(output);
        for (apex::ParticipantId id = 1; id <= 3; ++id) {
            ASSERT_TRUE(session.register_participant(id));
        }
        std::mt19937 random(987);
        for (apex::OrderId step = 1; step <= 150; ++step) {
            apex::Command command = apex::NewOrder{step, 1 + random() % 3,
                random() % 2 == 0 ? apex::Side::Buy : apex::Side::Sell,
                random() % 4 == 0 ? apex::OrderType::Market : apex::OrderType::Limit,
                95 + random() % 10, 1 + random() % 30, 0};
            if (random() % 4 == 0) { command = apex::CancelOrder{random() % step, 1 + random() % 3}; }
            else if (random() % 5 == 0) {
                command = apex::ReplaceOrder{random() % step, step, 1 + random() % 3,
                                            95 + random() % 10, 1 + random() % 30};
            }
            const auto events = session.process(command);
            ASSERT_FALSE(events.empty());
        }
        session.finish();
    };
    generate(path);
    const auto second = directory / "second.apex";
    generate(second);
    EXPECT_EQ(read_bytes(path), read_bytes(second));
    EXPECT_EQ(apex::replay(path), apex::replay(second));
    EXPECT_EQ(apex::replay(path).commands, 150U);
}

TEST_F(Journal, RejectsMagicVersionChecksumAndTrailingGarbage) {
    create_small();
    const auto original = read_bytes(path);
    for (const auto offset : {std::size_t{0}, std::size_t{11}, std::size_t{30}}) {
        auto damaged = original;
        damaged[offset] ^= 0x40;
        write_bytes(path, damaged);
        EXPECT_THROW((void)apex::replay(path), std::runtime_error);
    }
    auto trailing = original;
    trailing.push_back(0);
    write_bytes(path, trailing);
    EXPECT_THROW((void)apex::replay(path), std::runtime_error);
}

TEST_F(Journal, RejectsZeroAndOversizedLengthsBeforeAllocatingPayload) {
    create_small();
    const auto original = read_bytes(path);
    for (const auto length : {0U, apex::journal_detail::max_payload + 1}) {
        auto damaged = original;
        apex::journal_detail::Encoder encoder;
        encoder.u32(length);
        std::copy(encoder.bytes.begin(), encoder.bytes.end(), damaged.begin() + 12);
        write_bytes(path, damaged);
        EXPECT_THROW((void)apex::replay(path), std::runtime_error);
    }
}

TEST_F(Journal, DetectsSemanticMismatchEvenWhenChecksumIsRecomputed) {
    create_small();
    const auto original = read_bytes(path);
    // First record is registration. Last byte of payload belongs to book hash.
    auto damaged = original;
    damaged[12 + 12 + record_length(damaged, 12) - 1] ^= 1;
    repair_checksum(damaged, 12);
    write_bytes(path, damaged);
    EXPECT_THROW((void)apex::replay(path), std::runtime_error);
    // Second record is a command. Alter an event field, preserving its CRC.
    damaged = original;
    const auto second = 12 + 16 + record_length(damaged, 12);
    damaged[second + 12 + record_length(damaged, second) - 10] ^= 1;
    repair_checksum(damaged, second);
    write_bytes(path, damaged);
    EXPECT_THROW((void)apex::replay(path), std::runtime_error);
}

TEST_F(Journal, RejectsRecordReorderingAndUnknownTagsWithValidChecksums) {
    create_small();
    const auto original = read_bytes(path);
    auto damaged = original;
    damaged[12 + 11] = 2; // First sequence must be 1.
    repair_checksum(damaged, 12);
    write_bytes(path, damaged);
    EXPECT_THROW((void)apex::replay(path), std::runtime_error);
    damaged = original;
    damaged[12 + 12] = 200; // Unknown input tag.
    repair_checksum(damaged, 12);
    write_bytes(path, damaged);
    EXPECT_THROW((void)apex::replay(path), std::runtime_error);
}

TEST_F(Journal, BookHashIncludesSequencePriorityAndRemainingQuantity) {
    apex::BookSnapshot snapshot{1, {{100, 10, {{1, 1, apex::Side::Buy, apex::OrderType::Limit,
                                              100, 10, 10, 1, apex::OrderStatus::Resting}}}}, {}};
    const auto before = apex::book_hash(snapshot);
    auto altered = snapshot;
    altered.last_command_sequence = 2;
    EXPECT_NE(apex::book_hash(altered), before);
    altered = snapshot;
    altered.bids[0].orders[0].priority_sequence = 2;
    EXPECT_NE(apex::book_hash(altered), before);
    altered = snapshot;
    altered.bids[0].orders[0].remaining_quantity = 9;
    EXPECT_NE(apex::book_hash(altered), before);
}
