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

} // namespace

GrpcStream::GrpcStream(std::shared_ptr<http::Http2ClientConnection> conn, std::string_view authority,
                       std::string_view scheme, std::string_view service, std::string_view method, mem::BufPool &pool,
                       Options options) : conn_(std::move(conn)), pool_(&pool), exchange_(*conn_, *pool_) {
    authority_.assign(authority.data(), authority.size());
    scheme_.assign(scheme.data(), scheme.size());
    path_.reserve(service.size() + method.size() + 2);
    path_.push_back('/');
    path_.append(service.data(), service.size());
    path_.push_back('/');
    path_.append(method.data(), method.size());
    if (options.deadline.count() > 0) {
        grpc_timeout_ = encode_grpc_timeout(options.deadline);
    }
}

GrpcStream::GrpcStream(GrpcStream &&) noexcept = default;
GrpcStream &GrpcStream::operator=(GrpcStream &&) noexcept = default;

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

    auto send_result = co_await exchange_.send_request_header(head, false);
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

    auto write_result = co_await exchange_.write_body(std::move(*framed));
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
    auto write_result = co_await exchange_.write_body(std::move(empty));
    if (!write_result) {
        fail(write_result.error());
        co_return std::unexpected(write_result.error());
    }
    writes_done_ = true;
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>> GrpcStream::ensure_response_header() noexcept {
    auto header_result = co_await exchange_.read_header();
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
        grpc_message_.assign(m.data(), m.size());
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
            if (hr.error() != common::IoErr::TimedOut) {
                fail(hr.error());
            }
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

        auto body_result = co_await exchange_.read_body(kReadChunk);
        if (!body_result) {
            if (body_result.error() != common::IoErr::TimedOut) {
                fail(body_result.error());
            }
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
            if (hr.error() != common::IoErr::TimedOut) {
                fail(hr.error());
            }
            co_return std::unexpected(hr.error());
        }
    }

    if (!trailers_read_) {
        if (!trailers_only_) {
            // Drain any unread body to END_STREAM so the trailer block can follow.
            while (!body_ended_) {
                auto body_result = co_await exchange_.read_body(kReadChunk);
                if (!body_result) {
                    if (body_result.error() != common::IoErr::TimedOut) {
                        fail(body_result.error());
                        co_return std::unexpected(body_result.error());
                    }
                    continue; // read timeout is retryable
                }
                if (body_result->complete()) {
                    body_ended_ = true;
                }
            }

            auto trailer_result = co_await exchange_.read_header();
            if (!trailer_result) {
                if (trailer_result.error() != common::IoErr::TimedOut) {
                    fail(trailer_result.error());
                }
                co_return std::unexpected(trailer_result.error());
            }
            const http::Http2ResponseHead *trailer = *trailer_result;
            if (auto s = trailer->headers.get("grpc-status", kGrpcStatusHash); !s.empty()) {
                grpc_code_ = parse_grpc_status(s);
            }
            if (auto m = trailer->headers.get("grpc-message", kGrpcMessageHash); !m.empty()) {
                grpc_message_.assign(m.data(), m.size());
            }
        }
        // trailers_only_ case: status was captured in ensure_response_header().
        trailers_read_ = true;
    }

    finished_ = true;
    co_return GrpcStatus{grpc_code_, grpc_message_};
}

} // namespace fiber::grpc
