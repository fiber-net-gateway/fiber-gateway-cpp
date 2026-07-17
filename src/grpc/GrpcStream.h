#ifndef FIBER_GRPC_GRPC_STREAM_H
#define FIBER_GRPC_GRPC_STREAM_H

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include <google/protobuf/message_lite.h>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/mem/BufPool.h"
#include "../http/ClientHttp2Exchange.h"
#include "../http/ClientHttp2Types.h"
#include "../http/Http2ClientConnection.h"
#include "../http/HttpCommon.h"
#include "../http/HttpHeaders.h"
#include "GrpcFraming.h"
#include "GrpcStatus.h"
#include "ProtoCodec.h"

namespace fiber::grpc {

// Result of a single GrpcStream::read(): either a decoded message was produced
// (Message) or the server's response stream has ended (End). Transport/protocol
// failures are reported via IoErr on the IoResult instead.
enum class GrpcReadOutcome : std::uint8_t {
    Message,
    End,
};

// One full-duplex gRPC call over a single HTTP/2 stream. Owns the underlying
// ClientHttp2Exchange (and thus the stream lease) for the lifetime of the call,
// but only borrows its Http2ClientConnection. The GrpcClient that created this
// stream must outlive the stream and every coroutine driving it.
//
// The methods are coroutines driven by the caller on the client's event loop;
// no internal coroutines are spawned. The caller must keep at most one
// outstanding read and one outstanding write at a time (full-duplex: one of each
// concurrently is fine, since the exchange uses separate awaiter slots for
// send and receive).
//
// Failure handling:
//   - Any error from a read or write (including IoErr::TimedOut, which signals
//     the Options::deadline has elapsed) fails the call: the stream is
//     cancelled (RST_STREAM) and further calls return the abort reason.
//   - cancel() is synchronous, idempotent, and safe from any coroutine driving
//     the stream; it wakes a blocked read/write with Canceled.
class GrpcStream {
public:
    struct Options {
        // Call deadline. Sent as the grpc-timeout header (server-enforced) AND
        // enforced locally: each read/write on the stream is bounded by the time
        // remaining until the deadline, so a hung call fails with TimedOut
        // instead of blocking indefinitely. 0 = no deadline (wait indefinitely,
        // bounded only by the connection's keepalive or an external cancel()).
        std::chrono::milliseconds deadline{0};
        std::size_t max_inbound_message_bytes = std::numeric_limits<std::uint32_t>::max();
    };

    GrpcStream() noexcept = default;
    GrpcStream(http::Http2ClientConnection &conn, std::string_view authority, std::string_view scheme,
               std::string_view service, std::string_view method, mem::BufPool &pool, Options options);

    GrpcStream(const GrpcStream &) = delete;
    GrpcStream &operator=(const GrpcStream &) = delete;
    GrpcStream(GrpcStream &&) noexcept;
    GrpcStream &operator=(GrpcStream &&) noexcept;
    ~GrpcStream();

    // Open the HTTP/2 stream and send request HEADERS (end_stream=false). Must be
    // the first call. Sends grpc-timeout if Options::deadline > 0.
    fiber::async::Task<common::IoResult<void>> open() noexcept;

    // Encode + frame + write one request message (no END_STREAM).
    fiber::async::Task<common::IoResult<void>> write(const google::protobuf::MessageLite &request) noexcept;

    // Half-close the request side: send an empty DATA frame with END_STREAM.
    // After this, write() returns Already.
    fiber::async::Task<common::IoResult<void>> writes_done() noexcept;

    // Read one response message into `response`. Returns Message (decoded) or End
    // (response stream finished; call finish() for the gRPC status). On a
    // transport/protocol error (including TimedOut when the deadline elapses)
    // returns unexpected and fails the call.
    fiber::async::Task<common::IoResult<GrpcReadOutcome>> read(google::protobuf::MessageLite &response) noexcept;

    // Finish the call: read trailers (if any) and return the gRPC status. After
    // finish() the stream is done; further calls return Already/abort_reason_.
    // Safe to call without having read all messages (drains the body first).
    fiber::async::Task<common::IoResult<GrpcStatus>> finish() noexcept;

    // Cancel the call (RST_STREAM). Synchronous and idempotent.
    void cancel(common::IoErr reason = common::IoErr::Canceled) noexcept;

    // Apply or clear a client-side-only deadline after the stream is open. This
    // is useful for bounded handshakes on otherwise long-lived streams and does
    // not alter the grpc-timeout header already sent by open().
    void set_local_deadline(std::chrono::milliseconds timeout) noexcept;
    void clear_local_deadline() noexcept;

    [[nodiscard]] bool valid() const noexcept { return conn_ != nullptr; }

private:
    // Reads the response HEADERS (lazily, on first read()/finish()) and captures
    // any grpc-status present (trailers-only responses carry it here).
    fiber::async::Task<common::IoResult<void>> ensure_response_header() noexcept;

    // Time remaining until Options::deadline, or milliseconds::max() if no
    // deadline is set. Used as the per-call timeout for every exchange op so the
    // deadline is enforced locally without a background timer.
    [[nodiscard]] std::chrono::milliseconds remaining_timeout() const noexcept;

    // Mark the call failed and RST the stream. Idempotent.
    void fail(common::IoErr reason) noexcept;

    static constexpr std::size_t kReadChunk = 64 * 1024;

    http::Http2ClientConnection *conn_ = nullptr;
    mem::BufPool *pool_ = nullptr;
    http::ClientHttp2Exchange exchange_;
    GrpcFrameReader reader_;
    std::string authority_;
    std::string scheme_;
    std::string path_;
    std::string grpc_timeout_; // encoded grpc-timeout header value (set() copies it)

    // response state
    bool response_head_read_ = false;
    bool trailers_only_ = false; // response head carried grpc-status + END_STREAM
    bool body_ended_ = false; // body stream reached END_STREAM (trailers pending)
    bool trailers_read_ = false;
    int grpc_code_ = 0;
    std::string grpc_message_;

    // call state
    bool opened_ = false;
    bool writes_done_ = false;
    bool finished_ = false;
    bool failed_ = false;
    common::IoErr abort_reason_ = common::IoErr::None;

    // local deadline (Options::deadline). has_deadline_ = false => infinite.
    bool has_deadline_ = false;
    std::chrono::steady_clock::time_point deadline_abs_{};
};

} // namespace fiber::grpc

#endif // FIBER_GRPC_GRPC_STREAM_H
