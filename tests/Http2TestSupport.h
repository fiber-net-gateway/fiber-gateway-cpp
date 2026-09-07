#ifndef FIBER_TESTS_HTTP2_TEST_SUPPORT_H
#define FIBER_TESTS_HTTP2_TEST_SUPPORT_H

#include <cstdint>
#include <new>
#include <string>

#include <fiber/http/Http2CloseGate.h>
#include <fiber/http/Http2Connection.h>
#include <fiber/http/Http2LocalStreamGate.h>
#include <fiber/http/HttpHeaderHash.h>
#include "http/Huffman.h"

namespace {

// Test owner composes the factory and independently configurable observers into
// the single context accepted by the production connection.
class TestHttp2Connection : public fiber::http::Http2Connection {
public:
    using Callback = void (*)(void *, fiber::http::Http2Connection &) noexcept;
    TestHttp2Connection(Options options, void *factory_ctx, const Ops &factory_ops) :
        Http2Connection(options, this, owner_ops(options.role)), factory_ctx_(factory_ctx), factory_ops_(factory_ops) {}
    void observe_capacity(Callback callback, void *ctx) noexcept {
        capacity_ = callback;
        capacity_ctx_ = ctx;
    }
    void clear_capacity_observer() noexcept { capacity_ = nullptr; }
    void observe_state(Callback callback, void *ctx) noexcept {
        state_observer_ = callback;
        state_observer_ctx_ = ctx;
    }
    void observe_close_gate(fiber::http::Http2CloseGate &gate) noexcept { close_gate_ = &gate; }
    void observe_stream_gate(fiber::http::Http2LocalStreamGate &gate) noexcept { stream_gate_ = &gate; }

private:
    // Clients must not carry a peer-stream factory: they never accept a pushed
    // stream, so the connection asserts the slot is empty. The factory the
    // caller passed then stays unused, which keeps every call site uniform.
    static const Ops &owner_ops(ConnectionRole role) noexcept {
        static const Ops server_ops{&create, &state_observer_changed, &capacity_changed};
        static const Ops client_ops{nullptr, &state_observer_changed, &capacity_changed};
        return role == ConnectionRole::Client ? client_ops : server_ops;
    }
    static fiber::http::Http2Stream::Lease create(void *ctx, std::uint32_t id,
                                                  fiber::http::Http2Connection &conn) noexcept {
        auto &self = *static_cast<TestHttp2Connection *>(ctx);
        return self.factory_ops_.create_peer_stream(self.factory_ctx_, id, conn);
    }
    static void state_observer_changed(void *ctx, fiber::http::Http2Connection &conn) noexcept {
        auto &self = *static_cast<TestHttp2Connection *>(ctx);
        if (self.stream_gate_)
            self.stream_gate_->on_state_change();
        if (self.state_observer_)
            self.state_observer_(self.state_observer_ctx_, conn);
        if (conn.state() == State::Closed && self.close_gate_)
            self.close_gate_->on_connection_closed();
    }
    static void capacity_changed(void *ctx, fiber::http::Http2Connection &conn) noexcept {
        auto &self = *static_cast<TestHttp2Connection *>(ctx);
        if (self.stream_gate_)
            self.stream_gate_->on_capacity_change();
        if (self.capacity_)
            self.capacity_(self.capacity_ctx_, conn);
    }
    void *factory_ctx_;
    Ops factory_ops_;
    Callback capacity_ = nullptr;
    void *capacity_ctx_ = nullptr;
    Callback state_observer_ = nullptr;
    void *state_observer_ctx_ = nullptr;
    fiber::http::Http2CloseGate *close_gate_ = nullptr;
    fiber::http::Http2LocalStreamGate *stream_gate_ = nullptr;
};

struct TestHttp2StreamOwner {
    TestHttp2StreamOwner() : stream(this, ops()) {}

    static TestHttp2StreamOwner *create_owner() noexcept { return new (std::nothrow) TestHttp2StreamOwner(); }

    static fiber::http::Http2Stream::Lease create() noexcept {
        auto *owner = create_owner();
        if (!owner) {
            return {};
        }
        return fiber::http::Http2Stream::Lease::adopt(&owner->stream);
    }

    static const fiber::http::Http2Stream::Ops &ops() noexcept {
        static const fiber::http::Http2HpackDecoder::Ops kDecoderOps{
                &TestHttp2StreamOwner::on_indexed_field, &TestHttp2StreamOwner::on_indexed_name,
                &TestHttp2StreamOwner::on_name_raw,      &TestHttp2StreamOwner::on_name_huffman,
                &TestHttp2StreamOwner::on_value_raw,     &TestHttp2StreamOwner::on_value_huffman,
        };
        static const fiber::http::Http2Stream::Ops kOps{
                &TestHttp2StreamOwner::destroy_owner,
                &TestHttp2StreamOwner::on_header_block_start,
                &TestHttp2StreamOwner::on_header_block_complete,
                &TestHttp2StreamOwner::on_body,
                &TestHttp2StreamOwner::on_abort,
        };
        (void) kDecoderOps;
        return kOps;
    }

    static void destroy_owner(void *owner) noexcept { delete static_cast<TestHttp2StreamOwner *>(owner); }
    static fiber::common::IoErr on_header_block_start(void *owner,
                                                      fiber::http::Http2HpackDecoder::Sink &sink) noexcept {
        auto *self = static_cast<TestHttp2StreamOwner *>(owner);
        if (self->reading_trailers || self->trailers_complete) {
            return fiber::common::IoErr::Invalid;
        }
        if (self->headers_received) {
            self->reading_trailers = true;
        }
        sink.ctx = owner;
        sink.ops = &decoder_ops();
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_header_block_complete(void *owner, bool end_stream) noexcept {
        auto *self = static_cast<TestHttp2StreamOwner *>(owner);
        if (!self->headers_received) {
            self->headers_received = true;
            if (end_stream) {
                self->trailers_complete = true;
            }
            return fiber::common::IoErr::None;
        }
        if (!self->reading_trailers || !end_stream) {
            return fiber::common::IoErr::Invalid;
        }
        self->trailers_complete = true;
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_body(void *owner, fiber::mem::IoBuf &&, bool end_stream) noexcept {
        auto *self = static_cast<TestHttp2StreamOwner *>(owner);
        if (!self->headers_received || self->reading_trailers || self->trailers_complete) {
            return fiber::common::IoErr::Invalid;
        }
        if (end_stream) {
            self->trailers_complete = true;
        }
        return fiber::common::IoErr::None;
    }
    static void on_abort(void *, fiber::common::IoErr) noexcept {}
    static fiber::common::IoErr on_indexed_field(void *, fiber::http::Http2HpackDecoder::TableEntryView) noexcept {
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_indexed_name(void *owner,
                                                fiber::http::Http2HpackDecoder::TableEntryView entry) noexcept {
        auto *self = static_cast<TestHttp2StreamOwner *>(owner);
        self->pending_name_storage.assign(entry.name.data(), entry.name.size());
        self->pending_name_hash = entry.name_hash;
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_name_raw(void *owner, const std::uint8_t *data, std::size_t len) noexcept {
        auto *self = static_cast<TestHttp2StreamOwner *>(owner);
        self->pending_name_storage.assign(reinterpret_cast<const char *>(data), len);
        self->pending_name_hash = fiber::http::http_header_name_hash(self->pending_name_storage);
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_name_huffman(void *owner, const std::uint8_t *data, std::size_t len) noexcept {
        auto *self = static_cast<TestHttp2StreamOwner *>(owner);
        bool ok = false;
        const std::size_t decoded_len = fiber::http::hpack_huffman_decoded_length(data, len, &ok);
        if (!ok) {
            return fiber::common::IoErr::Invalid;
        }
        self->pending_name_storage.assign(decoded_len, '\0');
        fiber::http::HpackHuffmanDecodeState state;
        fiber::http::HpackHuffmanDecodeResult result = fiber::http::hpack_huffman_decode(
                state, data, len, reinterpret_cast<std::uint8_t *>(self->pending_name_storage.data()), decoded_len,
                true);
        if (result.code != fiber::http::HpackHuffmanCode::Ok || result.written != decoded_len) {
            return fiber::common::IoErr::Invalid;
        }
        self->pending_name_hash = fiber::http::http_header_name_hash(self->pending_name_storage);
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_value_raw(void *owner, const std::uint8_t *data, std::size_t len,
                                             fiber::http::Http2HpackDecoder::FieldView *out) noexcept {
        auto *self = static_cast<TestHttp2StreamOwner *>(owner);
        self->pending_value_storage.assign(reinterpret_cast<const char *>(data), len);
        if (out != nullptr) {
            out->name = self->pending_name_storage;
            out->name_hash = self->pending_name_hash;
            out->value = self->pending_value_storage;
        }
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_value_huffman(void *owner, const std::uint8_t *data, std::size_t len,
                                                 fiber::http::Http2HpackDecoder::FieldView *out) noexcept {
        auto *self = static_cast<TestHttp2StreamOwner *>(owner);
        bool ok = false;
        const std::size_t decoded_len = fiber::http::hpack_huffman_decoded_length(data, len, &ok);
        if (!ok) {
            return fiber::common::IoErr::Invalid;
        }
        self->pending_value_storage.assign(decoded_len, '\0');
        fiber::http::HpackHuffmanDecodeState state;
        fiber::http::HpackHuffmanDecodeResult result = fiber::http::hpack_huffman_decode(
                state, data, len, reinterpret_cast<std::uint8_t *>(self->pending_value_storage.data()), decoded_len,
                true);
        if (result.code != fiber::http::HpackHuffmanCode::Ok || result.written != decoded_len) {
            return fiber::common::IoErr::Invalid;
        }
        if (out != nullptr) {
            out->name = self->pending_name_storage;
            out->name_hash = self->pending_name_hash;
            out->value = self->pending_value_storage;
        }
        return fiber::common::IoErr::None;
    }
    static const fiber::http::Http2HpackDecoder::Ops &decoder_ops() noexcept {
        static const fiber::http::Http2HpackDecoder::Ops kOps{
                &TestHttp2StreamOwner::on_indexed_field, &TestHttp2StreamOwner::on_indexed_name,
                &TestHttp2StreamOwner::on_name_raw,      &TestHttp2StreamOwner::on_name_huffman,
                &TestHttp2StreamOwner::on_value_raw,     &TestHttp2StreamOwner::on_value_huffman,
        };
        return kOps;
    }

    fiber::http::Http2Stream stream;
    std::string pending_name_storage;
    std::string pending_value_storage;
    std::uint64_t pending_name_hash = 0;
    bool headers_received = false;
    bool reading_trailers = false;
    bool trailers_complete = false;
};

class TestHttp2StreamFactory {
public:
    [[nodiscard]] static const fiber::http::Http2Connection::Ops &ops() noexcept {
        static const fiber::http::Http2Connection::Ops kOps{
                &TestHttp2StreamFactory::create_peer_stream_op,
        };
        return kOps;
    }

    [[nodiscard]] fiber::http::Http2Stream::Lease create_peer_stream(std::uint32_t stream_id,
                                                                     fiber::http::Http2Connection &) noexcept {
        (void) stream_id;
        return TestHttp2StreamOwner::create();
    }

private:
    static fiber::http::Http2Stream::Lease create_peer_stream_op(void *ctx, std::uint32_t stream_id,
                                                                 fiber::http::Http2Connection &conn) noexcept {
        return static_cast<TestHttp2StreamFactory *>(ctx)->create_peer_stream(stream_id, conn);
    }
};

inline TestHttp2StreamFactory &test_http2_stream_factory() noexcept {
    static TestHttp2StreamFactory factory;
    return factory;
}

} // namespace

#endif // FIBER_TESTS_HTTP2_TEST_SUPPORT_H
