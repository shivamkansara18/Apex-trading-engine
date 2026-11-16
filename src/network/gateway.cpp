#include "apex/network/gateway.hpp"
#include "apex/network/protocol.hpp"
#include "socket.hpp"
#include "apex/concurrency/bounded_queue.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <netinet/in.h>
#include <poll.h>

namespace apex::network {
struct TcpGateway::Impl {
    struct Peer {
        explicit Peer(int fd) : socket(fd) {}
        detail::Socket socket;
        FrameDecoder decoder;
        std::chrono::steady_clock::time_point connected{std::chrono::steady_clock::now()}, activity{connected}, partial_since{connected};
        std::deque<Bytes> output;
        std::size_t output_offset{}, output_bytes{}, queued{};
        std::uint64_t last_request{};
        ParticipantId participant{};
        bool identified{}, subscribed{}, read_closed{}, closing{}, abort{};
    };
    struct Pending { std::uint64_t peer; Frame frame; };
    GatewayConfig config;
    detail::Socket listener;
    std::uint16_t bound_port{};
    std::map<std::uint64_t, std::unique_ptr<Peer>> peers;
    BoundedQueue<Pending> inbox;
    std::atomic<std::uint64_t> processed{}, discarded{}, overflows{}, timeouts{};
    std::uint64_t next_peer{}, feed_sequence{};
    CommandSequence last_command{};
    std::atomic<bool> started{false};
    MatchingEngine engine;
    std::unique_ptr<JournaledEngine> journal;

    explicit Impl(GatewayConfig settings) : config(std::move(settings)), inbox(config.inbox_capacity) {
        if (config.participants == 0 || config.participants > 32 || config.max_connections == 0 || config.max_connections > 256 ||
            config.inbox_capacity == 0 || config.inbox_capacity > 4096 || config.output_capacity_bytes < max_frame_size ||
            config.output_capacity_bytes > 16 * 1024 * 1024 || config.snapshot_depth == 0 || config.snapshot_depth > 64 ||
            config.limits.max_open_orders > 1000 || config.limits.max_order_quantity > 1'000'000) {
            throw std::invalid_argument("Invalid gateway resource limits");
        }
        if (config.handshake_timeout.count() <= 0 || config.idle_timeout.count() <= 0 || config.partial_frame_timeout.count() <= 0) {
            throw std::invalid_argument("Gateway deadlines must be positive");
        }
        RiskManager validation;
        (void)validation.register_participant(1, config.limits);
        listener.fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener.fd < 0) { throw std::system_error(errno, std::generic_category(), "Listener socket failed"); }
        detail::configure_socket(listener.fd);
        const int enabled = 1;
        if (::setsockopt(listener.fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0) {
            throw std::system_error(errno, std::generic_category(), "Listener configuration failed");
        }
        struct sockaddr_in address {};
        address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(config.port);
        if (::bind(listener.fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0 ||
            ::listen(listener.fd, 64) < 0) { throw std::system_error(errno, std::generic_category(), "Loopback bind/listen failed"); }
        socklen_t length = sizeof(address);
        if (::getsockname(listener.fd, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
            throw std::system_error(errno, std::generic_category(), "Cannot obtain listener port");
        }
        bound_port = ntohs(address.sin_port);
        if (!config.journal_path.empty()) { journal = std::make_unique<JournaledEngine>(config.journal_path); }
        for (ParticipantId id = 1; id <= config.participants; ++id) {
            const bool ok = journal ? journal->register_participant(id, config.limits) : engine.register_participant(id, config.limits);
            if (!ok) { throw std::logic_error("Gateway registration failed"); }
        }
    }
    const MatchingEngine& matching() const { return journal ? journal->engine() : engine; }
    void queue(Peer& peer, const Frame& frame) {
        if (peer.abort) { return; }
        auto bytes = encode_frame(frame);
        if (bytes.size() > config.output_capacity_bytes - peer.output_bytes) { ++overflows; peer.abort = true; return; }
        peer.output_bytes += bytes.size(); peer.output.push_back(std::move(bytes));
    }
    void fail(Peer& peer, std::uint64_t sequence, ErrorCode code, std::string_view message) {
        queue(peer, {MessageType::Error, sequence, encode_error(code, message)}); peer.closing = true;
    }
    void snapshot(Peer& peer, std::uint64_t request) {
        queue(peer, {MessageType::BookSnapshot, request,
                    encode_book(book_view(matching().book(), last_command, feed_sequence, config.snapshot_depth))});
    }
    void accept_peers() {
        for (int attempt = 0; attempt < 32; ++attempt) {
            const int fd = ::accept(listener.fd, nullptr, nullptr);
            if (fd < 0 && errno == EINTR) { continue; }
            if (fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { return; }
            if (fd < 0) { throw std::system_error(errno, std::generic_category(), "Accept failed"); }
            detail::Socket guard(fd);
            if (peers.size() >= config.max_connections) { continue; }
            detail::configure_socket(fd);
            if (next_peer == std::numeric_limits<std::uint64_t>::max()) { throw std::overflow_error("Connection sequence exhausted"); }
            auto peer = std::make_unique<Peer>(fd); guard.fd = -1;
            peers.emplace(++next_peer, std::move(peer));
        }
    }
    void read_peer(std::uint64_t id, Peer& peer) {
        std::array<std::uint8_t, 16 * 1024> buffer{};
        const auto received = ::recv(peer.socket.fd, buffer.data(), buffer.size(), 0);
        if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) { return; }
        if (received < 0) { peer.abort = true; return; }
        if (received == 0) {
            peer.read_closed = true;
            if (peer.decoder.partial()) { fail(peer, 0, ErrorCode::Malformed, "Truncated frame at EOF"); }
            return;
        }
        peer.activity = std::chrono::steady_clock::now();
        const bool was_partial = peer.decoder.partial();
        std::vector<Frame> frames;
        try { frames = peer.decoder.append(std::span(buffer).first(static_cast<std::size_t>(received))); }
        catch (const std::runtime_error& error) { fail(peer, 0, ErrorCode::Malformed, error.what()); return; }
        if (peer.decoder.partial() && (!was_partial || !frames.empty())) { peer.partial_since = peer.activity; }
        for (auto& frame : frames) {
            const auto sequence = frame.request_sequence;
            if (!inbox.try_push({id, std::move(frame)})) {
                fail(peer, sequence, ErrorCode::Overloaded, "Command inbox is full"); return;
            }
            ++peer.queued;
        }
    }
    void flush(Peer& peer) {
        std::size_t budget = 16 * 1024;
        while (!peer.output.empty() && budget && !peer.abort) {
            const auto& bytes = peer.output.front();
            const auto size = std::min(budget, bytes.size() - peer.output_offset);
            const auto sent = ::send(peer.socket.fd, bytes.data() + peer.output_offset, size, detail::send_flags());
            if (sent < 0 && errno == EINTR) { continue; }
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { return; }
            if (sent <= 0) { peer.abort = true; return; }
            const auto count = static_cast<std::size_t>(sent);
            budget -= count; peer.output_bytes -= count; peer.output_offset += count;
            if (peer.output_offset == bytes.size()) { peer.output.pop_front(); peer.output_offset = 0; }
        }
    }
    static bool relevant(const Event& event, ParticipantId participant) {
        return std::visit([&](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, Trade>) { return payload.buyer_id == participant || payload.seller_id == participant; }
            else if constexpr (std::is_same_v<T, CommandRejected>) { return false; }
            else { return payload.order.participant_id == participant; }
        }, event.payload);
    }
    void handle(std::uint64_t id, Peer& peer, const Frame& frame) {
        if (peer.last_request == std::numeric_limits<std::uint64_t>::max() || frame.request_sequence != peer.last_request + 1) {
            fail(peer, frame.request_sequence, ErrorCode::Sequence, "Request sequences must start at 1 and increase consecutively"); return;
        }
        peer.last_request = frame.request_sequence;
        if (frame.type == MessageType::Hello) {
            if (peer.identified) { fail(peer, frame.request_sequence, ErrorCode::Identity, "Session identity is immutable"); return; }
            ParticipantId participant;
            try { participant = decode_participant(frame.payload); }
            catch (const std::runtime_error& error) { fail(peer, frame.request_sequence, ErrorCode::Malformed, error.what()); return; }
            if (participant > config.participants) { fail(peer, frame.request_sequence, ErrorCode::Identity, "Participant is not registered"); return; }
            for (const auto& [other_id, other] : peers) {
                if (participant && other_id != id && other->identified && other->participant == participant &&
                    !other->read_closed && !other->closing && !other->abort) {
                    fail(peer, frame.request_sequence, ErrorCode::SessionInUse, "Participant already has a writable session"); return;
                }
            }
            peer.identified = true; peer.participant = participant;
            queue(peer, {MessageType::HelloAck, frame.request_sequence, frame.payload}); return;
        }
        if (!peer.identified) { fail(peer, frame.request_sequence, ErrorCode::Identity, "HELLO is required first"); return; }
        if (frame.type == MessageType::NewOrder || frame.type == MessageType::CancelOrder || frame.type == MessageType::ReplaceOrder) {
            Command command;
            try { command = decode_command(frame); }
            catch (const std::runtime_error& error) { fail(peer, frame.request_sequence, ErrorCode::Malformed, error.what()); return; }
            const auto participant = std::visit([](const auto& request) { return request.participant_id; }, command);
            if (peer.participant == 0 || participant != peer.participant) {
                fail(peer, frame.request_sequence, ErrorCode::Identity, "Request participant differs from session identity"); return;
            }
            if (feed_sequence >= std::numeric_limits<std::uint64_t>::max() - matching().book().order_count() - 1) {
                throw std::overflow_error("Market-data sequence exhausted");
            }
            const auto events = journal ? journal->process(command) : engine.process(command);
            if (events.empty() || events.size() > 100'000) { throw std::logic_error("Invalid engine event count"); }
            last_command = events.front().command_sequence;
            const bool accepted = !std::holds_alternative<CommandRejected>(events.front().payload);
            queue(peer, {MessageType::CommandAck, frame.request_sequence,
                         encode_ack({last_command, static_cast<std::uint32_t>(events.size()), accepted})});
            for (const auto& event : events) {
                const auto payload = encode_event(event);
                queue(peer, {MessageType::ExecutionReport, frame.request_sequence, payload});
                for (auto& [other_id, other] : peers) {
                    if (other_id != id && other->identified && !other->closing && relevant(event, other->participant)) {
                        queue(*other, {MessageType::ExecutionReport, 0, payload});
                    }
                }
                if (std::holds_alternative<Trade>(event.payload)) {
                    const auto feed = encode_trade_update(++feed_sequence, event);
                    for (auto& [unused, other] : peers) {
                        (void)unused;
                        if (other->subscribed && !other->closing) { queue(*other, {MessageType::TradeUpdate, 0, feed}); }
                    }
                }
            }
            ++feed_sequence;
            const auto book = encode_book(book_view(matching().book(), last_command, feed_sequence, config.snapshot_depth));
            for (auto& [unused, other] : peers) {
                (void)unused;
                if (other->subscribed && !other->closing) { queue(*other, {MessageType::BookSnapshot, 0, book}); }
            }
            return;
        }
        if (!frame.payload.empty()) { fail(peer, frame.request_sequence, ErrorCode::Malformed, "Control request payload must be empty"); return; }
        switch (frame.type) {
        case MessageType::Subscribe:
            peer.subscribed = true;
            queue(peer, {MessageType::SubscribeAck, frame.request_sequence, encode_sequence(feed_sequence)});
            snapshot(peer, frame.request_sequence); break;
        case MessageType::SnapshotRequest: snapshot(peer, frame.request_sequence); break;
        case MessageType::OpenOrdersRequest: {
            if (peer.participant == 0) { fail(peer, frame.request_sequence, ErrorCode::Identity, "Observers cannot request private orders"); break; }
            OpenOrders snapshot{last_command, peer.participant, {}};
            for (const auto side : {Side::Buy, Side::Sell}) {
                matching().book().visit_orders(side, [&](const Order& order) {
                    if (order.participant_id == peer.participant) { snapshot.orders.push_back(order); }
                    return true;
                });
            }
            queue(peer, {MessageType::OpenOrdersSnapshot, frame.request_sequence, encode_open_orders(snapshot)}); break;
        }
        case MessageType::Heartbeat: queue(peer, {MessageType::HeartbeatAck, frame.request_sequence, {}}); break;
        default: fail(peer, frame.request_sequence, ErrorCode::UnknownMessage, "Unknown request message type"); break;
        }
    }
    void process_inbox() {
        for (int budget = 0; budget < 64; ++budget) {
            auto item = inbox.try_pop();
            if (!item) { break; }
            auto pending = std::move(*item);
            const auto found = peers.find(pending.peer);
            if (found == peers.end()) { ++discarded; continue; }
            auto& peer = *found->second; --peer.queued;
            if (!peer.abort && !peer.closing) { ++processed; handle(pending.peer, peer, pending.frame); }
            else { ++discarded; }
        }
    }
    void cleanup() {
        for (auto it = peers.begin(); it != peers.end();) {
            const auto& peer = *it->second;
            if (peer.abort || ((peer.closing || peer.read_closed) && peer.output.empty() && peer.queued == 0)) { it = peers.erase(it); }
            else { ++it; }
        }
    }
    void run(std::stop_token stop) {
        if (started.exchange(true)) { throw std::logic_error("Gateway run() may only be called once"); }
        while (!stop.stop_requested()) {
            std::vector<struct pollfd> descriptors{{listener.fd, POLLIN, 0}};
            std::vector<std::uint64_t> ids;
            for (const auto& [id, peer] : peers) {
                short events = 0;
                if (!peer->read_closed && !peer->closing) { events |= POLLIN; }
                if (!peer->output.empty()) { events |= POLLOUT; }
                descriptors.push_back({peer->socket.fd, events, 0}); ids.push_back(id);
            }
            const auto ready = ::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), 20);
            if (ready < 0 && errno == EINTR) { continue; }
            if (ready < 0) { throw std::system_error(errno, std::generic_category(), "Gateway poll failed"); }
            if (descriptors[0].revents & POLLIN) { accept_peers(); }
            for (std::size_t index = 0; index < ids.size(); ++index) {
                auto& peer = *peers.at(ids[index]); const auto events = descriptors[index + 1].revents;
                if (events & (POLLERR | POLLNVAL)) { peer.abort = true; continue; }
                if (!peer.closing && !peer.read_closed && (events & (POLLIN | POLLHUP))) { read_peer(ids[index], peer); }
                if (events & POLLOUT) { flush(peer); }
            }
            process_inbox();
            const auto now = std::chrono::steady_clock::now();
            for (auto& [id, peer] : peers) {
                (void)id;
                if (!peer->abort && !peer->closing &&
                    ((!peer->identified && now - peer->connected >= config.handshake_timeout) ||
                     now - peer->activity >= config.idle_timeout ||
                     (peer->decoder.partial() && now - peer->partial_since >= config.partial_frame_timeout))) {
                    peer->abort = true; ++timeouts;
                }
            }
            cleanup();
        }
        while (inbox.stats().size != 0) { process_inbox(); }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (std::chrono::steady_clock::now() < deadline) {
            bool pending = false;
            for (auto& [id, peer] : peers) { (void)id; flush(*peer); pending |= !peer->abort && !peer->output.empty(); }
            if (!pending) { break; }
            (void)::poll(nullptr, 0, 1);
        }
        if (journal) { journal->finish(); }
        peers.clear();
    }
};
TcpGateway::TcpGateway(GatewayConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
TcpGateway::~TcpGateway() = default;
std::uint16_t TcpGateway::port() const noexcept { return impl_->bound_port; }
GatewayStats TcpGateway::stats() const {
    const auto queue = impl_->inbox.stats();
    return {impl_->processed.load(), impl_->discarded.load(), impl_->overflows.load(), impl_->timeouts.load(),
            queue.size, queue.high_watermark, queue.rejected};
}
void TcpGateway::run(std::stop_token stop) { impl_->run(stop); }
} // namespace apex::network
