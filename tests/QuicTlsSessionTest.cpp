#include <gtest/gtest.h>

#include <array>
#include <cstdint>
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

// The CRYPTO bytes a connection queued at `level`, in offset order.
std::vector<std::uint8_t> queued_crypto(fiber::quic::QuicConnection &connection,
                                        fiber::quic::QuicEncryptionLevel level) {
    std::vector<std::uint8_t> out;
    auto &space = connection.packet_number_space(level);
    for (const fiber::quic::QuicOutputFrame *frame = space.pending_frames.front(); frame != nullptr;
         frame = space.pending_frames.next_of(*frame)) {
        if (frame->type == fiber::quic::QuicFrameType::Crypto && frame->u.crypto.data != nullptr &&
            frame->u.crypto.offset == out.size()) {
            const fiber::mem::IoBuf &data = *frame->u.crypto.data;
            out.insert(out.end(), data.readable_data(), data.readable_data() + data.readable());
        }
    }
    return out;
}

} // namespace

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
        ASSERT_TRUE(client.tls().init_client(client_tls, client, /*allow_insecure=*/true));
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
        ASSERT_TRUE(server.tls().init_server(server_tls, server));
        ASSERT_TRUE(server.tls().provide_crypto_data(fiber::quic::QuicEncryptionLevel::Initial, hello.data(),
                                                     hello.size()));
        auto server_driven = server.tls().drive_handshake();
        ASSERT_TRUE(server_driven || server_driven.error() == fiber::common::IoErr::WouldBlock);
        ASSERT_FALSE(server.tls().handshake_done());
        ASSERT_FALSE(server.terminal_closing());

        // An incomplete message at 1-RTT (Finished header announcing 96 bytes,
        // 32 present), then bytes at the Handshake level below it.
        std::vector<std::uint8_t> partial{0x14, 0x00, 0x00, 0x60};
        partial.resize(partial.size() + 32, 0x42);
        ASSERT_TRUE(server.tls().provide_crypto_data(fiber::quic::QuicEncryptionLevel::Application, partial.data(),
                                                     partial.size()));
        auto lower = server.tls().provide_crypto_data(fiber::quic::QuicEncryptionLevel::Handshake, partial.data(),
                                                      partial.size());
        EXPECT_FALSE(lower.has_value());
        EXPECT_TRUE(server.terminal_closing());
        EXPECT_EQ(server.close_info().error_code, fiber::quic::quic_crypto_error_code(static_cast<std::uint8_t>(
                                                          fiber::tls::TlsAlertDesc::UnexpectedMessage)));
    });
}
