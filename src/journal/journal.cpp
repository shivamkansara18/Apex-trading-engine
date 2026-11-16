#include "apex/journal/journal.hpp"
#include "codec.hpp"

#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace apex {
namespace {
void write_all(int fd, const journal_detail::Bytes& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR) { continue; }
        if (written < 0) { throw std::system_error(errno, std::generic_category(), "Journal write failed"); }
        if (written == 0) { throw std::runtime_error("Journal write made no progress"); }
        offset += static_cast<std::size_t>(written);
    }
    int result;
    do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
    if (result < 0) { throw std::system_error(errno, std::generic_category(), "Journal sync failed"); }
}
}
JournaledEngine::JournaledEngine(const std::filesystem::path& path, EngineOptions options) : engine_(options) {
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd_ < 0) { throw std::system_error(errno, std::generic_category(), "Cannot create new journal"); }
    try {
        journal_detail::Encoder header;
        header.bytes.insert(header.bytes.end(), std::begin(journal_detail::magic), std::end(journal_detail::magic));
        header.u32(1);
        write_all(fd_, header.bytes);
    } catch (...) {
        ::close(fd_); fd_ = -1; throw;
    }
}
JournaledEngine::~JournaledEngine() { if (fd_ >= 0) { ::close(fd_); } }

void JournaledEngine::append(const std::vector<std::uint8_t>& payload) {
    if (payload.empty() || payload.size() > journal_detail::max_payload) {
        throw std::length_error("Journal payload exceeds 16 MiB limit");
    }
    if (record_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Journal record sequence exhausted");
    }
    journal_detail::Encoder body;
    body.u64(++record_sequence_);
    body.bytes.insert(body.bytes.end(), payload.begin(), payload.end());
    journal_detail::Encoder record;
    record.u32(static_cast<std::uint32_t>(payload.size()));
    record.bytes.insert(record.bytes.end(), body.bytes.begin(), body.bytes.end());
    record.u32(journal_detail::crc32(body.bytes));
    write_all(fd_, record.bytes);
}
JournalResult JournaledEngine::execute(const JournalInput& input) {
    if (failed_ || finished_) { throw std::logic_error("Journal session is failed or finished"); }
    try {
        journal_detail::Encoder payload;
        journal_detail::encode_input(payload, input);
        auto result = journal_detail::apply(engine_, input);
        const auto response = journal_detail::encode_result(result, engine_.snapshot());
        payload.bytes.insert(payload.bytes.end(), response.begin(), response.end());
        append(payload.bytes); // Persist before returning success/events to the caller.
        return result;
    } catch (...) {
        failed_ = true;
        throw;
    }
}
void JournaledEngine::finish() {
    if (failed_ || finished_) { throw std::logic_error("Journal session is failed or finished"); }
    try {
        append({journal_detail::seal_tag});
        const auto fd = fd_;
        fd_ = -1;
        if (::close(fd) < 0) { throw std::system_error(errno, std::generic_category(), "Journal close failed"); }
        finished_ = true;
    } catch (...) {
        failed_ = true;
        throw;
    }
}
} // namespace apex
