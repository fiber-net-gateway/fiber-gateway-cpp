#include <fiber/quic/QuicSendScheduler.h>

#include <array>
#include <expected>
#include <limits>

#include <fiber/common/Assert.h>
#include <fiber/quic/QuicUdpEndpoint.h>

namespace fiber::quic {

namespace {

inline constexpr std::size_t kQuicMaxGsoPayloadSize = 65487;

[[nodiscard]] QuicSendBuildState make_build_state(QuicConnection &connection) noexcept {
    QuicSendBuildState state{};
    state.congestion = connection.congestion();
    auto &paths = connection.paths().paths();
    for (std::size_t i = 0; i < paths.size(); ++i) {
        state.paths[i].path = &paths[i];
        state.paths[i].sent = paths[i].sent;
        state.paths[i].ecn_validation_sent = paths[i].ecn_validation_sent;
    }
    return state;
}

[[nodiscard]] QuicSendPathReservation &find_reservation(QuicSendBuildState &state, QuicPath &path) noexcept {
    for (QuicSendPathReservation &reservation: state.paths) {
        if (reservation.path == &path) {
            return reservation;
        }
    }
    FIBER_ASSERT(false);
    return state.paths[0];
}

void reserve_datagram(QuicConnection &connection, QuicSendBuildState &state,
                      const QuicSendDatagram &datagram) noexcept {
    if (datagram.path != nullptr) {
        QuicSendPathReservation &reservation = find_reservation(state, *datagram.path);
        reservation.sent = datagram.length > std::numeric_limits<std::uint64_t>::max() - reservation.sent
                                   ? std::numeric_limits<std::uint64_t>::max()
                                   : reservation.sent + datagram.length;
        if (datagram.spec.ecn != net::UdpEcn::Unspecified && datagram.path->ecn_state == QuicEcnState::Testing) {
            for (std::size_t i = 0; i < datagram.packet_count; ++i) {
                if (datagram.packets[i].ack_eliciting && reservation.ecn_validation_sent != UINT32_MAX) {
                    ++reservation.ecn_validation_sent;
                }
            }
        }
    }
    if (datagram.pacing_controlled) {
        quic_pacer_on_datagram_sent(state.pacer, datagram.length);
    }
    if (!connection.closing()) {
        for (std::size_t i = 0; i < datagram.packet_count; ++i) {
            const QuicSendPacketRecord &packet = datagram.packets[i];
            quic_congestion_on_packet_sent(state.congestion, packet.length, packet.ack_eliciting, false);
        }
    }
}

[[nodiscard]] std::size_t datagram_packet_count(const QuicSendDatagram *datagrams, std::size_t count) noexcept {
    std::size_t packets = 0;
    for (std::size_t i = 0; i < count; ++i) {
        packets += datagrams[i].packet_count;
    }
    return packets;
}

[[nodiscard]] bool packet_space_has_work(const QuicPacketNumberSpace &space) noexcept {
    return (space.send_ack && space.pending_ack != kUnsetPacketNumber) || !space.pending_frames.empty();
}

[[nodiscard]] bool gso_datagram_eligible(const QuicSendDatagram &datagram) noexcept {
    return datagram.path != nullptr && datagram.path->validated && !datagram.mtu_probe && datagram.pacing_controlled &&
           datagram.packet_count == 1 && datagram.packets[0].level == QuicEncryptionLevel::Application &&
           datagram.packets[0].ack_eliciting;
}

struct QuicSendMessageBatch {
    std::array<net::UdpPacketSendSpec, net::kUdpMaxBatchSize> specs{};
    std::array<std::size_t, net::kUdpMaxBatchSize> datagram_ends{};
    std::size_t count = 0;
};

[[nodiscard]] QuicSendMessageBatch make_send_messages(const QuicSendDatagram *datagrams, std::size_t datagram_count,
                                                      bool allow_gso, std::size_t max_gso_segments) noexcept {
    QuicSendMessageBatch messages{};
    std::size_t index = 0;
    while (index < datagram_count) {
        const QuicSendDatagram &first = datagrams[index];
        std::size_t end = index + 1;
        std::size_t total = first.length;

        if (allow_gso && gso_datagram_eligible(first) && first.length == first.path->mtu && first.length != 0 &&
            first.length <= UINT16_MAX && first.length <= kQuicMaxGsoPayloadSize) {
            while (end < datagram_count && end - index < max_gso_segments) {
                const QuicSendDatagram &next = datagrams[end];
                if (!gso_datagram_eligible(next) || next.path != first.path || next.spec.ecn != first.spec.ecn ||
                    next.data != first.data + total || next.length > first.length ||
                    next.length > kQuicMaxGsoPayloadSize - total) {
                    break;
                }
                total += next.length;
                ++end;
                if (next.length < first.length) {
                    break;
                }
            }
        }

        net::UdpPacketSendSpec spec = first.spec;
        if (end - index >= 3) {
            spec.buf = first.data;
            spec.len = total;
            spec.gso_segment_size = static_cast<std::uint16_t>(first.length);
        } else {
            end = index + 1;
        }
        messages.specs[messages.count] = spec;
        messages.datagram_ends[messages.count] = end;
        ++messages.count;
        index = end;
    }
    return messages;
}

[[nodiscard]] bool gso_retryable_error(common::IoErr error) noexcept {
    return error == common::IoErr::NotSupported || error == common::IoErr::Invalid ||
           error == common::IoErr::MessageTooLarge;
}

[[nodiscard]] bool gso_unavailable_error(common::IoErr error) noexcept {
    return error == common::IoErr::NotSupported || error == common::IoErr::Invalid;
}

} // namespace

QuicSendScheduler::QuicSendScheduler() noexcept = default;

QuicSendScheduler::~QuicSendScheduler() { close(); }

common::IoResult<void> QuicSendScheduler::init(event::EventLoop &loop, net::UdpSocket &socket,
                                               QuicUdpEndpoint &endpoint, const Options &options) noexcept {
    if (initialized_ || options.max_datagrams_per_batch == 0 || options.max_gso_segments == 0 ||
        options.max_datagrams_per_batch > net::kUdpMaxBatchSize || options.max_packets_per_wakeup == 0 ||
        options.max_gso_segments > net::kUdpMaxBatchSize || options.max_packets_per_connection == 0 ||
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
    gso_enabled_ = FIBER_HAVE_UDP_SEGMENT && options.enable_gso;
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
    gso_enabled_ = false;
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
        std::array<QuicSendDatagram, net::kUdpMaxBatchSize> datagrams{};
        std::array<net::UdpPacketSendSpec, net::kUdpMaxBatchSize> specs{};
        QuicSendBuildState build_state = make_build_state(connection);
        const bool allow_gso = gso_enabled_ && connection.state() == QuicConnectionState::Established &&
                               !connection.has_path_send_work() &&
                               !packet_space_has_work(connection.packet_number_space(QuicEncryptionLevel::Initial)) &&
                               !packet_space_has_work(connection.packet_number_space(QuicEncryptionLevel::Handshake));

        const QuicPath *initial_path = connection.active_path();
        const std::size_t initial_mtu = initial_path != nullptr ? initial_path->mtu : kQuicCongestionMinInitialSize;
        (void) quic_pacer_check(connection.pacer_, options_.pacing, connection.congestion(), connection.rtt(),
                                initial_mtu, loop_->now());
        build_state.pacer = connection.pacer_;

        std::size_t datagram_count = 0;
        std::size_t arena_used = 0;
        std::size_t batch_packets = 0;
        QuicBuildSendStatus terminal_status = QuicBuildSendStatus::Encoded;
        bool pacing_delayed = false;
        std::chrono::steady_clock::time_point pacing_deadline{};

        while (datagram_count < options_.max_datagrams_per_batch && arena_used < options_.send_buffer_size &&
               result.packets_sent + batch_packets < options_.max_packets_per_connection) {
            const QuicPath *active_path = connection.active_path();
            const std::size_t path_mtu = active_path != nullptr ? active_path->mtu : kQuicCongestionMinInitialSize;
            const auto pacing = quic_pacer_check(build_state.pacer, options_.pacing, build_state.congestion,
                                                 connection.rtt(), path_mtu, loop_->now());
            const QuicBuildMode mode = pacing.ready ? QuicBuildMode::Normal : QuicBuildMode::PacingExemptOnly;

            QuicSendDatagram &datagram = datagrams[datagram_count];
            datagram.data = endpoint_->send_buffer_.get() + arena_used;
            datagram.capacity = options_.send_buffer_size - arena_used;

            auto built = endpoint_->build_send_datagram(connection, datagram, build_state, mode);
            if (!built) {
                terminal_status = QuicBuildSendStatus::Closed;
                result.error = built.error();
                break;
            }
            terminal_status = built->status;
            if (built->status != QuicBuildSendStatus::Encoded) {
                if (built->status == QuicBuildSendStatus::NoWork && !pacing.ready &&
                    endpoint_->connection_has_send_work(connection)) {
                    pacing_delayed = true;
                    pacing_deadline = pacing.deadline;
                }
                break;
            }

            FIBER_ASSERT(datagram.length != 0);
            FIBER_ASSERT(mode == QuicBuildMode::Normal || !datagram.pacing_controlled);
            specs[datagram_count] = datagram.spec;
            arena_used += datagram.length;
            batch_packets += datagram.packet_count;
            reserve_datagram(connection, build_state, datagram);
            ++datagram_count;
        }

        if (datagram_count == 0) {
            endpoint_->finish_send_batch(connection);
            remove(connection);
            if (pacing_delayed) {
                connection.arm_pacing_timer(pacing_deadline);
            }
            if (result.error == common::IoErr::None && terminal_status == QuicBuildSendStatus::Encoded &&
                endpoint_->connection_has_send_work(connection)) {
                result.error = common::IoErr::NoMem;
            }
            return result;
        }

        QuicSendMessageBatch messages =
                make_send_messages(datagrams.data(), datagram_count, allow_gso, options_.max_gso_segments);
        auto sent = socket_->try_send_packets(messages.specs.data(), messages.count);
        bool used_individual_fallback = false;
        if (!sent && messages.count != 0 && messages.specs[0].gso_segment_size != 0 &&
            gso_retryable_error(sent.error())) {
            if (gso_unavailable_error(sent.error())) {
                gso_enabled_ = false;
            }
            sent = socket_->try_send_packets(specs.data(), datagram_count);
            used_individual_fallback = true;
        }
        if (!sent) {
            for (std::size_t i = datagram_count; i != 0; --i) {
                endpoint_->rollback_send_datagram(connection, datagrams[i - 1]);
            }
            endpoint_->finish_send_batch(connection);
            if (sent.error() == common::IoErr::MessageTooLarge && datagrams[0].mtu_probe &&
                datagrams[0].path != nullptr) {
                const QuicTime now = loop_ != nullptr ? quic_time_ms(loop_->now()) : QuicTime{0};
                auto handled = connection.paths().handle_mtu_probe_send_failed(*datagrams[0].path, now);
                if (!handled) {
                    result.error = handled.error();
                    return result;
                }
                if (*handled) {
                    continue;
                }
            }
            result.error = sent.error();
            return result;
        }

        const std::size_t sent_message_count = *sent;
        FIBER_ASSERT(sent_message_count <= (used_individual_fallback ? datagram_count : messages.count));
        const std::size_t sent_count =
                used_individual_fallback
                        ? sent_message_count
                        : (sent_message_count == 0 ? 0 : messages.datagram_ends[sent_message_count - 1]);
        FIBER_ASSERT(sent_count <= datagram_count);
        for (std::size_t i = 0; i < sent_count; ++i) {
            if (datagrams[i].pacing_controlled) {
                quic_pacer_on_datagram_sent(connection.pacer_, datagrams[i].length);
            }
            endpoint_->commit_send_datagram(connection, datagrams[i]);
        }
        for (std::size_t i = datagram_count; i > sent_count; --i) {
            endpoint_->rollback_send_datagram(connection, datagrams[i - 1]);
        }
        endpoint_->finish_send_batch(connection);
        result.packets_sent += datagram_packet_count(datagrams.data(), sent_count);

        if (result.error != common::IoErr::None) {
            return result;
        }

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
        if (sent_count < datagram_count) {
            continue;
        }
    }
}

} // namespace fiber::quic
