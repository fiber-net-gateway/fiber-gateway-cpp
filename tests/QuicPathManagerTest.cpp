#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include <fiber/quic/QuicConnection.h>
#include <fiber/quic/QuicFrame.h>
#include <fiber/quic/QuicPath.h>

#include "QuicTestLoop.h"

namespace {

using fiber::quic::QuicConnection;
using fiber::quic::QuicPath;
using fiber::quic::QuicPathState;
using fiber::quic::QuicPathTag;

fiber::net::SocketAddress loopback(std::uint16_t port) { return {fiber::net::IpAddress::loopback_v4(), port}; }

fiber::quic::QuicConnectionId cid_from(std::initializer_list<std::uint8_t> bytes) {
    auto cid = fiber::quic::QuicConnectionId::from_bytes(bytes.begin(), bytes.size());
    return cid.value_or(fiber::quic::QuicConnectionId{});
}

bool cid_equals(const fiber::quic::QuicConnectionId &lhs, const fiber::quic::QuicConnectionId &rhs) noexcept {
    return lhs.size() == rhs.size() && (lhs.empty() || std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0);
}

// A connection whose initial path is already validated (no anti-amplification
// accounting concerns) and whose peer is a stable remote endpoint.
QuicConnection::Options path_options(std::uint16_t remote_port) noexcept {
    QuicConnection::Options options = fiber::test::quic_options();
    options.remote_addr = loopback(remote_port);
    options.remote_connection_id = cid_from({0x10, 0x20, 0x30, 0x40});
    options.initial_path_validated = true;
    return options;
}

std::size_t count_path_frames(const QuicPath &path, fiber::quic::QuicFrameType type) {
    std::size_t count = 0;
    for (const fiber::quic::QuicOutputFrame *frame = path.pending_frames.front(); frame != nullptr;
         frame = path.pending_frames.next_of(*frame)) {
        if (frame->type == type) {
            ++count;
        }
    }
    return count;
}

fiber::quic::QuicNewConnectionIdFrame make_new_cid_frame(std::uint64_t sequence_number, std::uint64_t retire_prior_to,
                                                         std::initializer_list<std::uint8_t> cid_bytes,
                                                         std::uint8_t token_pattern) {
    fiber::quic::QuicNewConnectionIdFrame frame{};
    frame.sequence_number = sequence_number;
    frame.retire_prior_to = retire_prior_to;
    frame.cid_len = static_cast<std::uint8_t>(cid_bytes.size());
    std::size_t i = 0;
    for (std::uint8_t byte: cid_bytes) {
        frame.cid[i++] = byte;
    }
    for (std::size_t j = 0; j < fiber::quic::kStatelessResetTokenLength; ++j) {
        frame.stateless_reset_token[j] = static_cast<std::uint8_t>(token_pattern ^ static_cast<std::uint8_t>(j));
    }
    return frame;
}

} // namespace

TEST(QuicPathManagerTest, ValidationRetriesDoubleDeadlineThenFailsOverToBackup) {
    // RFC 9000 §9.3 path validation: each expiry re-queues the challenge pair
    // and doubles the deadline (validation_delay(tries)); after
    // kQuicPathRetries attempts the path is abandoned and, for the active
    // path, failover promotes the Backup path to Active.
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        QuicConnection conn(fiber::test::quic_endpoint(), path_options(5600));
        QuicPath *active = conn.active_path();
        ASSERT_NE(active, nullptr);
        ASSERT_TRUE(active->validated);

        // Default-RTT connection (not Established): PTO = 333 + 4*166 = 997 ms,
        // so the validation delay floors at 1000 ms and doubles per try.
        EXPECT_EQ(conn.paths().validation_delay(), fiber::quic::QuicTime{1000});
        EXPECT_EQ(conn.paths().validation_delay(1), fiber::quic::QuicTime{2000});
        EXPECT_EQ(conn.paths().validation_delay(2), fiber::quic::QuicTime{4000});

        QuicPath *backup = conn.create_path(loopback(5601), conn.local_addr(), cid_from({0x10, 0x20, 0x30, 0x40}),
                                            QuicPathTag::Backup);
        ASSERT_NE(backup, nullptr);
        EXPECT_EQ(conn.path_count(), 2U);

        auto started = conn.paths().start_validation(*active, fiber::quic::QuicTime{100});
        ASSERT_TRUE(started.has_value()) << static_cast<int>(started.error());
        EXPECT_EQ(active->state, QuicPathState::Validating);
        EXPECT_EQ(active->tries, 0U);
        EXPECT_EQ(active->expires, fiber::quic::QuicTime{1100});
        EXPECT_EQ(count_path_frames(*active, fiber::quic::QuicFrameType::PathChallenge), 2U);

        // Retry 1: challenges re-queued, deadline doubled from the expiry time.
        auto retry1 = conn.paths().expire_validation(*active, fiber::quic::QuicTime{1100});
        ASSERT_TRUE(retry1.has_value());
        EXPECT_TRUE(*retry1);
        EXPECT_EQ(active->tries, 1U);
        EXPECT_EQ(active->expires, fiber::quic::QuicTime{3100});
        EXPECT_EQ(count_path_frames(*active, fiber::quic::QuicFrameType::PathChallenge), 2U);

        // Retry 2: doubled again.
        auto retry2 = conn.paths().expire_validation(*active, fiber::quic::QuicTime{3100});
        ASSERT_TRUE(retry2.has_value());
        EXPECT_TRUE(*retry2);
        EXPECT_EQ(active->tries, 2U);
        EXPECT_EQ(active->expires, fiber::quic::QuicTime{7100});

        // Exhaustion: no retry, the failed path is freed and the Backup takes
        // over as the active path (options republished by set_active).
        auto exhausted = conn.paths().expire_validation(*active, fiber::quic::QuicTime{7100});
        ASSERT_TRUE(exhausted.has_value());
        EXPECT_FALSE(*exhausted);
        EXPECT_FALSE(active->allocated);
        EXPECT_FALSE(conn.closing());
        EXPECT_EQ(conn.path_count(), 1U);
        EXPECT_EQ(conn.active_path(), backup);
        EXPECT_EQ(backup->tag, QuicPathTag::Active);
        EXPECT_EQ(conn.remote_addr().port(), 5601U);
    });
}

TEST(QuicPathManagerTest, ValidationExhaustionWithoutBackupClosesConnection) {
    // Losing the only path with no Backup to promote is fatal: the connection
    // is closed with NO_VIABLE_PATH.
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        QuicConnection conn(fiber::test::quic_endpoint(), path_options(5602));
        QuicPath *active = conn.active_path();
        ASSERT_NE(active, nullptr);
        ASSERT_EQ(conn.path_count(), 1U);

        ASSERT_TRUE(conn.paths().start_validation(*active, fiber::quic::QuicTime{100}).has_value());
        ASSERT_TRUE(conn.paths().expire_validation(*active, fiber::quic::QuicTime{1100}).has_value());
        ASSERT_TRUE(conn.paths().expire_validation(*active, fiber::quic::QuicTime{3100}).has_value());
        EXPECT_FALSE(conn.closing());

        auto exhausted = conn.paths().expire_validation(*active, fiber::quic::QuicTime{7100});
        ASSERT_TRUE(exhausted.has_value());
        EXPECT_FALSE(*exhausted);
        EXPECT_TRUE(conn.closing());
        EXPECT_EQ(conn.close_error(), fiber::quic::QuicErrorCode::NoViablePath);
    });
}

TEST(QuicPathManagerTest, ProbePathValidationCompletesWithoutTouchingActiveState) {
    // A non-active (probe) path validating successfully must only clear its
    // own challenges and flip its own flags: no congestion reset, no options
    // republish, no active-path change (those are active-path-only effects).
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        QuicConnection conn(fiber::test::quic_endpoint(), path_options(5603));
        QuicPath *active = conn.active_path();
        ASSERT_NE(active, nullptr);
        conn.congestion().window = 77777;

        QuicPath *probe = conn.create_path(loopback(5604), conn.local_addr(), cid_from({0x10, 0x20, 0x30, 0x40}),
                                           QuicPathTag::Probe);
        ASSERT_NE(probe, nullptr);
        ASSERT_TRUE(conn.paths().start_validation(*probe, fiber::quic::QuicTime{100}).has_value());
        EXPECT_EQ(count_path_frames(*probe, fiber::quic::QuicFrameType::PathChallenge), 2U);

        // Respond with the second challenge of the pair.
        fiber::quic::QuicPathChallengeFrame response{};
        std::memcpy(response.data, probe->challenge[1], sizeof(response.data));
        auto matched = conn.recv_path_response_frame_with_path(response, fiber::quic::QuicTime{200});
        ASSERT_TRUE(matched.has_value()) << static_cast<int>(matched.error());
        EXPECT_EQ(*matched, probe);

        EXPECT_TRUE(probe->validated);
        EXPECT_EQ(probe->state, QuicPathState::Idle);
        EXPECT_EQ(probe->tries, 0U);
        EXPECT_EQ(probe->expires, fiber::quic::QuicTime{0});
        EXPECT_EQ(count_path_frames(*probe, fiber::quic::QuicFrameType::PathChallenge), 0U);

        // Active path and its congestion state are untouched.
        EXPECT_EQ(conn.active_path(), active);
        EXPECT_EQ(conn.congestion().window, 77777U);
        EXPECT_EQ(conn.remote_addr().port(), 5603U);
    });
}

TEST(QuicPathManagerTest, SetActiveDemotesOldActiveAndRepublishesOptions) {
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        QuicConnection conn(fiber::test::quic_endpoint(), path_options(5605));
        QuicPath *initial = conn.active_path();
        ASSERT_NE(initial, nullptr);
        EXPECT_EQ(initial->tag, QuicPathTag::Active);

        const fiber::quic::QuicConnectionId replacement_cid = cid_from({0x61, 0x62});
        QuicPath *replacement =
                conn.create_path(loopback(5606), conn.local_addr(), replacement_cid, QuicPathTag::Backup);
        ASSERT_NE(replacement, nullptr);

        EXPECT_TRUE(conn.set_active_path(*replacement));
        EXPECT_EQ(initial->tag, QuicPathTag::Backup);
        EXPECT_EQ(replacement->tag, QuicPathTag::Active);
        EXPECT_EQ(conn.active_path(), replacement);
        // The connection's published peer identity follows the active path.
        EXPECT_EQ(conn.remote_addr().port(), 5606U);
        EXPECT_TRUE(cid_equals(conn.remote_connection_id(), replacement_cid));

        // A freed path can no longer be activated.
        QuicPath *scratch = conn.create_path(loopback(5607), conn.local_addr(), replacement_cid, QuicPathTag::Probe);
        ASSERT_NE(scratch, nullptr);
        conn.free_path(*scratch);
        EXPECT_FALSE(conn.set_active_path(*scratch));
        EXPECT_EQ(conn.active_path(), replacement);
    });
}

TEST(QuicPathManagerTest, PathSlotsAreLimitedToThreeEntries) {
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        QuicConnection conn(fiber::test::quic_endpoint(), path_options(5608));
        ASSERT_EQ(conn.path_count(), 1U); // the initial active path

        QuicPath *second = conn.create_path(loopback(5609), conn.local_addr(), cid_from({0x10, 0x20, 0x30, 0x40}),
                                            QuicPathTag::Probe);
        QuicPath *third = conn.create_path(loopback(5610), conn.local_addr(), cid_from({0x10, 0x20, 0x30, 0x40}),
                                           QuicPathTag::Probe);
        ASSERT_NE(second, nullptr);
        ASSERT_NE(third, nullptr);
        EXPECT_EQ(conn.path_count(), 3U);

        // kQuicMaxPaths slots are exhausted.
        EXPECT_EQ(conn.create_path(loopback(5611), conn.local_addr(), cid_from({0x10, 0x20, 0x30, 0x40}),
                                   QuicPathTag::Probe),
                  nullptr);

        // Freeing one slot makes room again.
        conn.free_path(*second);
        EXPECT_EQ(conn.path_count(), 2U);
        EXPECT_NE(conn.create_path(loopback(5611), conn.local_addr(), cid_from({0x10, 0x20, 0x30, 0x40}),
                                   QuicPathTag::Probe),
                  nullptr);
        EXPECT_EQ(conn.path_count(), 3U);
    });
}

TEST(QuicPathManagerTest, CreateBindsPathToMatchingPeerCidSlot) {
    // A new path binds to the peer-CID pool slot whose cid bytes match its
    // remote_connection_id (sequence 0 for the handshake SCID, later sequences
    // for NEW_CONNECTION_ID-issued cids), and freeing the last path bound to a
    // slot releases it for retirement.
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        QuicConnection::Options options = path_options(5612);
        options.role = fiber::quic::QuicConnectionRole::Client;
        QuicConnection conn(fiber::test::quic_endpoint(), options);

        // The initial path is bound to the handshake SCID, sequence 0.
        EXPECT_EQ(conn.active_path()->remote_connection_id_sequence, 0U);

        // A path created with the handshake cid also binds sequence 0.
        QuicPath *initial_cid_path = conn.create_path(loopback(5613), conn.local_addr(),
                                                      cid_from({0x10, 0x20, 0x30, 0x40}), QuicPathTag::Probe);
        ASSERT_NE(initial_cid_path, nullptr);
        EXPECT_EQ(initial_cid_path->remote_connection_id_sequence, 0U);

        // Peer issues a new cid at sequence 1.
        auto installed = conn.recv_new_connection_id_frame(make_new_cid_frame(1, 0, {0xaa, 0xbb}, 0x42));
        ASSERT_TRUE(installed.has_value()) << static_cast<int>(installed.error());

        QuicPath *replacement_cid_path =
                conn.create_path(loopback(5614), conn.local_addr(), cid_from({0xaa, 0xbb}), QuicPathTag::Probe);
        ASSERT_NE(replacement_cid_path, nullptr);
        EXPECT_EQ(replacement_cid_path->remote_connection_id_sequence, 1U);
        EXPECT_NE(conn.paths().find_path_by_remote_cid_sequence(1), nullptr);

        // Freeing the only path bound to sequence 1 releases the slot: when the
        // peer then retires the active handshake cid (sequence 0), the freed
        // cid is available as the replacement and the active path rebinds to
        // it instead of the connection dying on CID exhaustion.
        conn.free_path(*replacement_cid_path);
        EXPECT_EQ(conn.paths().find_path_by_remote_cid_sequence(1), nullptr);

        auto retired = conn.recv_new_connection_id_frame(make_new_cid_frame(2, 1, {0x77, 0x88}, 0x99));
        ASSERT_TRUE(retired.has_value()) << static_cast<int>(retired.error());
        EXPECT_FALSE(conn.closing());
        EXPECT_EQ(conn.active_path()->remote_connection_id_sequence, 1U);
        ASSERT_EQ(conn.active_path()->remote_connection_id.size(), 2U);
        EXPECT_EQ(conn.active_path()->remote_connection_id.data()[0], 0xaa);
        EXPECT_EQ(conn.remote_connection_id().data()[0], 0xaa);
    });
}

TEST(QuicPathManagerTest, MigrationWithRebindRevalidatesOldActivePath) {
    // RFC 9000 §9.4: migration to an unvalidated path starts validation on the
    // new path AND puts the old active path back into validation (kept as
    // Backup while the new one proves itself). The rebound=false port-only
    // shape is covered by QuicConnectionTest.MigrationQueuesPathChallenges.
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        QuicConnection conn(fiber::test::quic_endpoint(), path_options(5615));
        QuicPath *initial = conn.active_path();
        ASSERT_NE(initial, nullptr);
        ASSERT_TRUE(initial->validated);

        QuicPath *migrated =
                conn.create_path(loopback(5616), conn.local_addr(), cid_from({0x71, 0x72}), QuicPathTag::Probe);
        ASSERT_NE(migrated, nullptr);

        auto migrated_result = conn.handle_migration(*migrated, true, fiber::quic::QuicTime{500});
        ASSERT_TRUE(migrated_result.has_value()) << static_cast<int>(migrated_result.error());

        EXPECT_EQ(initial->tag, QuicPathTag::Backup);
        EXPECT_EQ(initial->state, QuicPathState::Validating);
        EXPECT_EQ(initial->expires, fiber::quic::QuicTime{1500});
        EXPECT_EQ(count_path_frames(*initial, fiber::quic::QuicFrameType::PathChallenge), 2U);

        EXPECT_EQ(conn.active_path(), migrated);
        EXPECT_EQ(migrated->tag, QuicPathTag::Active);
        EXPECT_EQ(migrated->state, QuicPathState::Validating);
        EXPECT_EQ(count_path_frames(*migrated, fiber::quic::QuicFrameType::PathChallenge), 2U);
        EXPECT_EQ(conn.remote_addr().port(), 5616U);

        // Re-migration onto the path that is already active is a no-op: the
        // old path's deadline is not re-armed.
        auto again = conn.handle_migration(*migrated, true, fiber::quic::QuicTime{600});
        ASSERT_TRUE(again.has_value());
        EXPECT_EQ(initial->expires, fiber::quic::QuicTime{1500});
    });
}
