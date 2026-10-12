#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "quic/QuicCrypto.h"

#include <fiber/common/IoError.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/UdpSocket.h>
#include <fiber/quic/QuicConnection.h>
#include <fiber/quic/QuicSendScheduler.h>

#include "QuicTestLoop.h"

namespace {

using fiber::quic::QuicConnection;
using fiber::quic::QuicSendScheduler;

fiber::net::SocketAddress loopback(std::uint16_t port) { return {fiber::net::IpAddress::loopback_v4(), port}; }

fiber::quic::QuicConnectionId cid_from(std::initializer_list<std::uint8_t> bytes) {
    auto cid = fiber::quic::QuicConnectionId::from_bytes(bytes.begin(), bytes.size());
    return cid.value_or(fiber::quic::QuicConnectionId{});
}

// Deterministic, unpaced, non-GSO options: the pacer is always ready so every
// pump flushes whatever work the connections hold.
QuicSendScheduler::Options flat_scheduler_options() noexcept {
    QuicSendScheduler::Options options{};
    options.pacing.enabled = false;
    options.enable_gso = false;
    return options;
}

// An unattached connection whose initial path is validated (anti-amplification
// allows sends) and remote-pointed at an unused loopback port, with Handshake
// write keys installed from fixed material — the same trick as the pacing
// tests in QuicUdpEndpointTest, so Handshake packets can be sealed without a
// TLS handshake.
QuicConnection::Options probe_connection_options(std::uint16_t remote_port) noexcept {
    QuicConnection::Options options = fiber::test::quic_options();
    options.remote_addr = loopback(remote_port);
    options.remote_connection_id = cid_from({0x51, 0x52, 0x53, 0x54});
    options.initial_path_validated = true;
    return options;
}

void install_handshake_write_keys(QuicConnection &connection) {
    constexpr fiber::quic::QuicCryptoSuite suite = fiber::quic::QuicCryptoSuite::Aes128GcmSha256;
    std::array<std::uint8_t, fiber::quic::kQuicMaxSecretLength> secret{};
    for (std::size_t i = 0; i < 32; ++i) {
        secret[i] = static_cast<std::uint8_t>(0x70U + i);
    }
    auto installed = fiber::quic::quic_set_encryption_secret(
            connection.crypto(), fiber::quic::QuicEncryptionLevel::Handshake, true, suite, secret.data(), 32);
    ASSERT_TRUE(installed.has_value()) << static_cast<int>(installed.error());
}

// PTO-style probe PINGs in the Handshake space: the datagram builder breaks
// the packet after each pto_probe frame, so every ping becomes its own
// datagram — deterministic per-connection send work for the scheduler.
void queue_probe_pings(QuicConnection &connection, std::size_t count) {
    auto &space = connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Handshake);
    for (std::size_t i = 0; i < count; ++i) {
        fiber::quic::QuicOutputFrame *frame = space.alloc_frame();
        ASSERT_NE(frame, nullptr);
        frame->type = fiber::quic::QuicFrameType::Ping;
        frame->pto_probe = true;
        space.pending_frames.push_back(*frame);
    }
}

std::size_t sent_handshake_frame_count(const QuicConnection &connection) {
    const auto &space = connection.packet_number_space(fiber::quic::QuicEncryptionLevel::Handshake);
    std::size_t count = 0;
    for (const fiber::quic::QuicOutputFrame *frame = space.sent_frames.front(); frame != nullptr;
         frame = space.sent_frames.next_of(*frame)) {
        ++count;
    }
    return count;
}

} // namespace

TEST(QuicSendSchedulerTest, InitRejectsInvalidOptions) {
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        fiber::net::UdpSocket socket(fiber::test::quic_loop());
        ASSERT_TRUE(socket.bind(loopback(0), {}).has_value());
        QuicSendScheduler scheduler(fiber::test::quic_endpoint());

        const auto expect_invalid = [&](const QuicSendScheduler::Options &options) {
            auto initialized = scheduler.init(socket, options);
            ASSERT_FALSE(initialized.has_value());
            EXPECT_EQ(initialized.error(), fiber::common::IoErr::Invalid);
            EXPECT_FALSE(scheduler.initialized());
        };

        // Batch and budget bounds.
        QuicSendScheduler::Options options = flat_scheduler_options();
        options.max_datagrams_per_batch = 0;
        expect_invalid(options);
        options = flat_scheduler_options();
        options.max_datagrams_per_batch = fiber::net::kUdpMaxBatchSize + 1;
        expect_invalid(options);
        options = flat_scheduler_options();
        options.max_gso_segments = 0;
        expect_invalid(options);
        options = flat_scheduler_options();
        options.max_gso_segments = fiber::net::kUdpMaxBatchSize + 1;
        expect_invalid(options);
        options = flat_scheduler_options();
        options.max_packets_per_wakeup = 0;
        expect_invalid(options);
        options = flat_scheduler_options();
        options.max_packets_per_connection = 0;
        expect_invalid(options);

        // Pacing shape: every field of an enabled pacer must be usable.
        options = flat_scheduler_options();
        options.pacing.enabled = true;
        options.pacing.rate_numerator = 0;
        expect_invalid(options);
        options = flat_scheduler_options();
        options.pacing.enabled = true;
        options.pacing.rate_denominator = 0;
        expect_invalid(options);
        options = flat_scheduler_options();
        options.pacing.enabled = true;
        options.pacing.max_burst_packets = 0;
        expect_invalid(options);
        options = flat_scheduler_options();
        options.pacing.enabled = true;
        options.pacing.max_burst_packets = 65;
        expect_invalid(options);
        options = flat_scheduler_options();
        options.pacing.enabled = true;
        options.pacing.timer_granularity = std::chrono::microseconds{-1};
        expect_invalid(options);

        // The boundary values themselves are all accepted, and a scheduler
        // that was never initialized accepts a later valid init.
        QuicSendScheduler::Options boundary = flat_scheduler_options();
        boundary.max_datagrams_per_batch = 1;
        boundary.max_gso_segments = 1;
        boundary.max_packets_per_wakeup = 1;
        boundary.max_packets_per_connection = 1;
        boundary.pacing.enabled = true;
        boundary.pacing.max_burst_packets = 64;
        ASSERT_TRUE(scheduler.init(socket, boundary).has_value());
        EXPECT_TRUE(scheduler.initialized());
        EXPECT_FALSE(scheduler.has_work());
        EXPECT_EQ(scheduler.stop_reason(), fiber::common::IoErr::None);

        // A second init while initialized is rejected.
        auto again = scheduler.init(socket, flat_scheduler_options());
        EXPECT_FALSE(again.has_value());
        EXPECT_EQ(again.error(), fiber::common::IoErr::Invalid);

        scheduler.close();
        socket.close();
    });
}

TEST(QuicSendSchedulerTest, SubmitAndPumpBeforeInitAreInert) {
    // submit() on an uninitialized scheduler must not queue the connection,
    // and pump() must return an empty result instead of asserting.
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        QuicConnection connection(fiber::test::quic_endpoint(), probe_connection_options(5590));
        install_handshake_write_keys(connection);
        queue_probe_pings(connection, 2);
        QuicSendScheduler scheduler(fiber::test::quic_endpoint());

        scheduler.submit(connection);
        EXPECT_FALSE(scheduler.has_work());
        EXPECT_FALSE(connection.send_queue_hook_.linked());

        const QuicSendScheduler::PumpResult result = scheduler.pump();
        EXPECT_EQ(result.packets_sent, 0U);
        EXPECT_FALSE(result.write_blocked);
        EXPECT_FALSE(result.needs_reschedule);
        EXPECT_EQ(sent_handshake_frame_count(connection), 0U);

        // remove() of a never-submitted connection is a no-op.
        scheduler.remove(connection);
        EXPECT_FALSE(scheduler.has_work());
    });
}

TEST(QuicSendSchedulerTest, PumpSendsProbeDatagramsAndDrainsConnection) {
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        fiber::net::UdpSocket socket(fiber::test::quic_loop());
        ASSERT_TRUE(socket.bind(loopback(0), {}).has_value());
        QuicSendScheduler scheduler(fiber::test::quic_endpoint());
        ASSERT_TRUE(scheduler.init(socket, flat_scheduler_options()).has_value());

        QuicConnection connection(fiber::test::quic_endpoint(), probe_connection_options(5591));
        install_handshake_write_keys(connection);
        queue_probe_pings(connection, 2);

        scheduler.submit(connection);
        EXPECT_TRUE(scheduler.has_work());
        EXPECT_TRUE(connection.send_queue_hook_.linked());

        const QuicSendScheduler::PumpResult result = scheduler.pump();

        // Both probe pings went out as their own datagrams, in flight at the
        // congestion controller, and the drained connection left the queue.
        EXPECT_EQ(result.packets_sent, 2U);
        EXPECT_FALSE(result.write_blocked);
        EXPECT_FALSE(result.needs_reschedule);
        EXPECT_FALSE(scheduler.has_work());
        EXPECT_FALSE(connection.send_queue_hook_.linked());
        EXPECT_EQ(sent_handshake_frame_count(connection), 2U);
        EXPECT_GT(connection.congestion().in_flight, 0U);

        scheduler.close();
        socket.close();
    });
}

TEST(QuicSendSchedulerTest, PumpStopsAtPerWakeupBudget) {
    // max_packets_per_wakeup bounds the work per pump round: with a budget of
    // one connection the first pump fully drains connection A and stops,
    // reporting that B still needs a reschedule.
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        fiber::net::UdpSocket socket(fiber::test::quic_loop());
        ASSERT_TRUE(socket.bind(loopback(0), {}).has_value());
        QuicSendScheduler scheduler(fiber::test::quic_endpoint());
        QuicSendScheduler::Options options = flat_scheduler_options();
        options.max_packets_per_wakeup = 1;
        ASSERT_TRUE(scheduler.init(socket, options).has_value());

        QuicConnection first(fiber::test::quic_endpoint(), probe_connection_options(5592));
        QuicConnection second(fiber::test::quic_endpoint(), probe_connection_options(5593));
        install_handshake_write_keys(first);
        install_handshake_write_keys(second);
        queue_probe_pings(first, 2);
        queue_probe_pings(second, 2);
        scheduler.submit(first);
        scheduler.submit(second);

        const auto round1 = scheduler.pump();
        EXPECT_EQ(round1.packets_sent, 2U); // one connection's full flush
        EXPECT_TRUE(round1.needs_reschedule);
        EXPECT_TRUE(scheduler.has_work());
        EXPECT_EQ(sent_handshake_frame_count(first), 2U);
        EXPECT_EQ(sent_handshake_frame_count(second), 0U); // untouched this round

        const auto round2 = scheduler.pump();
        EXPECT_EQ(round2.packets_sent, 2U);
        EXPECT_FALSE(round2.needs_reschedule);
        EXPECT_FALSE(scheduler.has_work());
        EXPECT_EQ(sent_handshake_frame_count(second), 2U);

        scheduler.close();
        socket.close();
    });
}

TEST(QuicSendSchedulerTest, PumpRotatesConnectionAtPerConnectionBudget) {
    // max_packets_per_connection splits a busy connection's work across
    // flushes and rotates it behind its peers: with a budget of one packet,
    // two connections each holding two probe pings are serviced alternately
    // (A, B in the first round), never A drained before B starts.
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        fiber::net::UdpSocket socket(fiber::test::quic_loop());
        ASSERT_TRUE(socket.bind(loopback(0), {}).has_value());
        QuicSendScheduler scheduler(fiber::test::quic_endpoint());
        QuicSendScheduler::Options options = flat_scheduler_options();
        options.max_packets_per_wakeup = 2;
        options.max_packets_per_connection = 1;
        ASSERT_TRUE(scheduler.init(socket, options).has_value());

        QuicConnection first(fiber::test::quic_endpoint(), probe_connection_options(5594));
        QuicConnection second(fiber::test::quic_endpoint(), probe_connection_options(5595));
        install_handshake_write_keys(first);
        install_handshake_write_keys(second);
        queue_probe_pings(first, 2);
        queue_probe_pings(second, 2);
        scheduler.submit(first);
        scheduler.submit(second);

        // Round 1: one packet from each connection — first rotated behind
        // second after its single-packet budget, then the wakeup budget
        // stopped the round with both still queued.
        const auto round1 = scheduler.pump();
        EXPECT_EQ(round1.packets_sent, 2U);
        EXPECT_TRUE(round1.needs_reschedule);
        EXPECT_TRUE(scheduler.has_work());
        EXPECT_EQ(sent_handshake_frame_count(first), 1U);
        EXPECT_EQ(sent_handshake_frame_count(second), 1U);
        EXPECT_TRUE(first.send_queue_hook_.linked());
        EXPECT_TRUE(second.send_queue_hook_.linked());

        // Round 2: the wakeup budget now covers both remaining packets; each
        // connection is drained (and removed) at its own budget boundary.
        const auto round2 = scheduler.pump();
        EXPECT_EQ(round2.packets_sent, 2U);
        EXPECT_FALSE(round2.needs_reschedule);
        EXPECT_FALSE(scheduler.has_work());
        EXPECT_EQ(sent_handshake_frame_count(first), 2U);
        EXPECT_EQ(sent_handshake_frame_count(second), 2U);
        EXPECT_FALSE(first.send_queue_hook_.linked());
        EXPECT_FALSE(second.send_queue_hook_.linked());

        scheduler.close();
        socket.close();
    });
}

TEST(QuicSendSchedulerTest, CloseStopsPumpingAndKeepsFirstStopReason) {
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        fiber::net::UdpSocket socket(fiber::test::quic_loop());
        ASSERT_TRUE(socket.bind(loopback(0), {}).has_value());
        QuicSendScheduler scheduler(fiber::test::quic_endpoint());
        ASSERT_TRUE(scheduler.init(socket, flat_scheduler_options()).has_value());

        QuicConnection connection(fiber::test::quic_endpoint(), probe_connection_options(5596));
        install_handshake_write_keys(connection);
        queue_probe_pings(connection, 2);
        scheduler.submit(connection);
        ASSERT_TRUE(scheduler.has_work());

        scheduler.close(fiber::common::IoErr::Invalid);
        EXPECT_FALSE(scheduler.has_work());
        EXPECT_FALSE(connection.send_queue_hook_.linked());
        EXPECT_EQ(scheduler.stop_reason(), fiber::common::IoErr::Invalid);

        // A second close cannot overwrite the first reason, and a closed
        // scheduler neither pumps nor accepts new work.
        scheduler.close(fiber::common::IoErr::Canceled);
        EXPECT_EQ(scheduler.stop_reason(), fiber::common::IoErr::Invalid);

        const auto result = scheduler.pump();
        EXPECT_EQ(result.packets_sent, 0U);
        EXPECT_FALSE(result.needs_reschedule);
        scheduler.submit(connection);
        EXPECT_FALSE(scheduler.has_work());
        EXPECT_EQ(sent_handshake_frame_count(connection), 0U);

        // close() dropped initialized_, so the scheduler can be re-armed.
        ASSERT_TRUE(scheduler.init(socket, flat_scheduler_options()).has_value());
        EXPECT_TRUE(scheduler.initialized());
        EXPECT_EQ(scheduler.stop_reason(), fiber::common::IoErr::None);

        scheduler.close();
        socket.close();
    });
}
