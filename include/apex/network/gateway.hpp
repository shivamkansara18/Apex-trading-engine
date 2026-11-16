#pragma once

#include "apex/journal/journal.hpp"

#include <memory>
#include <chrono>
#include <stop_token>

namespace apex::network {
struct GatewayConfig {
    std::uint16_t port{9000}; // Zero selects an ephemeral loopback port.
    std::uint32_t participants{16};
    std::size_t max_connections{64};
    std::size_t inbox_capacity{256};
    std::size_t output_capacity_bytes{1024 * 1024}; // Per connection, pending bytes.
    std::size_t snapshot_depth{10};
    std::chrono::milliseconds handshake_timeout{5000};
    std::chrono::milliseconds idle_timeout{60000};
    std::chrono::milliseconds partial_frame_timeout{5000};
    RiskLimits limits{1'000'000, 1'000'000'000'000ULL, 1000, 1, 1'000'000'000, true};
    std::filesystem::path journal_path; // Optional new-file-only synchronous audit.
};

struct GatewayStats {
    std::uint64_t frames_processed{}, frames_discarded{}, output_overflows{}, deadline_disconnects{};
    std::size_t inbox_size{}, inbox_high_watermark{}, inbox_rejections{};
};

// One poll/event-loop thread owns network sessions, ordered inbox and engine.
// No socket thread or consumer can mutate the book. Bind is always 127.0.0.1.
class TcpGateway {
public:
    explicit TcpGateway(GatewayConfig config = {});
    ~TcpGateway();
    TcpGateway(const TcpGateway&) = delete;
    TcpGateway& operator=(const TcpGateway&) = delete;
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] GatewayStats stats() const; // Thread-safe observational snapshot.
    void run(std::stop_token stop = {}); // Call once; stop token is the only cross-thread control.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace apex::network
