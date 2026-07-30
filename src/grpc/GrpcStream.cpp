#include "GrpcStream.h"

#include <utility>

#include "../http/HttpHeaderHash.h"

namespace fiber::grpc {
namespace {

// Pre-hashed trailer/header names for per-response gRPC status extraction.
constexpr std::uint64_t kGrpcStatusHash = http::http_header_name_hash("grpc-status");
constexpr std::uint64_t kGrpcMessageHash = http::http_header_name_hash("grpc-message");

// Encode a duration as a gRPC timeout header value: <digits><unit>, choosing the
// largest unit (H/M/S/m) that divides the value evenly. Input is milliseconds.
std::string encode_grpc_timeout(std::chrono::milliseconds d) {
    const long long ms = d.count();
    if (ms % 3600000 == 0) {
        return std::to_string(ms / 3600000) + "H";
    }
    if (ms % 60000 == 0) {
        return std::to_string(ms / 60000) + "M";
    }
    if (ms % 1000 == 0) {
        return std::to_string(ms / 1000) + "S";
    }
    return std::to_string(ms) + "m";
}

// Bound the untrusted grpc-message allocation: a server may send a value of
// arbitrary length, so cap it before copying into the std::string member to
// keep the allocation bounded under this noexcept path. The grpc-status code is
// authoritative - the message is advisory - so truncation is safe.
constexpr std::size_t kMaxGrpcMessage = 8 * 1024;
void assign_grpc_message(std::string &dst, std::string_view src) {
    if (src.size() > kMaxGrpcMessage) {
        src = src.substr(0, kMaxGrpcMessage);
    }
    dst.assign(src.data(), src.size());
}

} // namespace

GrpcStream::GrpcStream(http::Http2ClientConnection &conn, std::string_view authority, std::string_view scheme,
                       std::string_view service, std::string_view method, mem::BufPool &pool, Options options) :
    conn_(&conn), pool_(&pool), exchange_(conn, pool), reader_(options.max_inbound_message_bytes) {
    authority_.assign(authority.data(), authority.size());
    scheme_.assign(scheme.data(), scheme.size());
    path_.reserve(service.size() + method.size() + 2);
    path_.push_back('/');
    path_.append(service.data(), service.size());
    path_.push_back('/');
    path_.append(method.data(), method.size());
    if (options.deadline.count() > 0) {
        grpc_timeout_ = encode_grpc_timeout(options.deadline);
        has_deadline_ = true;
        deadline_abs_ = conn_->loop().now() + options.deadline;
    }
}

GrpcStream::GrpcStream(GrpcStream &&other) noexcept { *this = std::move(other); }

GrpcStream &GrpcStream::operator=(GrpcStream &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (exchange_.valid() && !finished_) {
        exchange_.cancel(common::IoErr::Canceled);
    }

    conn_ = std::exchange(other.conn_, nullptr);
    pool_ = std::exchange(other.pool_, nullptr);
    exchange_ = std::move(other.exchange_);
    reader_ = std::move(other.reader_);
    authority_ = std::move(other.authority_);
    scheme_ = std::move(other.scheme_);
    path_ = std::move(other.path_);
    grpc_timeout_ = std::move(other.grpc_timeout_);

    response_head_read_ = std::exchange(other.response_head_read_, false);
    trailers_only_ = std::exchange(other.trailers_only_, false);
    body_ended_ = std::exchange(other.body_ended_, false);
    trailers_read_ = std::exchange(other.trailers_read_, false);
    grpc_code_ = std::exchange(other.grpc_code_, 0);
    grpc_message_ = std::move(other.grpc_message_);

    opened_ = std::exchange(other.opened_, false);
    writes_done_ = std::exchange(other.writes_done_, false);
    finished_ = std::exchange(other.finished_, false);
    failed_ = std::exchange(other.failed_, false);
    abort_reason_ = std::exchange(other.abort_reason_, common::IoErr::None);
    has_deadline_ = std::exchange(other.has_deadline_, false);
    deadline_abs_ = std::exchange(other.deadline_abs_, std::chrono::steady_clock::time_point{});
    return *this;
}

GrpcStream::~GrpcStream() {
    if (exchange_.valid() && !finished_) {
        exchange_.cancel(common::IoErr::Canceled); // best-effort RST if not cleanly closed
    }
}

void GrpcStream::fail(common::IoErr reason) noexcept {
    if (failed_) {
        return;
    }
    failed_ = true;
    abort_reason_ = reason;
    exchange_.cancel(reason); // RST_STREAM; wakes any blocked read/write with Canceled
}

void GrpcStream::cancel(common::IoErr reason) noexcept { fail(reason); }

void GrpcStream::set_local_deadline(std::chrono::milliseconds timeout) noexcept {
    FIBER_ASSERT(conn_ != nullptr);
    if (timeout <= std::chrono::milliseconds::zero()) {
        deadline_abs_ = conn_->loop().now();
    } else {
        deadline_abs_ = conn_->loop().now() + timeout;
    }
    has_deadline_ = true;
}

void GrpcStream::clear_local_deadline() noexcept {
    has_deadline_ = false;
    deadline_abs_ = {};
}

std::chrono::milliseconds GrpcStream::remaining_timeout() const noexcept {
    if (!has_deadline_) {
        return std::chrono::milliseconds::max();
    }
    const auto now = conn_->loop().now();
    if (now >= deadline_abs_) {
        return std::chrono::milliseconds(0);
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline_abs_ - now);
}

fiber::async::Task<common::IoResult<void>> GrpcStream::open() noexcept {
    if (!conn_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (failed_) {
        co_return std::unexpected(abort_reason_);
    }
    if (opened_) {
        co_return std::unexpected(common::IoErr::Already);
    }

    http::HttpHeaders headers(*pool_);
    headers.set("content-type", "application/grpc");
    headers.set("te", "trailers");
    headers.set("grpc-encoding", "identity");
    if (!grpc_timeout_.empty()) {
        headers.set("grpc-timeout", std::string_view(grpc_timeout_));
    }

    const http::Http2RequestHead head{
            .method = http::HttpMethod::Post,
            .scheme = std::string_view(scheme_),
            .authority = std::string_view(authority_),
            .path = std::string_view(path_),
            .headers = &headers,
    };

    auto send_result = co_await exchange_.send_request_header(head, false, remaining_timeout());
    if (!send_result) {
        fail(send_result.error());
        co_return std::unexpected(send_result.error());
    }
    opened_ = true;
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>> GrpcStream::write(const google::protobuf::MessageLite &request) noexcept {
    if (!conn_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (failed_) {
        co_return std::unexpected(abort_reason_);
    }
    if (!opened_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (writes_done_) {
        co_return std::unexpected(common::IoErr::Already);
    }

    auto payload = encode(conn_->loop().io_buf_node_pool(), request);
    if (!payload) {
        fail(payload.error());
        co_return std::unexpected(payload.error());
    }
    auto framed = frame(std::move(*payload));
    if (!framed) {
        fail(framed.error());
        co_return std::unexpected(framed.error());
    }

    auto write_result = co_await exchange_.write_all(std::move(*framed), remaining_timeout());
    if (!write_result) {
        // Any write failure (including TimedOut) fails the call: a timed-out
        // partial flush may have left a truncated frame on the wire.
        fail(write_result.error());
        co_return std::unexpected(write_result.error());
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>> GrpcStream::writes_done() noexcept {
    if (!conn_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (failed_) {
        co_return std::unexpected(abort_reason_);
    }
    if (!opened_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (writes_done_) {
        co_return std::unexpected(common::IoErr::Already);
    }

    // Empty chain marked complete -> an empty DATA frame with END_STREAM.
    mem::IoBufChain empty(conn_->loop().io_buf_node_pool());
    empty.mark_complete();
    auto write_result = co_await exchange_.write_all(std::move(empty), remaining_timeout());
    if (!write_result) {
        fail(write_result.error());
        co_return std::unexpected(write_result.error());
    }
    writes_done_ = true;
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>> GrpcStream::ensure_response_header() noexcept {
    auto header_result = co_await exchange_.read_header(remaining_timeout());
    if (!header_result) {
        co_return std::unexpected(header_result.error());
    }
    const http::Http2ResponseHead *resp = *header_result;
    if (resp->status_code != 200) {
        co_return std::unexpected(common::IoErr::Unknown);
    }
    if (auto s = resp->headers.get("grpc-status", kGrpcStatusHash); !s.empty()) {
        grpc_code_ = parse_grpc_status(s);
    }
    if (auto m = resp->headers.get("grpc-message", kGrpcMessageHash); !m.empty()) {
        assign_grpc_message(grpc_message_, m);
    }
    response_head_read_ = true;
    if (resp->end_stream) {
        trailers_only_ = true; // status carried in the response head, no body/trailer block
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<GrpcReadOutcome>>
GrpcStream::read(google::protobuf::MessageLite &response) noexcept {
    if (!conn_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (failed_) {
        co_return std::unexpected(abort_reason_);
    }
    if (finished_) {
        co_return GrpcReadOutcome::End;
    }

    if (!response_head_read_) {
        auto hr = co_await ensure_response_header();
        if (!hr) {
            fail(hr.error());
            co_return std::unexpected(hr.error());
        }
        if (trailers_only_) {
            body_ended_ = true;
            trailers_read_ = true;
            co_return GrpcReadOutcome::End;
        }
    }

    for (;;) {
        mem::IoBufChain payload;
        auto extract_result = reader_.next_payload(payload);
        if (!extract_result) {
            fail(extract_result.error());
            co_return std::unexpected(extract_result.error());
        }
        if (*extract_result) {
            auto decode_result = decode(payload, response);
            if (!decode_result) {
                fail(decode_result.error());
                co_return std::unexpected(decode_result.error());
            }
            co_return GrpcReadOutcome::Message;
        }

        if (body_ended_) {
            if (reader_.buffered_bytes() == 0) {
                co_return GrpcReadOutcome::End;
            }
            // Partial frame left after the body stream ended: truncated message.
            fail(common::IoErr::Invalid);
            co_return std::unexpected(common::IoErr::Invalid);
        }

        auto body_result = co_await exchange_.read_body(kReadChunk, remaining_timeout());
        if (!body_result) {
            fail(body_result.error());
            co_return std::unexpected(body_result.error());
        }
        const bool stream_end = body_result->complete();
        if (body_result->readable_bytes() > 0) {
            auto append_result = reader_.append(std::move(*body_result));
            if (!append_result) {
                fail(append_result.error());
                co_return std::unexpected(append_result.error());
            }
        }
        if (stream_end) {
            body_ended_ = true;
        }
    }
}

fiber::async::Task<common::IoResult<GrpcStatus>> GrpcStream::finish() noexcept {
    if (!conn_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (failed_) {
        co_return std::unexpected(abort_reason_);
    }
    if (finished_) {
        co_return GrpcStatus{grpc_code_, grpc_message_};
    }

    if (!response_head_read_) {
        auto hr = co_await ensure_response_header();
        if (!hr) {
            fail(hr.error());
            co_return std::unexpected(hr.error());
        }
    }

    if (!trailers_read_) {
        if (!trailers_only_) {
            // Drain any unread body to END_STREAM so the trailer block can follow.
            while (!body_ended_) {
                auto body_result = co_await exchange_.read_body(kReadChunk, remaining_timeout());
                if (!body_result) {
                    fail(body_result.error());
                    co_return std::unexpected(body_result.error());
                }
                if (body_result->complete()) {
                    body_ended_ = true;
                }
            }

            auto trailer_result = co_await exchange_.read_header(remaining_timeout());
            if (!trailer_result) {
                fail(trailer_result.error());
                co_return std::unexpected(trailer_result.error());
            }
            const http::Http2ResponseHead *trailer = *trailer_result;
            if (auto s = trailer->headers.get("grpc-status", kGrpcStatusHash); !s.empty()) {
                grpc_code_ = parse_grpc_status(s);
            }
            if (auto m = trailer->headers.get("grpc-message", kGrpcMessageHash); !m.empty()) {
                assign_grpc_message(grpc_message_, m);
            }
        }
        // trailers_only_ case: status was captured in ensure_response_header().
        trailers_read_ = true;
    }

    finished_ = true;
    co_return GrpcStatus{grpc_code_, grpc_message_};
}

} // namespace fiber::grpc
