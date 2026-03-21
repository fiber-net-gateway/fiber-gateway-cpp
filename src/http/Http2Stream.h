#ifndef FIBER_HTTP_HTTP2_STREAM_H
#define FIBER_HTTP_HTTP2_STREAM_H

#include <cstddef>
#include <cstdint>

#include "../common/IntrusiveList.h"
#include "../common/IoError.h"
#include "../common/mem/IoBuf.h"
#include "Http2HpackDecoder.h"
#include "Http2Protocol.h"

namespace fiber::http {

class Http2Connection;

class Http2Stream {
public:
    struct Ops {
        void (*on_destroy)(void *owner) noexcept = nullptr;
        common::IoErr (*on_header_block_start)(void *owner, Http2HpackDecoder::Sink &sink) noexcept = nullptr;
        common::IoErr (*on_header_block_complete)(void *owner, bool end_stream) noexcept = nullptr;
        common::IoErr (*on_body)(void *owner, mem::IoBuf &&buf, bool end_stream) noexcept = nullptr;
    };

    class Lease {
    public:
        Lease() noexcept = default;
        explicit Lease(Http2Stream *stream) noexcept : stream_(stream) {
            if (stream_) {
                stream_->retain();
            }
        }

        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;

        Lease(Lease &&other) noexcept : stream_(other.stream_) { other.stream_ = nullptr; }

        Lease &operator=(Lease &&other) noexcept {
            if (this == &other) {
                return *this;
            }
            reset();
            stream_ = other.stream_;
            other.stream_ = nullptr;
            return *this;
        }

        ~Lease() { reset(); }

        void reset() noexcept {
            if (!stream_) {
                return;
            }
            Http2Stream *stream = stream_;
            stream_ = nullptr;
            stream->release();
        }

        [[nodiscard]] Http2Stream *release_raw() noexcept {
            Http2Stream *stream = stream_;
            stream_ = nullptr;
            return stream;
        }

        [[nodiscard]] Http2Stream *get() const noexcept { return stream_; }
        [[nodiscard]] Http2Stream &operator*() const noexcept { return *stream_; }
        [[nodiscard]] Http2Stream *operator->() const noexcept { return stream_; }
        [[nodiscard]] explicit operator bool() const noexcept { return stream_ != nullptr; }

        // `adopt` transfers an existing initial reference into the lease
        // without incrementing the stream ref-count. This is the right entry
        // point for newly allocated self-owned streams and embedded streams
        // whose owner has just been heap-allocated.
        [[nodiscard]] static Lease adopt(Http2Stream *stream) noexcept {
            Lease lease;
            lease.stream_ = stream;
            return lease;
        }

    private:
        Http2Stream *stream_ = nullptr;
    };

    Http2Stream(const Http2Stream &) = delete;
    Http2Stream &operator=(const Http2Stream &) = delete;
    Http2Stream(Http2Stream &&) = delete;
    Http2Stream &operator=(Http2Stream &&) = delete;

    Http2Stream(std::uint32_t stream_id, void *owner, const Ops &ops) noexcept;

    [[nodiscard]] std::uint32_t stream_id() const noexcept { return stream_id_; }
    [[nodiscard]] void *owner() noexcept { return owner_; }
    [[nodiscard]] const void *owner() const noexcept { return owner_; }
    [[nodiscard]] std::int32_t send_window() const noexcept { return send_window_; }
    [[nodiscard]] bool attached_to_connection() const noexcept { return attached_to_connection_; }
    [[nodiscard]] common::IoErr close_reason() const noexcept { return close_reason_; }
    [[nodiscard]] bool remote_end_headers() const noexcept { return remote_end_headers_; }
    [[nodiscard]] bool remote_end_stream() const noexcept { return remote_end_stream_; }
    [[nodiscard]] bool remote_trailer() const noexcept { return remote_trailer_; }
    [[nodiscard]] bool remote_rst() const noexcept { return remote_rst_; }
    [[nodiscard]] bool local_headers_sent() const noexcept { return local_headers_sent_; }
    [[nodiscard]] bool local_end_stream() const noexcept { return local_end_stream_; }
    [[nodiscard]] bool local_rst() const noexcept { return local_rst_; }
    [[nodiscard]] Lease lease() noexcept { return Lease(this); }

    [[nodiscard]] bool active() const noexcept { return active_; }
    void set_active(bool active) noexcept { active_ = active; }

    common::IoErr on_headers_payload_recv(const mem::IoBuf &payload, std::size_t offset, std::size_t length,
                                          bool end_headers, bool end_stream, bool trailer_block) noexcept;
    common::IoErr on_data_payload_recv(mem::IoBuf payload, std::size_t offset, std::size_t length,
                                       bool end_stream) noexcept;
    void on_rst_recv(Http2ErrorCode code, common::IoErr result = common::IoErr::Canceled) noexcept;
    common::IoErr close_rst(Http2ErrorCode code, common::IoErr result = common::IoErr::Canceled) noexcept;
    // Peer SETTINGS_INITIAL_WINDOW_SIZE can shrink after we have already
    // reserved/sent DATA on this stream, so the per-stream send window is
    // allowed to become negative until future WINDOW_UPDATE frames restore it.
    void update_send_window(std::int32_t delta) noexcept;
    void close(common::IoErr result = common::IoErr::Canceled) noexcept;

private:
    [[nodiscard]] bool ready_for_connection_release() const noexcept;
    [[nodiscard]] bool ready_for_destruction() const noexcept;
    void retain() noexcept;
    void release() noexcept;

    std::uint32_t stream_id_ = 0;
    bool remote_end_headers_ = false;
    bool remote_end_stream_ = false;
    bool remote_trailer_ = false;
    bool remote_rst_ = false;
    bool local_headers_sent_ = false;
    bool local_end_stream_ = false;
    bool local_rst_ = false;
    bool active_ = false;
    Http2Connection *conn_ = nullptr;
    // RFC 7540 allows the stream-level send window to become negative after a
    // smaller SETTINGS_INITIAL_WINDOW_SIZE is applied to in-flight streams.
    std::int32_t send_window_ = 65535;
    common::IntrusiveListHook owned_hook_{};
    void *owner_ = nullptr;
    const Ops *ops_ = nullptr;
    std::uint32_t ref_count_ = 1;
    bool attached_to_connection_ = false;
    common::IoErr close_reason_ = common::IoErr::None;
    bool remote_header_block_open_ = false;

    friend class Http2Connection;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_STREAM_H
