#pragma once

#include "apex/network/protocol.hpp"

#include <chrono>
#include <deque>
#include <stdexcept>

namespace apex::network {
class SocketTimeout : public std::runtime_error {
public:
    SocketTimeout() : std::runtime_error("TCP operation timed out") {}
};
class TcpClient {
public:
    explicit TcpClient(std::uint16_t port, std::chrono::milliseconds timeout = std::chrono::seconds(3));
    ~TcpClient();
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;
    void send(const Frame& frame);
    [[nodiscard]] Frame receive(); // Throws on timeout, disconnect or malformed frame.
    [[nodiscard]] bool has_pending() const noexcept { return !pending_.empty(); }
    void shutdown_write();
    [[nodiscard]] int native_handle() const noexcept { return fd_; }
private:
    int fd_{-1};
    std::chrono::milliseconds timeout_;
    FrameDecoder decoder_;
    std::deque<Frame> pending_;
};
} // namespace apex::network
