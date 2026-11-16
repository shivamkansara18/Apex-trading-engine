#include "apex/network/client.hpp"
#include "socket.hpp"

#include <array>
#include <algorithm>
#include <climits>
#include <netinet/in.h>
#include <poll.h>

namespace apex::network {
namespace {
void wait_socket(int fd, short events, std::chrono::steady_clock::time_point deadline) {
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { throw SocketTimeout(); }
        struct pollfd descriptor {fd, events, 0};
        const auto ready = ::poll(&descriptor, 1, static_cast<int>(std::min<std::int64_t>(left, INT_MAX)));
        if (ready < 0 && errno == EINTR) { continue; }
        if (ready < 0) { throw std::system_error(errno, std::generic_category(), "Socket poll failed"); }
        if (ready == 0) { throw SocketTimeout(); }
        if (descriptor.revents & (POLLERR | POLLNVAL)) { throw std::runtime_error("TCP socket failure"); }
        if (descriptor.revents & (events | POLLHUP)) { return; }
    }
}
}
TcpClient::TcpClient(std::uint16_t port, std::chrono::milliseconds timeout) : timeout_(timeout) {
    if (port == 0 || timeout.count() <= 0) { throw std::invalid_argument("Invalid port or timeout"); }
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) { throw std::system_error(errno, std::generic_category(), "Socket creation failed"); }
    try {
        detail::configure_socket(fd_);
        struct sockaddr_in address {};
        address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
        if (::connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
            if (errno != EINPROGRESS) { throw std::system_error(errno, std::generic_category(), "Loopback connect failed"); }
            wait_socket(fd_, POLLOUT, std::chrono::steady_clock::now() + timeout_);
            int error = 0; socklen_t length = sizeof(error);
            if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error != 0) {
                throw std::system_error(error ? error : errno, std::generic_category(), "Connect failed");
            }
        }
    } catch (...) { ::close(fd_); fd_ = -1; throw; }
}
TcpClient::~TcpClient() { if (fd_ >= 0) { ::close(fd_); } }
void TcpClient::send(const Frame& frame) {
    const auto bytes = encode_frame(frame);
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        wait_socket(fd_, POLLOUT, deadline);
        const auto sent = ::send(fd_, bytes.data() + offset, bytes.size() - offset, detail::send_flags());
        if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) { continue; }
        if (sent <= 0) { throw std::runtime_error("TCP send failed/disconnected"); }
        offset += static_cast<std::size_t>(sent);
    }
}
Frame TcpClient::receive() {
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    while (pending_.empty()) {
        wait_socket(fd_, POLLIN, deadline);
        std::array<std::uint8_t, 16 * 1024> buffer{};
        const auto received = ::recv(fd_, buffer.data(), buffer.size(), 0);
        if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) { continue; }
        if (received < 0) { throw std::runtime_error("TCP receive failed"); }
        if (received == 0) { throw std::runtime_error(decoder_.partial() ? "Truncated frame at disconnect" : "TCP peer disconnected"); }
        for (auto& frame : decoder_.append(std::span(buffer).first(static_cast<std::size_t>(received)))) {
            pending_.push_back(std::move(frame));
        }
    }
    auto frame = std::move(pending_.front()); pending_.pop_front(); return frame;
}
void TcpClient::shutdown_write() {
    if (::shutdown(fd_, SHUT_WR) < 0) { throw std::system_error(errno, std::generic_category(), "Shutdown failed"); }
}
} // namespace apex::network
