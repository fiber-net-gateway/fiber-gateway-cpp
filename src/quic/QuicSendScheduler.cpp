#include "QuicSendScheduler.h"

#include <expected>

#include "../common/Assert.h"
#include "QuicUdpEndpoint.h"

namespace fiber::quic {

QuicSendScheduler::QuicSendScheduler() noexcept = default;

QuicSendScheduler::~QuicSendScheduler() { close(); }

common::IoResult<void> QuicSendScheduler::init(event::EventLoop &loop, net::UdpSocket &socket,
                                               QuicUdpEndpoint &endpoint, const Options &options) noexcept {
    if (initialized_ || options.max_packets_per_wakeup == 0 || options.max_packets_per_connection == 0 ||
        (options.pacing.enabled && (options.pacing.rate_numerator == 0 || options.pacing.rate_denominator == 0 ||
                                    options.pacing.max_burst_packets == 0 || options.pacing.max_burst_packets > 64 ||
                                    options.pacing.timer_granularity.count() < 0))) {
        return std::unexpected(common::IoErr::Invalid);
    }

    loop_ = &loop;
    socket_ = &socket;
    endpoint_ = &endpoint;
    options_ = options;
    stop_reason_ = common::IoErr::None;
    closing_ = false;
    initialized_ = true;
    return {};
}

void QuicSendScheduler::submit(QuicConnection &connection) noexcept {
    if (!initialized_ || closing_) {
        return;
    }
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    // Ordinary work remains pending until the existing pacing timer fires. Only frames that are exempt from pacing
    // may requeue the connection early.
    if (connection.pacing_timer_armed() && !connection.has_pacing_exempt_send_work()) {
        return;
    }
    enqueue_ready(connection);
}

void QuicSendScheduler::remove(QuicConnection &connection) noexcept {
    auto &entry = connection.send_queue_entry;
    if (entry.link.linked()) {
        ready_.erase(entry);
    }
    entry.connection = nullptr;
}

void QuicSendScheduler::close(common::IoErr reason) noexcept {
    if (!initialized_) {
        return;
    }
    if (stop_reason_ == common::IoErr::None) {
        stop_reason_ = reason;
    }
    closing_ = true;
    clear_ready();
    initialized_ = false;
    loop_ = nullptr;
    socket_ = nullptr;
    endpoint_ = nullptr;
}

QuicSendScheduler::PumpResult QuicSendScheduler::pump() noexcept {
    PumpResult pump_result{};
    if (!initialized_ || closing_) {
        return pump_result;
    }
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());

    std::size_t flushes = 0;
    while (!closing_ && has_work()) {
        QuicConnection *connection = front_ready();
        if (connection == nullptr) {
            break;
        }

        FlushResult result = flush_connection(*connection);
        ++flushes;
        pump_result.packets_sent += result.packets_sent;
        if (result.error == common::IoErr::WouldBlock) {
            pump_result.write_blocked = true;
            return pump_result;
        }
        if (result.error != common::IoErr::None) {
            connection->close(QuicErrorCode::InternalError);
            remove(*connection);
        }

        if (pump_result.packets_sent >= options_.max_packets_per_wakeup || flushes >= options_.max_packets_per_wakeup) {
            pump_result.needs_reschedule = has_work();
            return pump_result;
        }
    }
    return pump_result;
}

void QuicSendScheduler::enqueue_ready(QuicConnection &connection) noexcept {
    auto &entry = connection.send_queue_entry;
    if (entry.link.linked()) {
        return;
    }
    entry.connection = &connection;
    ready_.push_back(entry);
}

void QuicSendScheduler::rotate_front_to_back(QuicConnection &connection) noexcept {
    auto &entry = connection.send_queue_entry;
    if (!entry.link.linked() || ready_.front() != &entry || ready_.back() == &entry) {
        return;
    }
    ready_.erase(entry);
    ready_.push_back(entry);
}

QuicConnection *QuicSendScheduler::front_ready() noexcept {
    auto *entry = ready_.front();
    if (!entry) {
        return nullptr;
    }
    if (entry->connection == nullptr) {
        ready_.erase(*entry);
        return nullptr;
    }
    return entry->connection;
}

void QuicSendScheduler::clear_ready() noexcept {
    while (!ready_.empty()) {
        auto *entry = ready_.front();
        ready_.erase(*entry);
        entry->connection = nullptr;
    }
}

QuicSendScheduler::FlushResult QuicSendScheduler::flush_connection(QuicConnection &connection) noexcept {
    FIBER_ASSERT(endpoint_ != nullptr);
    FIBER_ASSERT(socket_ != nullptr);
    FIBER_ASSERT(connection.send_queue_entry.link.linked());

    FlushResult result{};
    connection.cancel_pacing_timer();
    for (;;) {
        const QuicPath *active_path = connection.active_path();
        const std::size_t path_mtu = active_path != nullptr ? active_path->mtu : kQuicCongestionMinInitialSize;
        const auto pacing = quic_pacer_check(connection.pacer_, options_.pacing, connection.congestion(),
                                             connection.rtt(), path_mtu, loop_->now());
        const QuicBuildMode mode = pacing.ready ? QuicBuildMode::Normal : QuicBuildMode::PacingExemptOnly;

        QuicSendDatagram datagram{};
        datagram.data = endpoint_->send_buffer_.get();
        datagram.capacity = options_.send_buffer_size;

        auto built = endpoint_->build_send_datagram(connection, datagram, mode);
        if (!built) {
            result.error = built.error();
            return result;
        }

        if (built->status == QuicBuildSendStatus::NoWork) {
            remove(connection);
            if (!pacing.ready && endpoint_->connection_has_send_work(connection)) {
                connection.arm_pacing_timer(pacing.deadline);
            }
            return result;
        }
        if (built->status == QuicBuildSendStatus::Closed || built->status == QuicBuildSendStatus::Blocked) {
            remove(connection);
            return result;
        }

        FIBER_ASSERT(mode == QuicBuildMode::Normal || !datagram.pacing_controlled);

        auto sent = socket_->try_send_packet(datagram.spec);
        if (!sent) {
            endpoint_->rollback_send_datagram(connection, datagram);
            if (sent.error() == common::IoErr::MessageTooLarge && datagram.mtu_probe && datagram.path != nullptr) {
                const QuicTime now = loop_ != nullptr ? quic_time_ms(loop_->now()) : QuicTime{0};
                auto handled = connection.paths().handle_mtu_probe_send_failed(*datagram.path, now);
                if (!handled) {
                    result.error = handled.error();
                    return result;
                }
                if (*handled) {
                    continue;
                }
            }
            if (sent.error() == common::IoErr::WouldBlock) {
                result.error = common::IoErr::WouldBlock;
                return result;
            }
            result.error = sent.error();
            return result;
        }

        if (datagram.pacing_controlled) {
            quic_pacer_on_datagram_sent(connection.pacer_, datagram.length);
        }
        endpoint_->commit_send_datagram(connection, datagram);
        result.packets_sent += datagram.packet_count;
        if (result.packets_sent >= options_.max_packets_per_connection) {
            if (endpoint_->connection_has_send_work(connection)) {
                rotate_front_to_back(connection);
            } else {
                remove(connection);
            }
            return result;
        }
        if (!endpoint_->connection_has_send_work(connection)) {
            remove(connection);
            return result;
        }
    }
}

} // namespace fiber::quic
