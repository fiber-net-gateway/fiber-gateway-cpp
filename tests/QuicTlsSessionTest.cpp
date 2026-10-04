#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include <fiber/net/TlsCredential.h>
#include <fiber/net/TlsParams.h>
#include <fiber/net/TlsServerHandshakeConfig.h>
#include <fiber/quic/QuicConnection.h>
#include <fiber/quic/QuicTlsSession.h>
#include <fiber/tls/TlsTypes.h>
#include "quic/QuicCrypto.h"

#include "QuicTestLoop.h"
#include "QuicTestTlsCertificate.h"

namespace {

constexpr std::string_view kAlpn[] = {"h3"};

fiber::quic::QuicConnectionId cid(std::uint8_t seed) {
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(seed + i);
    }
    return fiber::quic::QuicConnectionId::from_bytes(bytes.data(), bytes.size())
            .value_or(fiber::quic::QuicConnectionId{});
}

fiber::mem::IoBufChain crypto_chain(std::span<const std::uint8_t> bytes) {
    fiber::mem::IoBufChain chain;
    if (!bytes.empty()) {
        auto buf = fiber::mem::IoBuf::allocate(bytes.size());
        FIBER_ASSERT(buf);
        std::memcpy(buf.writable_data(), bytes.data(), bytes.size());
        buf.commit(bytes.size());
        FIBER_ASSERT(chain.append(std::move(buf)));
    }
    return chain;
}

// The CRYPTO bytes a connection queued at `level`, in offset order.
std::vector<std::uint8_t> queued_crypto(fiber::quic::QuicConnection &connection,
                                        fiber::quic::QuicEncryptionLevel level) {
    std::vector<std::uint8_t> out;
    auto &space = connection.packet_number_space(level);
    for (const fiber::quic::QuicOutputFrame *frame = space.pending_frames.front(); frame != nullptr;
         frame = space.pending_frames.next_of(*frame)) {
        if (frame->type == fiber::quic::QuicFrameType::Crypto && frame->data && frame->u.crypto.offset == out.size()) {
            const fiber::mem::IoBuf &data = frame->data;
            out.insert(out.end(), data.readable_data(), data.readable_data() + data.readable());
        }
    }
    return out;
}

// Supplies a real server flight; the test controls the client's done-transition
// and any bytes piggybacked after Finished.
template<typename F>
void with_server_flight(F &&test) {
    ::fiber::test::run_in_quic_loop([&](::fiber::mem::IoBufNodePool &) {
        fiber::net::TlsCredentialOptions credential_options{};
        credential_options.certificate_chain =
                fiber::net::TlsPemSource::from_content(fiber::test::kQuicTestCertificatePem);
        credential_options.private_key = fiber::net::TlsPemSource::from_content(fiber::test::kQuicTestPrivateKeyPem);
        auto credential = fiber::net::TlsCredential::create(credential_options);
        ASSERT_TRUE(credential.has_value());
        fiber::net::TlsServerParam server_tls{};
        server_tls.configure_callback = &fiber::net::configure_tls_with_credential;
        server_tls.configure_ctx = &*credential;
        server_tls.min_version = 0x0304;
        server_tls.max_version = 0x0304;
        server_tls.alpn = kAlpn;

        const auto client_cid = cid(0x10);
        const auto server_cid = cid(0x40);

        fiber::quic::QuicConnection::Options client_options = fiber::test::quic_options();
        client_options.role = fiber::quic::QuicConnectionRole::Client;
        client_options.local_connection_id = client_cid;
        client_options.remote_connection_id = server_cid;
        client_options.original_destination_connection_id = server_cid;
        std::vector<std::uint8_t> tickets;
        client_options.owner = &tickets;
        client_options.ops.on_new_tls_session = [](void *owner, fiber::quic::QuicConnection &,
                                                   fiber::tls::TlsSessionState &&session) noexcept {
            auto &received = *static_cast<std::vector<std::uint8_t> *>(owner);
            if (session.identity.size() == 1) {
                received.push_back(session.identity[0]);
            }
            return true;
        };
        fiber::quic::QuicConnection client(fiber::test::quic_endpoint(), client_options);
        ASSERT_TRUE(fiber::quic::quic_init_initial_crypto(client.crypto(), fiber::quic::QuicConnectionRole::Client,
                                                          server_cid));
        fiber::net::TlsClientParam client_tls{};
        client_tls.min_version = 0x0304;
        client_tls.max_version = 0x0304;
        client_tls.alpn = kAlpn;
        client_tls.server_name = "localhost";
        ASSERT_TRUE(client.tls().init_client(client_tls, /*allow_insecure=*/true));
        auto client_driven = client.tls().drive_handshake();
        ASSERT_TRUE(client_driven || client_driven.error() == fiber::common::IoErr::WouldBlock);
        const auto hello = queued_crypto(client, fiber::quic::QuicEncryptionLevel::Initial);
        ASSERT_FALSE(hello.empty());

        fiber::quic::QuicConnection::Options server_options = fiber::test::quic_options();
        server_options.role = fiber::quic::QuicConnectionRole::Server;
        server_options.local_connection_id = server_cid;
        server_options.remote_connection_id = client_cid;
        server_options.original_destination_connection_id = server_cid;
        fiber::quic::QuicConnection server(fiber::test::quic_endpoint(), server_options);
        ASSERT_TRUE(fiber::quic::quic_init_initial_crypto(server.crypto(), fiber::quic::QuicConnectionRole::Server,
                                                          server_cid));
        ASSERT_TRUE(server.tls().init_server(server_tls));
        auto hello_chain = crypto_chain(hello);
        ASSERT_TRUE(server.tls().provide_crypto_data(fiber::quic::QuicEncryptionLevel::Initial, hello_chain));
        EXPECT_TRUE(hello_chain.empty());
        auto server_driven = server.tls().drive_handshake();
        ASSERT_TRUE(server_driven || server_driven.error() == fiber::common::IoErr::WouldBlock);
        ASSERT_FALSE(server.tls().handshake_done());
        ASSERT_FALSE(server.terminal_closing());

        // Direct TLS feeding bypasses the packet layer that normally records the SCID.
        ASSERT_TRUE(client.adopt_server_initial_source_connection_id(server_cid));
        auto initial = crypto_chain(queued_crypto(server, fiber::quic::QuicEncryptionLevel::Initial));
        ASSERT_TRUE(client.tls().provide_crypto_data(fiber::quic::QuicEncryptionLevel::Initial, initial));
        auto flight = crypto_chain(queued_crypto(server, fiber::quic::QuicEncryptionLevel::Handshake));
        ASSERT_NE(flight.readable_bytes(), 0u);
        test(client, flight, tickets);
    });
}

// A valid NST with an empty nonce and extension block, and a one-byte identity.
constexpr std::array<std::uint8_t, 18> kTicket{4, 0, 0, 14, 0, 0, 0, 60, 0, 0, 0, 0, 0, 0, 1, 0x42, 0, 0};

} // namespace

TEST(QuicTlsSessionTest, RoleMismatchLeavesSessionAvailableForCorrectRole) {
    ::fiber::test::run_in_quic_loop([](::fiber::mem::IoBufNodePool &) {
        fiber::net::TlsCredentialOptions credential_options{};
        credential_options.certificate_chain =
                fiber::net::TlsPemSource::from_content(fiber::test::kQuicTestCertificatePem);
        credential_options.private_key = fiber::net::TlsPemSource::from_content(fiber::test::kQuicTestPrivateKeyPem);
        auto credential = fiber::net::TlsCredential::create(credential_options);
        ASSERT_TRUE(credential.has_value());
        fiber::net::TlsServerParam server_tls{};
        server_tls.configure_callback = &fiber::net::configure_tls_with_credential;
        server_tls.configure_ctx = &*credential;
        server_tls.alpn = kAlpn;
        fiber::net::TlsClientParam client_tls{};
        client_tls.alpn = kAlpn;
        client_tls.server_name = "localhost";

        using fiber::quic::QuicConnectionRole;
        for (auto role: {QuicConnectionRole::Client, QuicConnectionRole::Server}) {
            auto options = fiber::test::quic_options();
            options.role = role;
            options.local_connection_id = cid(0x10);
            options.remote_connection_id = cid(0x40);
            options.original_destination_connection_id = options.remote_connection_id;
            fiber::quic::QuicConnection connection(fiber::test::quic_endpoint(), options);
            ASSERT_TRUE(fiber::quic::quic_init_initial_crypto(connection.crypto(), role,
                                                              options.original_destination_connection_id));
            auto &session = connection.tls();
            const bool is_client = role == QuicConnectionRole::Client;
            auto rejected = is_client ? session.init_server(server_tls) : session.init_client(client_tls, true);
            ASSERT_FALSE(rejected);
            EXPECT_EQ(rejected.error(), fiber::common::IoErr::Invalid);
            EXPECT_FALSE(session.initialized());
            EXPECT_FALSE(session.handshake_done());
            EXPECT_TRUE(queued_crypto(connection, fiber::quic::QuicEncryptionLevel::Initial).empty());

            auto initialized = is_client ? session.init_client(client_tls, true) : session.init_server(server_tls);
            ASSERT_TRUE(initialized);
            EXPECT_TRUE(session.initialized());
            EXPECT_FALSE(session.handshake_done());
            EXPECT_EQ(queued_crypto(connection, fiber::quic::QuicEncryptionLevel::Initial).empty(), !is_client);
        }
    });
}

// RFC 9001 §4.1.3: TLS consumes CRYPTO data level by level. A client that
// sends a 1-RTT CRYPTO frame (the server already holds 1-RTT read keys after
// its first flight) and then Handshake CRYPTO must get a CRYPTO_ERROR close,
// not take the server down on the engine's level-order assert. Found by
// fuzz/quic_endpoint_fuzzer.
TEST(QuicTlsSessionTest, CryptoBelowProvidedLevelClosesWithUnexpectedMessage) {
    ::fiber::test::run_in_quic_loop([&](::fiber::mem::IoBufNodePool &) {
        fiber::net::TlsCredentialOptions credential_options{};
        credential_options.certificate_chain =
                fiber::net::TlsPemSource::from_content(fiber::test::kQuicTestCertificatePem);
        credential_options.private_key = fiber::net::TlsPemSource::from_content(fiber::test::kQuicTestPrivateKeyPem);
        auto credential = fiber::net::TlsCredential::create(credential_options);
        ASSERT_TRUE(credential.has_value());
        fiber::net::TlsServerParam server_tls{};
        server_tls.configure_callback = &fiber::net::configure_tls_with_credential;
        server_tls.configure_ctx = &*credential;
        server_tls.min_version = 0x0304;
        server_tls.max_version = 0x0304;
        server_tls.alpn = kAlpn;

        const auto client_cid = cid(0x10);
        const auto server_cid = cid(0x40);

        fiber::quic::QuicConnection::Options client_options = fiber::test::quic_options();
        client_options.role = fiber::quic::QuicConnectionRole::Client;
        client_options.local_connection_id = client_cid;
        client_options.remote_connection_id = server_cid;
        client_options.original_destination_connection_id = server_cid;
        fiber::quic::QuicConnection client(fiber::test::quic_endpoint(), client_options);
        ASSERT_TRUE(fiber::quic::quic_init_initial_crypto(client.crypto(), fiber::quic::QuicConnectionRole::Client,
                                                          server_cid));
        fiber::net::TlsClientParam client_tls{};
        client_tls.min_version = 0x0304;
        client_tls.max_version = 0x0304;
        client_tls.alpn = kAlpn;
        client_tls.server_name = "localhost";
        ASSERT_TRUE(client.tls().init_client(client_tls, /*allow_insecure=*/true));
        auto client_driven = client.tls().drive_handshake();
        ASSERT_TRUE(client_driven || client_driven.error() == fiber::common::IoErr::WouldBlock);
        const auto hello = queued_crypto(client, fiber::quic::QuicEncryptionLevel::Initial);
        ASSERT_FALSE(hello.empty());

        fiber::quic::QuicConnection::Options server_options = fiber::test::quic_options();
        server_options.role = fiber::quic::QuicConnectionRole::Server;
        server_options.local_connection_id = server_cid;
        server_options.remote_connection_id = client_cid;
        server_options.original_destination_connection_id = server_cid;
        fiber::quic::QuicConnection server(fiber::test::quic_endpoint(), server_options);
        ASSERT_TRUE(fiber::quic::quic_init_initial_crypto(server.crypto(), fiber::quic::QuicConnectionRole::Server,
                                                          server_cid));
        ASSERT_TRUE(server.tls().init_server(server_tls));
        auto hello_chain = crypto_chain(hello);
        ASSERT_TRUE(server.tls().provide_crypto_data(fiber::quic::QuicEncryptionLevel::Initial, hello_chain));
        EXPECT_TRUE(hello_chain.empty());
        auto server_driven = server.tls().drive_handshake();
        ASSERT_TRUE(server_driven || server_driven.error() == fiber::common::IoErr::WouldBlock);
        ASSERT_FALSE(server.tls().handshake_done());
        ASSERT_FALSE(server.terminal_closing());

        // An incomplete message at 1-RTT (Finished header announcing 96 bytes,
        // 32 present), then bytes at the Handshake level below it.
        std::vector<std::uint8_t> partial{0x14, 0x00, 0x00, 0x60};
        partial.resize(partial.size() + 32, 0x42);
        auto partial_chain = crypto_chain(partial);
        ASSERT_TRUE(server.tls().provide_crypto_data(fiber::quic::QuicEncryptionLevel::Application, partial_chain));
        EXPECT_TRUE(partial_chain.empty());
        partial_chain = crypto_chain(partial);
        auto lower = server.tls().provide_crypto_data(fiber::quic::QuicEncryptionLevel::Handshake, partial_chain);
        EXPECT_EQ(partial_chain.readable_bytes(), partial.size());
        EXPECT_FALSE(lower.has_value());
        EXPECT_TRUE(server.terminal_closing());
        EXPECT_EQ(server.close_info().error_code, fiber::quic::quic_crypto_error_code(static_cast<std::uint8_t>(
                                                          fiber::tls::TlsAlertDesc::UnexpectedMessage)));
    });
}

TEST(QuicTlsSessionTest, PostHandshakeChainPreservesEngineTailBeforeLaterProvides) {
    with_server_flight([](auto &client, auto &flight, auto &tickets) {
        using fiber::quic::QuicEncryptionLevel;
        auto prefix = crypto_chain(std::span(kTicket).first(2));
        ASSERT_TRUE(flight.append_chain(std::move(prefix)));
        ASSERT_TRUE(client.tls().provide_crypto_data(QuicEncryptionLevel::Handshake, flight));
        EXPECT_TRUE(flight.empty());
        // No drive between provides: the engine's partial header must come first.
        auto rest = crypto_chain(std::span(kTicket).subspan(2));
        auto next = crypto_chain(kTicket);
        ASSERT_TRUE(rest.append_chain(std::move(next)));
        prefix = crypto_chain(std::span(kTicket).first(3));
        ASSERT_TRUE(rest.append_chain(std::move(prefix)));
        ASSERT_TRUE(client.tls().provide_crypto_data(QuicEncryptionLevel::Application, rest));
        EXPECT_TRUE(rest.empty());
        ASSERT_TRUE(client.tls().drive_handshake());
        ASSERT_EQ(tickets.size(), 2u);
        auto tail = crypto_chain(std::span(kTicket).subspan(3));
        ASSERT_TRUE(client.tls().provide_crypto_data(QuicEncryptionLevel::Application, tail));
        ASSERT_TRUE(client.tls().process_post_handshake());
        EXPECT_EQ(tickets, (std::vector<std::uint8_t>{0x42, 0x42, 0x42}));
        EXPECT_FALSE(client.terminal_closing());
    });
}

TEST(QuicTlsSessionTest, PostHandshakeChainHandlesEveryTicketSplit) {
    with_server_flight([](auto &client, auto &flight, auto &tickets) {
        using fiber::quic::QuicEncryptionLevel;
        ASSERT_TRUE(client.tls().provide_crypto_data(QuicEncryptionLevel::Handshake, flight));
        ASSERT_TRUE(client.tls().drive_handshake());
        for (std::size_t split = 1; split < kTicket.size(); ++split) {
            auto prefix = crypto_chain(std::span(kTicket).first(split));
            ASSERT_TRUE(client.tls().provide_crypto_data(QuicEncryptionLevel::Application, prefix));
            ASSERT_TRUE(client.tls().process_post_handshake());
            EXPECT_EQ(tickets.size(), split - 1);
            auto suffix = crypto_chain(std::span(kTicket).subspan(split));
            ASSERT_TRUE(client.tls().provide_crypto_data(QuicEncryptionLevel::Application, suffix));
            ASSERT_TRUE(client.tls().process_post_handshake());
            ASSERT_EQ(tickets.size(), split);
            EXPECT_EQ(tickets.back(), 0x42);
            EXPECT_FALSE(client.terminal_closing());
        }
    });
}

TEST(QuicTlsSessionTest, PostHandshakeChainRejectsOversizedInputAndEngineTail) {
    for (bool engine_tail: {false, true}) {
        with_server_flight([&](auto &client, auto &flight, auto &) {
            using fiber::quic::QuicEncryptionLevel;
            std::vector<std::uint8_t> oversized(64 * 1024 + 1, 0);
            auto input = crypto_chain(oversized);
            if (engine_tail) {
                ASSERT_TRUE(flight.append_chain(std::move(input)));
                ASSERT_TRUE(client.tls().provide_crypto_data(QuicEncryptionLevel::Handshake, flight));
                const auto result = client.tls().drive_handshake();
                ASSERT_FALSE(result);
                EXPECT_EQ(result.error(), fiber::common::IoErr::MessageTooLarge);
            } else {
                ASSERT_TRUE(client.tls().provide_crypto_data(QuicEncryptionLevel::Handshake, flight));
                ASSERT_TRUE(client.tls().drive_handshake());
                const auto result = client.tls().provide_crypto_data(QuicEncryptionLevel::Application, input);
                ASSERT_FALSE(result);
                EXPECT_EQ(result.error(), fiber::common::IoErr::MessageTooLarge);
                EXPECT_EQ(input.readable_bytes(), oversized.size());
            }
            EXPECT_TRUE(client.terminal_closing());
            EXPECT_EQ(client.tls().last_alert(), static_cast<std::uint8_t>(fiber::tls::TlsAlertDesc::DecodeError));
        });
    }
}

TEST(QuicTlsSessionTest, PostHandshakeChainRejectsLengthIncludingHeaderAndKeyUpdate) {
    for (const auto &wire: {std::vector<std::uint8_t>{4, 1, 0, 0}, std::vector<std::uint8_t>{24, 0, 0, 1, 0}}) {
        with_server_flight([&](auto &client, auto &flight, auto &) {
            using fiber::quic::QuicEncryptionLevel;
            ASSERT_TRUE(client.tls().provide_crypto_data(QuicEncryptionLevel::Handshake, flight));
            ASSERT_TRUE(client.tls().drive_handshake());
            auto input = crypto_chain(wire);
            ASSERT_TRUE(client.tls().provide_crypto_data(QuicEncryptionLevel::Application, input));
            EXPECT_FALSE(client.tls().process_post_handshake());
            EXPECT_TRUE(client.terminal_closing());
            EXPECT_EQ(client.tls().last_alert(),
                      static_cast<std::uint8_t>(wire[0] == 4 ? fiber::tls::TlsAlertDesc::DecodeError
                                                             : fiber::tls::TlsAlertDesc::UnexpectedMessage));
        });
    }
}
