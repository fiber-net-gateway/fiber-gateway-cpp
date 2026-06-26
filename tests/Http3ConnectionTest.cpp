#include <gtest/gtest.h>

#include "http/Http3Connection.h"
#include "quic/QuicConnection.h"

#include "QuicTestLoop.h"

TEST(Http3ConnectionTest, StartsOverOpenQuicConnection) {
    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &fiber::test::quic_loop();
    fiber::quic::QuicConnection quic(quic_options);
    fiber::http::Http3Connection h3(quic);

    auto result = h3.start();

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Running);
    EXPECT_EQ(h3.role(), fiber::quic::QuicConnectionRole::Server);
}

TEST(Http3ConnectionTest, AppliesPeerSettingsOnce) {
    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &fiber::test::quic_loop();
    fiber::quic::QuicConnection quic(quic_options);
    fiber::http::Http3Connection h3(quic);
    fiber::http::Http3Settings settings{};
    settings.qpack_blocked_streams = 8;

    auto first = h3.apply_peer_settings(settings);
    auto second = h3.apply_peer_settings(settings);

    EXPECT_TRUE(first.has_value());
    EXPECT_FALSE(second.has_value());
    EXPECT_TRUE(h3.peer_settings_received());
    EXPECT_EQ(h3.peer_settings().qpack_blocked_streams, 8U);
}
