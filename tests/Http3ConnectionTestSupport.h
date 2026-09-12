#ifndef FIBER_TEST_HTTP3_CONNECTION_SUPPORT_H
#define FIBER_TEST_HTTP3_CONNECTION_SUPPORT_H
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fiber/common/Assert.h>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/Http3Protocol.h>
#include <fiber/http/Http3QpackStaticTable.h>
#include <fiber/http/HttpHeaderHash.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/quic/QuicConnection.h>
#include <fiber/quic/QuicCursor.h>
#include <fiber/quic/QuicFrame.h>
#include "http/Http3ClientConnectionImpl.h"
#include "http/Http3QpackEncoderIoBufWriter.h"
#include "http/Http3ServerConnection.h"
#include "http/ServerHttp3Request.h"
#include "quic/QuicTransportCodec.h"
#include "quic/QuicTransportParamsCodec.h"

#include "QuicTestLoop.h"

namespace {
using namespace std::chrono_literals;

class ServerFixture {
public:
    explicit ServerFixture(
            fiber::quic::QuicUdpEndpoint &endpoint, const fiber::quic::QuicConnection::Options &options,
            fiber::http::Http3ServerOptions http_options = {},
            fiber::http::HttpHandler handler = [](fiber::http::HttpExchange &) -> fiber::async::Task<void> {
                co_return;
            }) : loop_(endpoint.loop()), options_(http_options), closed_future_(closed_.get_future()) {
        static const fiber::http::Http3ServerConnection::Ops ops{
                [](void *owner, fiber::http::Http3ServerConnection &) noexcept {
                    static_cast<ServerFixture *>(owner)->closed_.set_value();
                }};
        connection_ = fiber::http::Http3ServerConnection::create(
                endpoint, options, std::make_shared<const fiber::http::HttpHandler>(std::move(handler)), options_, this,
                ops);
        FIBER_ASSERT(connection_ != nullptr);
        lease_ = fiber::quic::QuicConnection::Lease::adopt(&connection_->quic());
    }
    ~ServerFixture() { finish(); }
    fiber::http::Http3ServerConnection &connection() { return *connection_; }
    void finish() {
        if (connection_ == nullptr) {
            return;
        }
        fiber::async::spawn(loop_, [this]() -> fiber::async::DetachedTask {
            connection_->close();
            connection_->quic().mark_closed();
            co_await connection_->wait_closed();
            lease_.reset();
        });
        FIBER_ASSERT(closed_future_.wait_for(5s) == std::future_status::ready);
        connection_ = nullptr;
    }

private:
    fiber::event::EventLoop &loop_;
    fiber::http::Http3ServerOptions options_;
    std::promise<void> closed_;
    std::future<void> closed_future_;
    fiber::http::Http3ServerConnection *connection_ = nullptr;
    fiber::quic::QuicConnection::Lease lease_;
};
class ClientFixture {
public:
    explicit ClientFixture(fiber::quic::QuicUdpEndpoint &endpoint, const fiber::quic::QuicConnection::Options &options,
                           fiber::http::Http3ClientConnectionImpl::Options http_options = {}) : loop_(endpoint.loop()) {
        connection_ = fiber::http::Http3ClientConnectionImpl::create(endpoint, options, http_options);
        FIBER_ASSERT(connection_ != nullptr);
        lease_ = fiber::quic::QuicConnection::Lease::adopt(&connection_->quic());
    }
    ~ClientFixture() { finish(); }
    fiber::http::Http3ClientConnectionImpl &connection() { return *connection_; }
    void finish() {
        if (connection_ == nullptr) {
            return;
        }
        std::promise<void> done;
        auto future = done.get_future();
        fiber::async::spawn(loop_, [this, &done]() -> fiber::async::DetachedTask {
            connection_->close();
            connection_->quic().mark_closed();
            co_await connection_->wait_closed();
            lease_.reset();
            // Cleanup was queued by the last release before this loop barrier.
            fiber::async::spawn(loop_, [&done]() -> fiber::async::DetachedTask {
                done.set_value();
                co_return;
            });
        });
        FIBER_ASSERT(future.wait_for(5s) == std::future_status::ready);
        connection_ = nullptr;
    }

private:
    fiber::event::EventLoop &loop_;
    fiber::http::Http3ClientConnectionImpl *connection_ = nullptr;
    fiber::quic::QuicConnection::Lease lease_;
};
// Component fixture owns no Request and can therefore use stack QUIC storage.
class ControlFixture {
public:
    explicit ControlFixture(
            fiber::quic::QuicUdpEndpoint &endpoint, const fiber::quic::QuicConnection::Options &options,
            fiber::http::Http3ErrorCode push_result = fiber::http::Http3ErrorCode::StreamCreationError) :
        push_result_(push_result), quic_(endpoint, options), gate_(quic_),
        control_(quic_, gate_, {}, this, control_ops()) {
        fiber::quic::QuicConnection::Ops ops{.create_stream =
                                                     [](void *, std::uint64_t) noexcept {
                                                         return fiber::http::Http3ControlStreams::create_stream();
                                                     },
                                             .on_peer_stream_attached =
                                                     [](void *p, fiber::quic::QuicStream &s) noexcept {
                                                         static_cast<ControlFixture *>(p)->control_.accept_peer_stream(
                                                                 s);
                                                     },
                                             .on_state_change =
                                                     [](void *p, fiber::quic::QuicConnection &) noexcept {
                                                         static_cast<ControlFixture *>(p)->gate_.on_state_change();
                                                     },
                                             .on_capacity_change =
                                                     [](void *p, fiber::quic::QuicConnection &) noexcept {
                                                         static_cast<ControlFixture *>(p)->gate_.on_capacity_change();
                                                     }};
        FIBER_ASSERT(quic_.set_app_ops(this, ops));
    }
    fiber::quic::QuicConnection &quic() { return quic_; }
    auto start() { return control_.start(); }
    void close(fiber::http::Http3ErrorCode error = fiber::http::Http3ErrorCode::NoError) {
        if (closing_) {
            return;
        }
        closing_ = true;
        error_ = error;
        gate_.cancel_all(fiber::common::IoErr::Canceled);
        control_.stop(error);
        quic_.close_application(static_cast<std::uint64_t>(error));
    }
    auto wait_closed() { return control_.join_readers(); }
    void stop_readers_only() { control_.stop(fiber::http::Http3ErrorCode::NoError); }
    auto close_error() const { return error_; }
    auto apply_peer_settings(const fiber::http::Http3Settings &s) { return control_.apply_peer_settings(s); }
    auto peer_settings_received() const { return control_.peer_settings_received(); }
    const auto &peer_settings() const { return control_.peer_settings(); }
    bool peer_control_stream_seen() const { return control_.peer_control_stream_seen(); }
    bool peer_qpack_encoder_stream_seen() const { return control_.peer_qpack_encoder_stream_seen(); }
    bool peer_qpack_decoder_stream_seen() const { return control_.peer_qpack_decoder_stream_seen(); }
    void finish() {
        std::promise<void> done;
        auto future = done.get_future();
        fiber::async::spawn(quic_.loop(), [this, &done]() -> fiber::async::DetachedTask {
            close();
            quic_.mark_closed();
            co_await wait_closed();
            done.set_value();
        });
        FIBER_ASSERT(future.wait_for(5s) == std::future_status::ready);
    }

private:
    static fiber::http::Http3ControlStreams::Ops control_ops() noexcept {
        return {[](void *, const fiber::http::Http3ControlStreamEvent &) noexcept {
                    return fiber::http::Http3ErrorCode::NoError;
                },
                [](void *owner) noexcept { return static_cast<ControlFixture *>(owner)->push_result_; },
                [](void *p, fiber::http::Http3ErrorCode e) noexcept { static_cast<ControlFixture *>(p)->close(e); }};
    }
    fiber::http::Http3ErrorCode push_result_;
    fiber::quic::QuicConnection quic_;
    fiber::quic::QuicLocalStreamGate gate_;
    fiber::http::Http3ControlStreams control_;
    fiber::http::Http3ErrorCode error_ = fiber::http::Http3ErrorCode::NoError;
    bool closing_ = false;
};


using namespace std::chrono_literals;

struct StartResult {
    bool ok = false;
    fiber::common::IoErr error = fiber::common::IoErr::None;
};

struct CapturedHttp3Request {
    fiber::http::HttpMethod method = fiber::http::HttpMethod::Unknown;
    fiber::http::HttpVersion version = fiber::http::HttpVersion::HTTP_0_9;
    std::string method_view;
    std::string scheme;
    std::string protocol;
    std::string unparsed_uri;
    std::string path;
    std::string query;
    std::string exten;
    std::string host;
    std::string content_type;
    std::string range;
    std::string if_range;
    std::string expect;
    std::string accept_encoding;
    std::string accept_language;
    bool method_uses_static_storage = false;
    bool accept_encoding_uses_static_storage = false;
    bool accept_language_name_uses_static_storage = false;
    fiber::http::HttpBodySpec body_spec{fiber::http::HttpBodySpec::None()};
};

struct ServerRequestContext {
    fiber::http::Http3ServerOptions options{};
    fiber::http::HttpHandler handler;
};

struct Http3RequestRunResult {
    std::future_status handler_status = std::future_status::timeout;
    CapturedHttp3Request snapshot{};
};

struct Http3BodyReadOutcome {
    fiber::common::IoErr first_error = fiber::common::IoErr::None;
    fiber::common::IoErr second_error = fiber::common::IoErr::None;
    std::string first_body;
    std::string second_body;
    std::string trailer_value;
    bool first_complete = false;
    bool second_complete = false;
    bool trailers_complete = false;
    fiber::http::HttpBodySpec body_spec{fiber::http::HttpBodySpec::None()};
};

struct Http3BodyWriteOutcome {
    fiber::common::IoErr header_error = fiber::common::IoErr::None;
    fiber::common::IoErr body_error = fiber::common::IoErr::None;
    std::size_t written = 0;
    std::size_t remaining = 0;
    std::size_t foreign_pool_cached_after_write = 0;
    bool header_ok = false;
    bool body_ok = false;
    bool complete = false;
};

struct Http3WriteTimeoutOutcome {
    fiber::common::IoErr header_error = fiber::common::IoErr::None;
    fiber::common::IoErr first_error = fiber::common::IoErr::None;
    fiber::common::IoErr retry_error = fiber::common::IoErr::None;
    fiber::common::IoErr terminal_error = fiber::common::IoErr::None;
    std::size_t remaining = 0;
    bool header_ok = false;
    bool complete = false;
    bool response_channel_closed = false;
};

struct ClientRequestNotification {
    std::size_t rejected = 0;
    std::size_t closed = 0;
    std::uint64_t goaway_id = 0;
    fiber::http::Http3ErrorCode close_error = fiber::http::Http3ErrorCode::NoError;
};

void on_client_request_rejected(void *owner, std::uint64_t goaway_id) noexcept {
    auto &notification = *static_cast<ClientRequestNotification *>(owner);
    ++notification.rejected;
    notification.goaway_id = goaway_id;
}

void on_client_request_closed(void *owner, fiber::http::Http3ErrorCode error) noexcept {
    auto &notification = *static_cast<ClientRequestNotification *>(owner);
    ++notification.closed;
    notification.close_error = error;
}

using HeaderList = std::vector<std::pair<std::string_view, std::string_view>>;

fiber::quic::QuicConnectionId connection_id_from(std::initializer_list<std::uint8_t> bytes) {
    auto id = fiber::quic::QuicConnectionId::from_bytes(bytes.begin(), bytes.size());
    return id.value_or(fiber::quic::QuicConnectionId{});
}

StartResult to_start_result(fiber::common::IoResult<void> result) noexcept {
    if (result) {
        return {.ok = true};
    }
    return {.ok = false, .error = result.error()};
}

std::string field_value(const fiber::http::HttpHeaders::HeaderField *field) {
    if (field == nullptr) {
        return {};
    }
    return std::string(field->value_view());
}

const fiber::http::HttpHeaders::HeaderField *find_field(const fiber::http::HttpExchange &exchange,
                                                        std::string_view name) {
    for (const auto &field: exchange.request_headers()) {
        if (field.name_view() == name) {
            return &field;
        }
    }
    return nullptr;
}

bool field_uses_static_storage(const fiber::http::HttpExchange &exchange, std::string_view name,
                               std::uint32_t static_index, bool check_value) {
    fiber::http::Http3QpackStaticTable::TableEntryView entry;
    if (!fiber::http::Http3QpackStaticTable::get_by_index(static_index, entry)) {
        return false;
    }
    const auto *field = find_field(exchange, name);
    return field != nullptr && field->name == entry.name.data() && (!check_value || field->value == entry.value.data());
}

CapturedHttp3Request capture_request(const fiber::http::HttpExchange &exchange) {
    fiber::http::Http3QpackStaticTable::TableEntryView method_entry;
    const bool have_method_entry = fiber::http::Http3QpackStaticTable::get_by_index(17, method_entry);
    return CapturedHttp3Request{
            .method = exchange.method(),
            .version = exchange.version(),
            .method_view = std::string(exchange.method_view()),
            .scheme = std::string(exchange.scheme()),
            .protocol = std::string(exchange.protocol()),
            .unparsed_uri = std::string(exchange.uri().unparsed_uri),
            .path = std::string(exchange.uri().path),
            .query = std::string(exchange.uri().query),
            .exten = std::string(exchange.uri().exten),
            .host = field_value(exchange.host_header()),
            .content_type = field_value(exchange.content_type_header()),
            .range = field_value(exchange.range_header()),
            .if_range = field_value(exchange.if_range_header()),
            .expect = field_value(exchange.expect_header()),
            .accept_encoding = std::string(exchange.request_headers().get("accept-encoding")),
            .accept_language = std::string(exchange.request_headers().get("accept-language")),
            .method_uses_static_storage =
                    have_method_entry && exchange.method_view().data() == method_entry.value.data(),
            .accept_encoding_uses_static_storage = field_uses_static_storage(exchange, "accept-encoding", 31, true),
            .accept_language_name_uses_static_storage =
                    field_uses_static_storage(exchange, "accept-language", 72, false),
            .body_spec = exchange.request_body_spec(),
    };
}

fiber::quic::QuicTransportParams valid_peer_transport_params(const fiber::quic::QuicConnection::Options &options,
                                                             std::uint64_t initial_max_stream_data_bidi_local = 1024) {
    fiber::quic::QuicTransportParams params{};
    params.has_initial_source_connection_id = true;
    params.initial_source_connection_id = options.remote_connection_id;
    params.max_udp_payload_size = fiber::quic::kMinInitialDatagramSize;
    params.active_connection_id_limit = 2;
    params.initial_max_data = 4096;
    params.initial_max_stream_data_bidi_local = initial_max_stream_data_bidi_local;
    params.initial_max_stream_data_bidi_remote = 1024;
    params.initial_max_stream_data_uni = 1024;
    params.initial_max_streams_uni = 8;
    params.initial_max_streams_bidi = 8;
    return params;
}

template<typename Connection>
fiber::async::DetachedTask start_h3(Connection *h3, std::promise<StartResult> *done) {
    if constexpr (std::is_same_v<Connection, fiber::http::Http3ServerConnection>) {
        h3->start();
        co_await h3->wait_started();
        done->set_value({.ok = h3->state() == fiber::http::Http3ConnectionState::Running});
    } else {
        auto result = co_await h3->start();
        done->set_value(to_start_result(result));
    }
}

template<typename Connection>
StartResult start_h3_on_loop(fiber::event::EventLoop &loop, fiber::quic::QuicConnection &quic,
                             const fiber::quic::QuicConnection::Options &options, Connection &h3,
                             std::uint64_t initial_max_stream_data_bidi_local = 1024) {
    std::promise<StartResult> done;
    auto future = done.get_future();
    fiber::async::spawn(
            loop, [&quic, &options, &h3, &done, initial_max_stream_data_bidi_local]() -> fiber::async::DetachedTask {
                auto params = valid_peer_transport_params(options, initial_max_stream_data_bidi_local);
                if (options.role == fiber::quic::QuicConnectionRole::Client) {
                    auto adopted = quic.adopt_server_initial_source_connection_id(options.remote_connection_id);
                    if (!adopted) {
                        done.set_value(to_start_result(adopted));
                        co_return;
                    }
                    params.has_original_destination_connection_id = true;
                    params.original_destination_connection_id = options.original_destination_connection_id;
                }
                auto applied = quic.apply_peer_transport_params(params);
                if (!applied) {
                    done.set_value(to_start_result(applied));
                    co_return;
                }
                auto established = quic.mark_established();
                if (!established) {
                    done.set_value(to_start_result(established));
                    co_return;
                }
                fiber::async::spawn(fiber::event::EventLoop::current(),
                                    [&h3, &done]() { return start_h3(&h3, &done); });
            });
    if (future.wait_for(2s) != std::future_status::ready) {
        return {.ok = false, .error = fiber::common::IoErr::TimedOut};
    }
    return future.get();
}

void append_varint(std::vector<std::uint8_t> &out, std::uint64_t value) {
    std::array<std::uint8_t, 8> buf{};
    fiber::quic::QuicWriteCursor cursor(buf.data(), buf.size());
    ASSERT_TRUE(fiber::quic::quic_write_varint(cursor, value).has_value());
    out.insert(out.end(), buf.data(), buf.data() + cursor.offset());
}

std::vector<std::uint8_t> chain_to_bytes(fiber::mem::IoBufChain chain) {
    std::vector<std::uint8_t> out;
    out.reserve(chain.readable_bytes());
    while (auto *front = chain.front()) {
        if (front->readable() == 0) {
            chain.drop_empty_front();
            continue;
        }
        const std::uint8_t *data = front->readable_data();
        out.insert(out.end(), data, data + front->readable());
        chain.consume_and_compact(front->readable());
    }
    return out;
}

std::vector<std::uint8_t> qpack_header_block(const HeaderList &headers) {
    fiber::mem::IoBufNodePool pool;
    fiber::http::Http3QpackEncoderIoBufWriter writer(
            pool, fiber::http::Http3QpackEncoder::Options{.huffman_threshold = 1024});
    for (const auto &[name, value]: headers) {
        EXPECT_EQ(writer.encode_field(name, fiber::http::http_header_name_hash(name), value),
                  fiber::common::IoErr::None);
    }

    fiber::mem::IoBufChain block(pool);
    EXPECT_EQ(writer.finish(block), fiber::common::IoErr::None);
    return chain_to_bytes(std::move(block));
}

std::vector<std::uint8_t> headers_frame(const HeaderList &headers) {
    std::vector<std::uint8_t> block = qpack_header_block(headers);
    std::vector<std::uint8_t> out;
    append_varint(out, static_cast<std::uint64_t>(fiber::http::Http3FrameType::Headers));
    append_varint(out, block.size());
    out.insert(out.end(), block.begin(), block.end());
    return out;
}

std::vector<std::uint8_t> data_frame(std::string_view body) {
    std::vector<std::uint8_t> out;
    append_varint(out, static_cast<std::uint64_t>(fiber::http::Http3FrameType::Data));
    append_varint(out, body.size());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

void append_frame(std::vector<std::uint8_t> &request, const std::vector<std::uint8_t> &frame) {
    request.insert(request.end(), frame.begin(), frame.end());
}

std::vector<std::uint8_t> control_settings_stream(std::uint64_t blocked_streams = 0) {
    std::vector<std::uint8_t> out;
    append_varint(out, static_cast<std::uint64_t>(fiber::http::Http3StreamType::Control));
    append_varint(out, static_cast<std::uint64_t>(fiber::http::Http3FrameType::Settings));

    std::vector<std::uint8_t> payload;
    append_varint(payload, static_cast<std::uint64_t>(fiber::http::Http3SettingId::QpackBlockedStreams));
    append_varint(payload, blocked_streams);
    append_varint(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

void append_control_varint_frame(std::vector<std::uint8_t> &out, fiber::http::Http3FrameType type,
                                 std::uint64_t value) {
    std::vector<std::uint8_t> payload;
    append_varint(payload, value);
    append_varint(out, static_cast<std::uint64_t>(type));
    append_varint(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

std::vector<std::uint8_t> uni_stream_type(fiber::http::Http3StreamType type) {
    std::vector<std::uint8_t> out;
    append_varint(out, static_cast<std::uint64_t>(type));
    return out;
}

void feed_stream(fiber::quic::QuicConnection &conn, std::uint64_t stream_id, const std::vector<std::uint8_t> &data,
                 bool fin = false, std::uint64_t offset = 0) {
    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = stream_id;
    frame.offset = offset;
    frame.length = data.size();
    frame.fin = fin;
    fiber::mem::IoBuf payload = fiber::mem::IoBuf::allocate(data.size());
    if (!data.empty()) {
        ASSERT_TRUE(payload);
        std::memcpy(payload.writable_data(), data.data(), data.size());
        payload.commit(data.size());
    }
    ASSERT_TRUE(conn.recv_stream_frame(frame, std::move(payload)).has_value());
}

template<typename Connection>
fiber::async::DetachedTask close_and_wait(Connection *h3, std::promise<void> *done) {
    h3->close();
    co_await h3->wait_closed();
    done->set_value();
}

fiber::async::DetachedTask feed_request_stream_then_wait(fiber::quic::QuicConnection *conn,
                                                         const std::vector<std::uint8_t> *data,
                                                         std::promise<void> *done) {
    feed_stream(*conn, 0, *data, true);
    co_await fiber::async::sleep(20ms);
    done->set_value();
}

fiber::async::DetachedTask feed_request_stream_then_delayed_fin(fiber::quic::QuicConnection *conn,
                                                                const std::vector<std::uint8_t> *data,
                                                                std::promise<void> *done) {
    feed_stream(*conn, 0, *data);
    co_await fiber::async::sleep(20ms);
    const std::vector<std::uint8_t> empty;
    feed_stream(*conn, 0, empty, true, data->size());
    co_await fiber::async::sleep(20ms);
    done->set_value();
}

template<typename Connection>
fiber::async::DetachedTask feed_one_stream_then_close(fiber::quic::QuicConnection *conn, Connection *h3,
                                                      const std::vector<std::uint8_t> *data, std::uint64_t stream_id,
                                                      bool fin, std::promise<void> *done) {
    feed_stream(*conn, stream_id, *data, fin);
    fiber::async::spawn(fiber::event::EventLoop::current(),
                        [h3, done]() -> fiber::async::DetachedTask { return close_and_wait(h3, done); });
    co_return;
}

template<typename Connection>
fiber::async::DetachedTask
feed_two_streams_then_wait(fiber::quic::QuicConnection *conn, Connection *h3, const std::vector<std::uint8_t> *first,
                           const std::vector<std::uint8_t> *second, std::promise<void> *done) {
    feed_stream(*conn, 2, *first);
    feed_stream(*conn, 6, *second);
    fiber::async::spawn(fiber::event::EventLoop::current(), [h3, done]() -> fiber::async::DetachedTask {
        co_await h3->wait_closed();
        done->set_value();
    });
    co_return;
}

fiber::async::DetachedTask feed_stream_then_delay(fiber::quic::QuicConnection *conn,
                                                  const std::vector<std::uint8_t> *data, std::uint64_t stream_id,
                                                  std::promise<void> *done) {
    feed_stream(*conn, stream_id, *data);
    co_await fiber::async::sleep(20ms);
    done->set_value();
}

template<typename Connection>
fiber::async::DetachedTask feed_stream_then_wait_closed(fiber::quic::QuicConnection *conn, Connection *h3,
                                                        const std::vector<std::uint8_t> *data, std::uint64_t stream_id,
                                                        std::promise<void> *done) {
    feed_stream(*conn, stream_id, *data);
    co_await h3->wait_closed();
    done->set_value();
}


} // namespace
#endif
