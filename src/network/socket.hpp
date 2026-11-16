#pragma once

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

namespace apex::network::detail {
struct Socket {
    int fd{-1};
    explicit Socket(int value = -1) : fd(value) {}
    ~Socket() { if (fd >= 0) { ::close(fd); } }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};
inline void configure_socket(int fd) {
    const auto flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        throw std::system_error(errno, std::generic_category(), "Cannot configure socket");
    }
    const int no_delay = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) < 0) {
        throw std::system_error(errno, std::generic_category(), "Cannot configure TCP_NODELAY");
    }
#ifdef __APPLE__
    const int enabled = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) {
        throw std::system_error(errno, std::generic_category(), "Cannot disable SIGPIPE");
    }
#endif
}
inline int send_flags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}
} // namespace apex::network::detail
