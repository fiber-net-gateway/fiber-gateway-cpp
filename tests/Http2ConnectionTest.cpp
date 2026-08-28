#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Timeout.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoopGroup.h>

#define private public
#include <fiber/http/Http2Connection.h>
#undef private

#include <fiber/http/ClientHttp2Exchange.h>
#include <fiber/http/Http2HpackDecoder.h>
#include <fiber/http/Http2HpackStaticTable.h>
#include <fiber/http/Http2Stream.h>
#include <fiber/http/ServerRequestFactory.h>
#include "Http2TestSupport.h"
#include "HttpTransportStub.h"
#include "http/Http2HeadersFrameEncoder.h"
#include "http/Huffman.h"

namespace {

using fiber::async::DetachedTask;

constexpr std::string_view kClientConnectionPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

class FakeHttpTransport final : public fiber::test::HttpTransportStub {
public:
    explicit FakeHttpTransport(std::vector<std::string> chunks, std::vector<size_t> write_steps = {},
                               bool block_reads = false, bool hold_eof = false, bool report_pending_read = true) :
        chunks_(std::move(chunks)), write_steps_(std::move(write_steps)), reads_blocked_(block_reads),
        hold_eof_(hold_eof), report_pending_read_(report_pending_read) {}

    fiber::async::Task<fiber::common::IoResult<void>> handshake(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> shutdown(std::chrono::milliseconds) override {
        ++shutdown_count_;
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> wait_readable(std::chrono::milliseconds) override {
        ++wait_readable_call_count_;
        while ((reads_blocked_ || (hold_eof_ && next_chunk_ >= chunks_.size())) && !closed_) {
            co_await fiber::async::sleep(std::chrono::milliseconds(1));
        }
        co_return fiber::common::IoResult<void>{};
    }

    [[nodiscard]] bool has_pending_read() const noexcept override {
        return report_pending_read_ && !reads_blocked_ && (!hold_eof_ || next_chunk_ < chunks_.size());
    }

    fiber::common::IoErr poll_read(void *buf, size_t len, size_t &out,
                                   fiber::event::IoEvent &wait_event) noexcept override {
        out = 0;
        wait_event = fiber::event::IoEvent::None;
        if (closed_) {
            return fiber::common::IoErr::ConnReset;
        }
        if (reads_blocked_ || (hold_eof_ && next_chunk_ >= chunks_.size())) {
            wait_event = fiber::event::IoEvent::Read;
            return fiber::common::IoErr::WouldBlock;
        }
        if (next_chunk_ >= chunks_.size()) {
            return fiber::common::IoErr::None;
        }
        const std::string &chunk = chunks_[next_chunk_++];
        out = std::min(len, chunk.size());
        std::memcpy(buf, chunk.data(), out);
        return fiber::common::IoErr::None;
    }

    fiber::common::IoErr poll_read_into(fiber::mem::IoBuf &buf, size_t &out,
                                        fiber::event::IoEvent &wait_event) noexcept override {
        ++read_into_call_count_;
        fiber::common::IoErr err = poll_read(buf.writable_data(), buf.writable(), out, wait_event);
        if (err == fiber::common::IoErr::None) {
            buf.commit(out);
        }
        return err;
    }

    fiber::common::IoErr poll_write(const void *buf, size_t len, size_t &out,
                                    fiber::event::IoEvent &wait_event) noexcept override {
        out = 0;
        wait_event = fiber::event::IoEvent::None;
        if (closed_) {
            return fiber::common::IoErr::ConnReset;
        }
        out = next_write_size(len);
        const auto *ptr = static_cast<const char *>(buf);
        written_.append(ptr, ptr + out);
        ++write_call_count_;
        return fiber::common::IoErr::None;
    }

    fiber::common::IoErr poll_writev(fiber::mem::IoBufChain &buf, size_t &out,
                                     fiber::event::IoEvent &wait_event) noexcept override {
        out = 0;
        wait_event = fiber::event::IoEvent::None;
        if (closed_) {
            return fiber::common::IoErr::ConnReset;
        }
        const size_t take = next_write_size(buf.readable_bytes());
        std::array<iovec, 16> iov{};
        const int count = buf.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
        size_t remaining = take;
        for (int i = 0; i < count && remaining > 0; ++i) {
            const size_t chunk = std::min<std::size_t>(iov[i].iov_len, remaining);
            const char *ptr = static_cast<const char *>(iov[i].iov_base);
            written_.append(ptr, ptr + chunk);
            remaining -= chunk;
        }
        buf.consume_and_compact(take);
        ++write_call_count_;
        out = take;
        return fiber::common::IoErr::None;
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> read(void *buf, size_t len,
                                                             std::chrono::milliseconds) override {
        while (reads_blocked_ && !closed_) {
            co_await fiber::async::sleep(std::chrono::milliseconds(1));
        }
        while (hold_eof_ && next_chunk_ >= chunks_.size() && !closed_) {
            co_await fiber::async::sleep(std::chrono::milliseconds(1));
        }
        if (next_chunk_ >= chunks_.size()) {
            co_return static_cast<size_t>(0);
        }
        const std::string &chunk = chunks_[next_chunk_++];
        size_t take = std::min(len, chunk.size());
        std::memcpy(buf, chunk.data(), take);
        co_return take;
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> read_into(fiber::mem::IoBuf &buf,
                                                                  std::chrono::milliseconds) override {
        ++read_into_call_count_;
        while (reads_blocked_ && !closed_) {
            co_await fiber::async::sleep(std::chrono::milliseconds(1));
        }
        while (hold_eof_ && next_chunk_ >= chunks_.size() && !closed_) {
            co_await fiber::async::sleep(std::chrono::milliseconds(1));
        }
        if (next_chunk_ >= chunks_.size()) {
            co_return static_cast<size_t>(0);
        }
        const std::string &chunk = chunks_[next_chunk_++];
        size_t take = std::min(buf.writable(), chunk.size());
        std::memcpy(buf.writable_data(), chunk.data(), take);
        buf.commit(take);
        co_return take;
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> readv_into(fiber::mem::IoBufChain &,
                                                                   std::chrono::milliseconds) override {
        co_return std::unexpected(fiber::common::IoErr::NotSupported);
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> write(const void *buf, size_t len,
                                                              std::chrono::milliseconds) override {
        if (closed_) {
            co_return std::unexpected(fiber::common::IoErr::ConnReset);
        }
        size_t take = next_write_size(len);
        const auto *ptr = static_cast<const char *>(buf);
        written_.append(ptr, ptr + take);
        ++write_call_count_;
        co_return take;
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> write(fiber::mem::IoBuf &buf,
                                                              std::chrono::milliseconds timeout) override {
        auto result = co_await write(buf.readable_data(), buf.readable(), timeout);
        if (result) {
            buf.consume(*result);
        }
        co_return result;
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> writev(fiber::mem::IoBufChain &buf,
                                                               std::chrono::milliseconds) override {
        if (closed_) {
            co_return std::unexpected(fiber::common::IoErr::ConnReset);
        }

        size_t take = next_write_size(buf.readable_bytes());
        std::array<iovec, 16> iov{};
        int count = buf.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
        size_t remaining = take;
        for (int i = 0; i < count && remaining > 0; ++i) {
            size_t chunk = std::min<std::size_t>(iov[i].iov_len, remaining);
            const char *ptr = static_cast<const char *>(iov[i].iov_base);
            written_.append(ptr, ptr + chunk);
            remaining -= chunk;
        }
        buf.consume_and_compact(take);
        ++write_call_count_;
        co_return take;
    }

    void close() override {
        closed_ = true;
        ++close_count_;
    }
    void release_reads() noexcept {
        reads_blocked_ = false;
        notify_read_ready();
    }
    void release_eof() noexcept {
        hold_eof_ = false;
        notify_read_ready();
    }
    void append_read_chunk(std::string chunk) {
        chunks_.push_back(std::move(chunk));
        notify_read_ready();
    }

    [[nodiscard]] bool valid() const noexcept override { return !closed_; }
    [[nodiscard]] int fd() const noexcept override { return -1; }
    [[nodiscard]] std::string_view negotiated_alpn() const noexcept override { return "h2"; }
    [[nodiscard]] const fiber::net::SocketAddress &remote_addr() const noexcept override { return remote_addr_; }
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept override { return loop_ ? *loop_ : fallback_loop_; }
    [[nodiscard]] const std::string &written() const noexcept { return written_; }
    [[nodiscard]] std::size_t close_count() const noexcept { return close_count_; }
    [[nodiscard]] std::size_t shutdown_count() const noexcept { return shutdown_count_; }
    [[nodiscard]] std::size_t wait_readable_call_count() const noexcept { return wait_readable_call_count_; }
    [[nodiscard]] std::size_t read_into_call_count() const noexcept { return read_into_call_count_; }

private:
    size_t next_write_size(size_t available) noexcept {
        if (available == 0) {
            return 0;
        }
        if (write_call_count_ < write_steps_.size()) {
            return std::min(available, write_steps_[write_call_count_]);
        }
        return available;
    }

    std::vector<std::string> chunks_;
    std::vector<size_t> write_steps_;
    size_t next_chunk_ = 0;
    size_t write_call_count_ = 0;
    bool closed_ = false;
    bool reads_blocked_ = false;
    bool hold_eof_ = false;
    bool report_pending_read_ = true;
    std::size_t close_count_ = 0;
    std::size_t shutdown_count_ = 0;
    std::size_t wait_readable_call_count_ = 0;
    std::size_t read_into_call_count_ = 0;
    std::string written_;
    fiber::net::SocketAddress remote_addr_{};
    fiber::event::EventLoop *loop_ = fiber::event::EventLoop::current_or_null();
    mutable fiber::event::EventLoop fallback_loop_{};
};

class ScriptedReadTransport final : public fiber::test::HttpTransportStub {
public:
    enum class ReadActionKind : std::uint8_t {
        Chunk,
        TimedOut,
        Eof,
    };

    struct ReadAction {
        ReadActionKind kind = ReadActionKind::Eof;
        std::string data{};
    };

    explicit ScriptedReadTransport(std::vector<ReadAction> actions) : actions_(std::move(actions)) {}

    fiber::async::Task<fiber::common::IoResult<void>> handshake(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> shutdown(std::chrono::milliseconds) override {
        ++shutdown_count_;
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> wait_readable(std::chrono::milliseconds) override {
        ++wait_readable_call_count_;
        if (closed_) {
            co_return std::unexpected(fiber::common::IoErr::Canceled);
        }
        if (next_action_ < actions_.size() && actions_[next_action_].kind == ReadActionKind::TimedOut) {
            ++next_action_;
            co_return std::unexpected(fiber::common::IoErr::TimedOut);
        }
        co_return fiber::common::IoResult<void>{};
    }

    [[nodiscard]] bool has_pending_read() const noexcept override { return next_action_ < actions_.size(); }

    fiber::common::IoErr poll_read(void *buf, size_t len, size_t &out,
                                   fiber::event::IoEvent &wait_event) noexcept override {
        out = 0;
        wait_event = fiber::event::IoEvent::None;
        if (closed_) {
            return fiber::common::IoErr::ConnReset;
        }
        if (next_action_ >= actions_.size()) {
            return fiber::common::IoErr::None;
        }

        ReadAction &action = actions_[next_action_++];
        switch (action.kind) {
            case ReadActionKind::Chunk:
                out = std::min(len, action.data.size());
                std::memcpy(buf, action.data.data(), out);
                return fiber::common::IoErr::None;
            case ReadActionKind::TimedOut:
                wait_event = fiber::event::IoEvent::Read;
                return fiber::common::IoErr::WouldBlock;
            case ReadActionKind::Eof:
                return fiber::common::IoErr::None;
        }
        return fiber::common::IoErr::Invalid;
    }

    fiber::common::IoErr poll_read_into(fiber::mem::IoBuf &buf, size_t &out,
                                        fiber::event::IoEvent &wait_event) noexcept override {
        ++read_into_call_count_;
        fiber::common::IoErr err = poll_read(buf.writable_data(), buf.writable(), out, wait_event);
        if (err == fiber::common::IoErr::None) {
            buf.commit(out);
        }
        return err;
    }

    fiber::common::IoErr poll_write(const void *buf, size_t len, size_t &out,
                                    fiber::event::IoEvent &wait_event) noexcept override {
        out = 0;
        wait_event = fiber::event::IoEvent::None;
        if (closed_) {
            return fiber::common::IoErr::ConnReset;
        }
        const auto *ptr = static_cast<const char *>(buf);
        written_.append(ptr, ptr + len);
        out = len;
        return fiber::common::IoErr::None;
    }

    fiber::common::IoErr poll_writev(fiber::mem::IoBufChain &buf, size_t &out,
                                     fiber::event::IoEvent &wait_event) noexcept override {
        out = 0;
        wait_event = fiber::event::IoEvent::None;
        if (closed_) {
            return fiber::common::IoErr::ConnReset;
        }
        std::array<iovec, 16> iov{};
        const int count = buf.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
        for (int i = 0; i < count; ++i) {
            const char *ptr = static_cast<const char *>(iov[i].iov_base);
            written_.append(ptr, ptr + iov[i].iov_len);
            out += iov[i].iov_len;
        }
        buf.consume_and_compact(out);
        return fiber::common::IoErr::None;
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> read(void *buf, size_t len,
                                                             std::chrono::milliseconds) override {
        if (closed_) {
            co_return std::unexpected(fiber::common::IoErr::ConnReset);
        }
        if (next_action_ >= actions_.size()) {
            co_return static_cast<size_t>(0);
        }

        ReadAction &action = actions_[next_action_++];
        switch (action.kind) {
            case ReadActionKind::Chunk: {
                const size_t take = std::min(len, action.data.size());
                std::memcpy(buf, action.data.data(), take);
                co_return take;
            }
            case ReadActionKind::TimedOut:
                co_return std::unexpected(fiber::common::IoErr::TimedOut);
            case ReadActionKind::Eof:
                co_return static_cast<size_t>(0);
        }

        co_return std::unexpected(fiber::common::IoErr::Invalid);
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> read_into(fiber::mem::IoBuf &buf,
                                                                  std::chrono::milliseconds timeout) override {
        ++read_into_call_count_;
        auto result = co_await read(buf.writable_data(), buf.writable(), timeout);
        if (result) {
            buf.commit(*result);
        }
        co_return result;
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> readv_into(fiber::mem::IoBufChain &,
                                                                   std::chrono::milliseconds) override {
        co_return std::unexpected(fiber::common::IoErr::NotSupported);
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> write(const void *buf, size_t len,
                                                              std::chrono::milliseconds) override {
        if (closed_) {
            co_return std::unexpected(fiber::common::IoErr::ConnReset);
        }
        const auto *ptr = static_cast<const char *>(buf);
        written_.append(ptr, ptr + len);
        co_return len;
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> write(fiber::mem::IoBuf &buf,
                                                              std::chrono::milliseconds timeout) override {
        auto result = co_await write(buf.readable_data(), buf.readable(), timeout);
        if (result) {
            buf.consume(*result);
        }
        co_return result;
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> writev(fiber::mem::IoBufChain &buf,
                                                               std::chrono::milliseconds) override {
        if (closed_) {
            co_return std::unexpected(fiber::common::IoErr::ConnReset);
        }

        std::array<iovec, 16> iov{};
        const int count = buf.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
        size_t total = 0;
        for (int i = 0; i < count; ++i) {
            const char *ptr = static_cast<const char *>(iov[i].iov_base);
            written_.append(ptr, ptr + iov[i].iov_len);
            total += iov[i].iov_len;
        }
        buf.consume_and_compact(total);
        co_return total;
    }

    void close() override {
        closed_ = true;
        ++close_count_;
    }

    [[nodiscard]] bool valid() const noexcept override { return !closed_; }
    [[nodiscard]] int fd() const noexcept override { return -1; }
    [[nodiscard]] std::string_view negotiated_alpn() const noexcept override { return "h2"; }
    [[nodiscard]] const fiber::net::SocketAddress &remote_addr() const noexcept override { return remote_addr_; }
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept override { return loop_ ? *loop_ : fallback_loop_; }
    [[nodiscard]] const std::string &written() const noexcept { return written_; }
    [[nodiscard]] std::size_t close_count() const noexcept { return close_count_; }
    [[nodiscard]] std::size_t wait_readable_call_count() const noexcept { return wait_readable_call_count_; }
    [[nodiscard]] std::size_t read_into_call_count() const noexcept { return read_into_call_count_; }

private:
    std::vector<ReadAction> actions_;
    size_t next_action_ = 0;
    bool closed_ = false;
    std::size_t close_count_ = 0;
    std::size_t shutdown_count_ = 0;
    std::size_t wait_readable_call_count_ = 0;
    std::size_t read_into_call_count_ = 0;
    std::string written_;
    fiber::net::SocketAddress remote_addr_{};
    fiber::event::EventLoop *loop_ = fiber::event::EventLoop::current_or_null();
    mutable fiber::event::EventLoop fallback_loop_{};
};

struct ObservedChunk {
    fiber::http::Http2Connection::FrameHeader header{};
    std::size_t offset = 0;
    std::size_t length = 0;
    fiber::mem::IoBuf payload{};
};

class RecordingHttp2Connection final : public fiber::http::Http2Connection {
public:
    RecordingHttp2Connection(std::unique_ptr<fiber::http::HttpTransport> transport, Options options) :
        fiber::http::Http2Connection(options, &test_http2_stream_factory(), TestHttp2StreamFactory::ops()) {
        FIBER_ASSERT(start(std::move(transport)) == fiber::common::IoErr::None);
    }

    const std::vector<ObservedChunk> &chunks() const noexcept { return chunks_; }
    fiber::async::Task<void> stop_and_join() noexcept { co_await stop_and_wait_closed(); }

    void set_payload_error(fiber::common::IoErr err) noexcept { payload_error_ = err; }

protected:
    fiber::common::IoErr on_frame_payload(const FrameHeader &fhr, const fiber::mem::IoBuf &buf, std::size_t offset,
                                          std::size_t length) noexcept override {
        ObservedChunk chunk;
        chunk.header = fhr;
        chunk.offset = offset;
        chunk.length = length;
        if (length != 0) {
            chunk.payload = buf.retain_slice(0, length);
            if (!chunk.payload) {
                return fiber::common::IoErr::NoMem;
            }
        }
        chunks_.push_back(std::move(chunk));
        return payload_error_;
    }

private:
    std::vector<ObservedChunk> chunks_;
    fiber::common::IoErr payload_error_ = fiber::common::IoErr::None;
};

struct RunOutcome {
    fiber::common::IoResult<void> result;
    std::vector<ObservedChunk> chunks;
    std::size_t wait_readable_call_count = 0;
    std::size_t read_into_call_count = 0;
};

struct SendOutcome {
    fiber::common::IoErr submit_error = fiber::common::IoErr::None;
    std::string written;
};

struct ServerHeaderRunOutcome {
    fiber::common::IoResult<void> result;
    fiber::common::IoResult<void> header_result{};
    std::string written;
};

struct ServerDelayedSendAfterCloseOutcome {
    fiber::common::IoResult<void> run_result;
    fiber::common::IoResult<void> delayed_send_result;
    bool delayed_send_completed = false;
};

struct ClientRequestHeaderRunOutcome {
    fiber::common::IoResult<void> result;
    std::string written;
    std::uint32_t stream_id = 0;
};

struct ClientAbortRunOutcome {
    fiber::common::IoResult<void> header_result;
    fiber::common::IoResult<void> abort_result;
    std::string written;
    std::uint32_t stream_id = 0;
    bool local_rst = false;
};

struct ClientExtendedConnectRunOutcome {
    fiber::common::IoResult<void> header_result;
    fiber::common::IoResult<void> run_result;
    fiber::http::Http2ExtendedConnectSupport support_before = fiber::http::Http2ExtendedConnectSupport::Unknown;
    fiber::http::Http2ExtendedConnectSupport support_after = fiber::http::Http2ExtendedConnectSupport::Unknown;
    std::string written;
    std::uint32_t stream_id = 0;
};

struct ClientRequestBodyRunOutcome {
    fiber::common::IoResult<void> header_result;
    fiber::common::IoResult<std::size_t> body_result;
    std::string written;
    std::uint32_t stream_id = 0;
    std::int32_t conn_send_window = 0;
};

struct ClientBodyCancelRunOutcome {
    fiber::common::IoResult<void> header_result;
    fiber::common::IoResult<std::size_t> canceled_body_result;
    fiber::common::IoResult<std::size_t> retried_body_result;
    std::int32_t conn_window_after_cancel = 0;
    std::int32_t stream_window_after_cancel = 0;
    std::string written;
};

struct ClientPartialBodyRunOutcome {
    fiber::common::IoResult<void> header_result;
    fiber::common::IoResult<std::size_t> first_body_result;
    fiber::common::IoResult<std::size_t> second_body_result;
    std::string written;
};

struct ClientConnWindowWaitRunOutcome {
    fiber::common::IoResult<void> header_result;
    fiber::common::IoResult<std::size_t> body_result;
    std::int32_t conn_send_window = 0;
    std::int32_t stream_send_window = 0;
    std::string written;
};

struct ClientRequestTrailerRunOutcome {
    fiber::common::IoResult<void> header_result;
    fiber::common::IoResult<std::size_t> body_result;
    fiber::common::IoResult<void> trailer_result;
    std::string written;
    std::uint32_t stream_id = 0;
};

struct ClientResponseBodyRunOutcome {
    fiber::common::IoResult<void> header_result;
    fiber::common::IoResult<void> run_result;
    fiber::common::IoResult<fiber::mem::IoBufChain> body_result;
    std::string written;
    std::uint32_t stream_id = 0;
};

struct ResponseHeadSnapshot {
    bool present = false;
    fiber::http::OutgoingHeaderKind kind = fiber::http::OutgoingHeaderKind::Final;
    int status_code = 0;
    bool end_stream = false;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct ClientResponseHeaderRunOutcome {
    fiber::common::IoResult<void> header_result;
    fiber::common::IoResult<void> run_result;
    fiber::common::IoResult<const fiber::http::Http2ResponseHead *> informational_result;
    fiber::common::IoResult<const fiber::http::Http2ResponseHead *> final_result;
    fiber::common::IoResult<fiber::mem::IoBufChain> body_result;
    fiber::common::IoResult<const fiber::http::Http2ResponseHead *> trailer_result;
    fiber::common::IoResult<const fiber::http::Http2ResponseHead *> end_result;
    ResponseHeadSnapshot informational;
    ResponseHeadSnapshot final;
    ResponseHeadSnapshot trailer;
    std::string written;
    std::uint32_t stream_id = 0;
};

struct ClientResponseAbortRunOutcome {
    fiber::common::IoResult<void> header_result;
    fiber::common::IoResult<void> run_result;
    fiber::common::IoResult<const fiber::http::Http2ResponseHead *> read_header_result;
    fiber::common::IoResult<fiber::mem::IoBufChain> read_body_result;
    std::string written;
    std::uint32_t stream_id = 0;
};

struct ClientGoawayRunOutcome {
    fiber::common::IoResult<void> send_result;
    fiber::common::IoResult<void> run_result;
    fiber::http::Http2Connection::State state = fiber::http::Http2Connection::State::Init;
    std::string written;
};

DetachedTask run_http2_connection_capture_task(fiber::http::Http2Connection *connection,
                                               std::shared_ptr<fiber::common::IoResult<void>> result,
                                               std::shared_ptr<std::atomic_bool> done) {
    if (connection) {
        *result = co_await connection->wait_closed();
    }
    if (done) {
        done->store(true, std::memory_order_release);
    }
    co_return;
}

struct ControlRunOutcome {
    fiber::common::IoResult<void> result;
    std::string written;
    fiber::http::Http2Connection::State state = fiber::http::Http2Connection::State::Init;
    std::size_t transport_close_count = 0;
    std::int32_t conn_send_window = 0;
    std::uint32_t peer_max_frame_size = 0;
    std::uint32_t peer_max_concurrent_streams = 0;
    bool peer_enable_push = true;
    std::int32_t stream1_send_window = 0;
    std::int32_t stream3_send_window = 0;
    bool stream1_registered = false;
    bool stream1_remote_end_stream = false;
    bool stream1_remote_rst = false;
    bool stream2_remote_end_stream = false;
    bool stream2_registered = false;
    bool stream3_registered = false;
};

struct RetainedStreamOutcome {
    fiber::common::IoResult<void> result;
    bool stream_opened = false;
    bool stream_registered_after_run = false;
    bool lease_valid_after_run = false;
    bool attached_after_run = true;
    fiber::common::IoErr close_reason = fiber::common::IoErr::None;
};

struct KeepaliveRunOutcome {
    fiber::common::IoResult<void> result;
    std::string written;
    fiber::http::Http2Connection::State state = fiber::http::Http2Connection::State::Init;
    std::size_t transport_close_count = 0;
    std::size_t wait_readable_call_count = 0;
    std::size_t read_into_call_count = 0;
    bool read_buffer_released = false;
};

std::string iobuf_to_string(const fiber::mem::IoBuf &buf) {
    return std::string(reinterpret_cast<const char *>(buf.readable_data()), buf.readable());
}

ResponseHeadSnapshot snapshot_response_head(const fiber::http::Http2ResponseHead *head) {
    ResponseHeadSnapshot snapshot;
    if (!head) {
        return snapshot;
    }
    snapshot.present = true;
    snapshot.kind = head->kind;
    snapshot.status_code = head->status_code;
    snapshot.end_stream = head->end_stream;
    for (auto it = head->headers.begin(); it != head->headers.end(); ++it) {
        snapshot.headers.emplace_back(std::string(it->name_view()), std::string(it->value_view()));
    }
    return snapshot;
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

fiber::common::IoErr noop_test_header_block_start(void *, fiber::http::Http2HpackDecoder::Sink &) noexcept {
    return fiber::common::IoErr::None;
}

fiber::common::IoErr noop_test_header_block_complete(void *, bool) noexcept { return fiber::common::IoErr::None; }

fiber::common::IoErr noop_test_body(void *, fiber::mem::IoBuf &&, bool) noexcept { return fiber::common::IoErr::None; }

void noop_test_abort(void *, fiber::common::IoErr) noexcept {}
void noop_test_destroy(void *) noexcept {}

const fiber::http::Http2Stream::Ops kHeaderEncodeStreamOps{
        &noop_test_destroy, &noop_test_header_block_start, &noop_test_header_block_complete, &noop_test_body,
        &noop_test_abort,
};

struct HeaderEncodeCase final {
    std::uint32_t stream_id = 0;
    bool end_stream = false;
    std::vector<std::pair<std::string_view, std::string_view>> headers;

    fiber::common::IoErr on_encode(fiber::http::Http2Stream &stream, const fiber::http::Http2OutboundEncodeRequest &req,
                                   fiber::http::Http2OutboundEncodeTarget &target,
                                   fiber::http::Http2OutboundEncodeResult &result) noexcept;
};

fiber::common::IoErr HeaderEncodeCase::on_encode(fiber::http::Http2Stream &,
                                                 const fiber::http::Http2OutboundEncodeRequest &req,
                                                 fiber::http::Http2OutboundEncodeTarget &target,
                                                 fiber::http::Http2OutboundEncodeResult &result) noexcept {
    fiber::http::Http2HeadersFrameEncoder frame_encoder({
            .stream_id = stream_id,
            .max_frame_size = req.max_frame_size,
            .first_frame_payload_cap = 1024,
            .end_stream = end_stream,
            .hpack = {.huffman_threshold = 1024},
    });
    fiber::common::IoErr err = frame_encoder.begin(target);
    if (err != fiber::common::IoErr::None) {
        return err;
    }
    for (const auto &[name, value]: headers) {
        err = frame_encoder.encode_field(name, fiber::http::http_header_name_hash(name), value);
        if (err != fiber::common::IoErr::None) {
            frame_encoder.abort();
            return err;
        }
    }
    err = frame_encoder.finish();
    if (err != fiber::common::IoErr::None) {
        return err;
    }

    result.flow_controlled_bytes = 0;
    result.operation_final_batch = true;
    return fiber::common::IoErr::None;
}

std::string build_headers_frame_bytes(std::uint32_t stream_id,
                                      std::initializer_list<std::pair<std::string_view, std::string_view>> headers,
                                      bool end_stream = false) {
    HeaderEncodeCase test_case;
    test_case.stream_id = stream_id;
    test_case.end_stream = end_stream;
    test_case.headers.assign(headers.begin(), headers.end());
    fiber::mem::IoBufNodePool node_pool;
    fiber::http::Http2OutboundEncodeTarget target(node_pool);
    fiber::http::Http2Stream stream(&test_case, kHeaderEncodeStreamOps);
    fiber::http::Http2OutboundEncodeRequest request{.max_frame_size = 16384};
    fiber::http::Http2OutboundEncodeResult encode_result;
    if (test_case.on_encode(stream, request, target, encode_result) != fiber::common::IoErr::None) {
        return {};
    }
    std::vector<std::uint8_t> bytes = chain_to_bytes(target.take_chain());
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

struct DecodedHeaderBlock {
    std::vector<std::pair<std::string, std::string>> fields;
    std::string pending_name;
    std::uint64_t pending_name_hash = 0;

    static fiber::common::IoErr on_indexed_field(void *ctx,
                                                 fiber::http::Http2HpackDecoder::TableEntryView entry) noexcept {
        auto *self = static_cast<DecodedHeaderBlock *>(ctx);
        self->fields.emplace_back(std::string(entry.name), std::string(entry.value));
        return fiber::common::IoErr::None;
    }

    static fiber::common::IoErr on_indexed_name(void *ctx,
                                                fiber::http::Http2HpackDecoder::TableEntryView entry) noexcept {
        auto *self = static_cast<DecodedHeaderBlock *>(ctx);
        self->pending_name.assign(entry.name.data(), entry.name.size());
        self->pending_name_hash = entry.name_hash;
        return fiber::common::IoErr::None;
    }

    static fiber::common::IoErr on_name_raw(void *ctx, const std::uint8_t *data, std::size_t len) noexcept {
        auto *self = static_cast<DecodedHeaderBlock *>(ctx);
        self->pending_name.assign(reinterpret_cast<const char *>(data), len);
        self->pending_name_hash = fiber::http::http_header_name_hash(self->pending_name);
        return fiber::common::IoErr::None;
    }

    static fiber::common::IoErr on_name_huffman(void *ctx, const std::uint8_t *data, std::size_t len) noexcept {
        auto *self = static_cast<DecodedHeaderBlock *>(ctx);
        bool ok = false;
        const std::size_t decoded_len = fiber::http::hpack_huffman_decoded_length(data, len, &ok);
        if (!ok) {
            return fiber::common::IoErr::Invalid;
        }
        self->pending_name.assign(decoded_len, '\0');
        fiber::http::HpackHuffmanDecodeState state;
        auto result = fiber::http::hpack_huffman_decode(
                state, data, len, reinterpret_cast<std::uint8_t *>(self->pending_name.data()), decoded_len, true);
        if (result.code != fiber::http::HpackHuffmanCode::Ok || result.written != decoded_len) {
            return fiber::common::IoErr::Invalid;
        }
        self->pending_name_hash = fiber::http::http_header_name_hash(self->pending_name);
        return fiber::common::IoErr::None;
    }

    static fiber::common::IoErr on_value_raw(void *ctx, const std::uint8_t *data, std::size_t len,
                                             fiber::http::Http2HpackDecoder::FieldView *out) noexcept {
        auto *self = static_cast<DecodedHeaderBlock *>(ctx);
        std::string value(reinterpret_cast<const char *>(data), len);
        self->fields.emplace_back(self->pending_name, std::move(value));
        if (out) {
            out->name = self->fields.back().first;
            out->name_hash = self->pending_name_hash;
            out->value = self->fields.back().second;
        }
        self->pending_name.clear();
        self->pending_name_hash = 0;
        return fiber::common::IoErr::None;
    }

    static fiber::common::IoErr on_value_huffman(void *ctx, const std::uint8_t *data, std::size_t len,
                                                 fiber::http::Http2HpackDecoder::FieldView *out) noexcept {
        auto *self = static_cast<DecodedHeaderBlock *>(ctx);
        bool ok = false;
        const std::size_t decoded_len = fiber::http::hpack_huffman_decoded_length(data, len, &ok);
        if (!ok) {
            return fiber::common::IoErr::Invalid;
        }
        std::string value(decoded_len, '\0');
        fiber::http::HpackHuffmanDecodeState state;
        auto result = fiber::http::hpack_huffman_decode(
                state, data, len, reinterpret_cast<std::uint8_t *>(value.data()), decoded_len, true);
        if (result.code != fiber::http::HpackHuffmanCode::Ok || result.written != decoded_len) {
            return fiber::common::IoErr::Invalid;
        }
        self->fields.emplace_back(self->pending_name, std::move(value));
        if (out) {
            out->name = self->fields.back().first;
            out->name_hash = self->pending_name_hash;
            out->value = self->fields.back().second;
        }
        self->pending_name.clear();
        self->pending_name_hash = 0;
        return fiber::common::IoErr::None;
    }

    static const fiber::http::Http2HpackDecoder::Ops &ops() noexcept {
        static const fiber::http::Http2HpackDecoder::Ops kOps{
                &DecodedHeaderBlock::on_indexed_field, &DecodedHeaderBlock::on_indexed_name,
                &DecodedHeaderBlock::on_name_raw,      &DecodedHeaderBlock::on_name_huffman,
                &DecodedHeaderBlock::on_value_raw,     &DecodedHeaderBlock::on_value_huffman,
        };
        return kOps;
    }
};

struct ParsedHeadersFrames {
    std::uint8_t first_flags = 0;
    std::vector<std::uint8_t> header_block;
};

std::vector<ParsedHeadersFrames> collect_stream_header_blocks(std::string_view bytes, std::uint32_t stream_id) {
    std::vector<ParsedHeadersFrames> blocks;
    ParsedHeadersFrames current;
    bool collecting = false;
    for (std::size_t pos = 0; pos + 9 <= bytes.size();) {
        const auto *frame = reinterpret_cast<const std::uint8_t *>(bytes.data() + pos);
        const std::uint32_t length = (static_cast<std::uint32_t>(frame[0]) << 16) |
                                     (static_cast<std::uint32_t>(frame[1]) << 8) | static_cast<std::uint32_t>(frame[2]);
        if (pos + 9 + length > bytes.size()) {
            break;
        }
        const std::uint8_t type = frame[3];
        const std::uint8_t flags = frame[4];
        const std::uint32_t frame_stream_id =
                ((static_cast<std::uint32_t>(frame[5]) & 0x7fU) << 24) | (static_cast<std::uint32_t>(frame[6]) << 16) |
                (static_cast<std::uint32_t>(frame[7]) << 8) | static_cast<std::uint32_t>(frame[8]);
        if (frame_stream_id == stream_id &&
            (type == static_cast<std::uint8_t>(fiber::http::Http2FrameType::Headers) ||
             type == static_cast<std::uint8_t>(fiber::http::Http2FrameType::Continuation))) {
            if (!collecting && type == static_cast<std::uint8_t>(fiber::http::Http2FrameType::Headers)) {
                current = {};
                current.first_flags = flags;
                collecting = true;
            }
            if (collecting) {
                const auto *payload = frame + 9;
                current.header_block.insert(current.header_block.end(), payload, payload + length);
                if ((flags & 0x4U) != 0) {
                    blocks.push_back(std::move(current));
                    current = {};
                    collecting = false;
                }
            }
        }
        pos += 9 + length;
    }
    return blocks;
}

ParsedHeadersFrames collect_stream_headers_frames(std::string_view bytes, std::uint32_t stream_id) {
    ParsedHeadersFrames parsed;
    const auto blocks = collect_stream_header_blocks(bytes, stream_id);
    if (!blocks.empty()) {
        parsed = blocks.front();
    }
    return parsed;
}

std::vector<std::pair<std::string, std::string>> decode_header_block(const std::vector<std::uint8_t> &header_block) {
    fiber::http::Http2HpackDecoder decoder;
    EXPECT_TRUE(decoder.init(4096, 64 * 1024));

    DecodedHeaderBlock decoded;
    decoder.begin_block(&decoded, &DecodedHeaderBlock::ops());
    EXPECT_EQ(decoder.decode(header_block.data(), header_block.size(), true), fiber::common::IoErr::None);
    return decoded.fields;
}

std::string make_frame(std::uint32_t length, std::uint8_t type, std::uint8_t flags, std::uint32_t stream_id,
                       std::string_view payload) {
    EXPECT_EQ(length, payload.size());

    std::string out;
    out.resize(9 + payload.size());
    out[0] = static_cast<char>((length >> 16) & 0xffU);
    out[1] = static_cast<char>((length >> 8) & 0xffU);
    out[2] = static_cast<char>(length & 0xffU);
    out[3] = static_cast<char>(type);
    out[4] = static_cast<char>(flags);
    out[5] = static_cast<char>((stream_id >> 24) & 0x7fU);
    out[6] = static_cast<char>((stream_id >> 16) & 0xffU);
    out[7] = static_cast<char>((stream_id >> 8) & 0xffU);
    out[8] = static_cast<char>(stream_id & 0xffU);
    if (!payload.empty()) {
        std::memcpy(out.data() + 9, payload.data(), payload.size());
    }
    return out;
}

struct EncodedFrame {
    std::uint32_t length = 0;
    std::uint8_t type = 0;
    std::uint8_t flags = 0;
    std::uint32_t stream_id = 0;
    std::string payload;
};

std::vector<EncodedFrame> parse_frames(std::string_view data) {
    std::vector<EncodedFrame> frames;
    std::size_t pos = 0;
    while (pos + 9 <= data.size()) {
        EncodedFrame frame;
        frame.length = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos])) << 16) |
                       (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + 1])) << 8) |
                       static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + 2]));
        frame.type = static_cast<std::uint8_t>(data[pos + 3]);
        frame.flags = static_cast<std::uint8_t>(data[pos + 4]);
        frame.stream_id = ((static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + 5])) & 0x7fU) << 24) |
                          (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + 6])) << 16) |
                          (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + 7])) << 8) |
                          static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + 8]));
        pos += 9;
        if (pos + frame.length > data.size()) {
            break;
        }
        frame.payload.assign(data.data() + pos, frame.length);
        pos += frame.length;
        frames.push_back(std::move(frame));
    }
    return frames;
}

std::string describe_frames(const std::vector<EncodedFrame> &frames) {
    std::ostringstream out;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (i != 0) {
            out << " | ";
        }
        out << "#" << i << " type=" << static_cast<int>(frames[i].type) << " sid=" << frames[i].stream_id
            << " len=" << frames[i].length << " payload=" << frames[i].payload;
    }
    return out.str();
}

std::uint32_t parse_window_update_increment(const EncodedFrame &frame) {
    EXPECT_EQ(frame.type, static_cast<std::uint8_t>(fiber::http::Http2FrameType::WindowUpdate));
    EXPECT_EQ(frame.payload.size(), 4U);
    return ((static_cast<std::uint32_t>(static_cast<std::uint8_t>(frame.payload[0])) & 0x7fU) << 24) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(frame.payload[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(frame.payload[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(frame.payload[3]));
}

std::optional<std::uint32_t> parse_settings_parameter(const EncodedFrame &frame, std::uint16_t id) {
    if (frame.type != static_cast<std::uint8_t>(fiber::http::Http2FrameType::Settings) ||
        (frame.payload.size() % 6U) != 0) {
        return std::nullopt;
    }

    for (std::size_t pos = 0; pos < frame.payload.size(); pos += 6U) {
        std::uint16_t current_id = (static_cast<std::uint16_t>(static_cast<std::uint8_t>(frame.payload[pos])) << 8) |
                                   static_cast<std::uint16_t>(static_cast<std::uint8_t>(frame.payload[pos + 1]));
        if (current_id != id) {
            continue;
        }
        return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(frame.payload[pos + 2])) << 24) |
               (static_cast<std::uint32_t>(static_cast<std::uint8_t>(frame.payload[pos + 3])) << 16) |
               (static_cast<std::uint32_t>(static_cast<std::uint8_t>(frame.payload[pos + 4])) << 8) |
               static_cast<std::uint32_t>(static_cast<std::uint8_t>(frame.payload[pos + 5]));
    }

    return std::nullopt;
}

std::size_t client_initial_flight_prefix_length(std::string_view written) {
    if (written.size() < kClientConnectionPreface.size() ||
        written.substr(0, kClientConnectionPreface.size()) != kClientConnectionPreface) {
        return 0;
    }

    std::size_t pos = kClientConnectionPreface.size();
    if (pos + 9 > written.size()) {
        return 0;
    }

    std::uint32_t first_len = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(written[pos])) << 16) |
                              (static_cast<std::uint32_t>(static_cast<std::uint8_t>(written[pos + 1])) << 8) |
                              static_cast<std::uint32_t>(static_cast<std::uint8_t>(written[pos + 2]));
    std::uint8_t first_type = static_cast<std::uint8_t>(written[pos + 3]);
    std::uint32_t first_stream_id =
            ((static_cast<std::uint32_t>(static_cast<std::uint8_t>(written[pos + 5])) & 0x7fU) << 24) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(written[pos + 6])) << 16) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(written[pos + 7])) << 8) |
            static_cast<std::uint32_t>(static_cast<std::uint8_t>(written[pos + 8]));
    if (first_type != 0x4 || first_stream_id != 0 || pos + 9 + first_len > written.size()) {
        return 0;
    }
    pos += 9 + first_len;

    if (pos + 13 <= written.size()) {
        std::uint32_t second_len = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(written[pos])) << 16) |
                                   (static_cast<std::uint32_t>(static_cast<std::uint8_t>(written[pos + 1])) << 8) |
                                   static_cast<std::uint32_t>(static_cast<std::uint8_t>(written[pos + 2]));
        std::uint8_t second_type = static_cast<std::uint8_t>(written[pos + 3]);
        std::uint32_t second_stream_id =
                ((static_cast<std::uint32_t>(static_cast<std::uint8_t>(written[pos + 5])) & 0x7fU) << 24) |
                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(written[pos + 6])) << 16) |
                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(written[pos + 7])) << 8) |
                static_cast<std::uint32_t>(static_cast<std::uint8_t>(written[pos + 8]));
        if (second_len == 4 && second_type == 0x8 && second_stream_id == 0 && pos + 13 <= written.size()) {
            pos += 13;
        }
    }

    return pos;
}

std::string strip_client_initial_flight(std::string written) {
    std::size_t prefix = client_initial_flight_prefix_length(written);
    if (prefix == 0) {
        return written;
    }
    return written.substr(prefix);
}

DetachedTask run_http2_connection(std::shared_ptr<std::promise<RunOutcome>> promise, std::vector<std::string> chunks,
                                  fiber::http::Http2Connection::Options options,
                                  fiber::common::IoErr payload_error = fiber::common::IoErr::None) {
    auto transport = std::make_unique<FakeHttpTransport>(std::move(chunks));
    auto *transport_impl = transport.get();
    RecordingHttp2Connection connection(std::move(transport), options);
    connection.set_payload_error(payload_error);

    RunOutcome outcome;
    outcome.result = co_await connection.wait_closed();
    outcome.chunks = connection.chunks();
    outcome.wait_readable_call_count = transport_impl->wait_readable_call_count();
    outcome.read_into_call_count = transport_impl->read_into_call_count();
    co_await connection.stop_and_join();
    promise->set_value(std::move(outcome));

    fiber::event::EventLoop::current().stop();
    co_return;
}

RunOutcome execute_connection(std::vector<std::string> chunks, fiber::http::Http2Connection::Options options = {},
                              fiber::common::IoErr payload_error = fiber::common::IoErr::None) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<RunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0),
                        [promise = std::move(promise), chunks = std::move(chunks), options, payload_error]() mutable {
                            return run_http2_connection(std::move(promise), std::move(chunks), options, payload_error);
                        });

    auto status = future.wait_for(std::chrono::seconds(2));
    if (status != std::future_status::ready) {
        group.stop();
        group.join();
        ADD_FAILURE() << "Timed out waiting for http2 connection task";
        return {};
    }

    RunOutcome outcome = future.get();
    group.join();
    return outcome;
}

DetachedTask run_http2_server_request(std::shared_ptr<std::promise<ServerHeaderRunOutcome>> promise,
                                      std::vector<std::string> chunks, fiber::http::HttpHandler handler,
                                      fiber::http::HttpServerOptions http_options,
                                      fiber::http::Http2Connection::Options options, bool hold_eof = true) {
    auto transport = std::make_unique<FakeHttpTransport>(std::move(chunks), std::vector<size_t>{}, false, hold_eof);
    FakeHttpTransport *fake_transport = transport.get();
    fiber::http::HttpHandler wrapped_handler =
            [handler = std::move(handler), fake_transport,
             hold_eof](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        co_await handler(exchange);
        if (hold_eof) {
            fake_transport->release_eof();
        }
        co_return;
    };
    fiber::http::ServerRequestFactory factory(http_options, wrapped_handler);
    fiber::http::Http2Connection connection(options, &factory, fiber::http::ServerRequestFactory::ops());
    ServerHeaderRunOutcome outcome;
    fiber::common::IoErr start_err = connection.start(std::move(transport));
    if (start_err != fiber::common::IoErr::None) {
        outcome.result = std::unexpected(start_err);
        promise->set_value(std::move(outcome));
        fiber::event::EventLoop::current().stop();
        co_return;
    }

    outcome.result = co_await connection.wait_closed();
    outcome.written = fake_transport->written();
    promise->set_value(std::move(outcome));

    fiber::event::EventLoop::current().stop();
    co_return;
}

ServerHeaderRunOutcome execute_server_request(std::vector<std::string> chunks, fiber::http::HttpHandler handler,
                                              fiber::http::HttpServerOptions http_options,
                                              fiber::http::Http2Connection::Options options = {},
                                              bool hold_eof = true) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ServerHeaderRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise = std::move(promise), chunks = std::move(chunks),
                                      handler = std::move(handler), http_options, options, hold_eof]() mutable {
        return run_http2_server_request(std::move(promise), std::move(chunks), std::move(handler), http_options,
                                        options, hold_eof);
    });

    auto status = future.wait_for(std::chrono::seconds(2));
    if (status != std::future_status::ready) {
        group.stop();
        group.join();
        ADD_FAILURE() << "Timed out waiting for http2 server request task";
        return {};
    }

    ServerHeaderRunOutcome outcome = future.get();
    group.join();
    return outcome;
}

ServerHeaderRunOutcome execute_server_request(std::vector<std::string> chunks, fiber::http::HttpHandler handler,
                                              fiber::http::Http2Connection::Options options = {},
                                              bool hold_eof = true) {
    return execute_server_request(std::move(chunks), std::move(handler), fiber::http::HttpServerOptions{}, options,
                                  hold_eof);
}

ServerHeaderRunOutcome execute_server_request(std::vector<std::string> chunks,
                                              fiber::http::Http2Connection::Options options = {}) {
    auto header_result = std::make_shared<fiber::common::IoResult<void>>();
    fiber::http::HttpHandler handler =
            [header_result](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        fiber::http::HttpHeaders headers(exchange.pool());
        headers.set("server", "fiber");
        *header_result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 204,
                .headers = &headers,
                .end_stream = true,
        });
        co_return;
    };
    ServerHeaderRunOutcome outcome = execute_server_request(std::move(chunks), std::move(handler), options);
    outcome.header_result = *header_result;
    return outcome;
}

DetachedTask
run_server_delayed_send_after_close(std::shared_ptr<std::promise<ServerDelayedSendAfterCloseOutcome>> promise,
                                    std::vector<std::string> chunks, fiber::http::HttpServerOptions http_options,
                                    fiber::http::Http2Connection::Options options = {}) {
    auto delayed_send_result = std::make_shared<fiber::common::IoResult<void>>();
    auto delayed_send_completed = std::make_shared<std::atomic<bool>>(false);
    fiber::http::HttpHandler handler = [delayed_send_result, delayed_send_completed](
                                               fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        co_await fiber::async::sleep(std::chrono::milliseconds(10));
        *delayed_send_result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 204,
                .end_stream = true,
        });
        delayed_send_completed->store(true, std::memory_order_release);
        co_return;
    };

    ServerDelayedSendAfterCloseOutcome outcome;
    {
        auto transport = std::make_unique<FakeHttpTransport>(std::move(chunks), std::vector<size_t>{}, false, false);
        fiber::http::ServerRequestFactory factory(http_options, handler);
        fiber::http::Http2Connection connection(options, &factory, fiber::http::ServerRequestFactory::ops());
        fiber::common::IoErr start_err = connection.start(std::move(transport));
        if (start_err != fiber::common::IoErr::None) {
            outcome.run_result = std::unexpected(start_err);
            promise->set_value(std::move(outcome));
            fiber::event::EventLoop::current().stop();
            co_return;
        }

        outcome.run_result = co_await connection.wait_closed();
    }

    for (int i = 0; i < 100 && !delayed_send_completed->load(std::memory_order_acquire); ++i) {
        co_await fiber::async::sleep(std::chrono::milliseconds(1));
    }

    outcome.delayed_send_completed = delayed_send_completed->load(std::memory_order_acquire);
    outcome.delayed_send_result = *delayed_send_result;
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

ServerDelayedSendAfterCloseOutcome
execute_server_delayed_send_after_close(std::vector<std::string> chunks,
                                        fiber::http::HttpServerOptions http_options = {},
                                        fiber::http::Http2Connection::Options options = {}) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ServerDelayedSendAfterCloseOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise = std::move(promise), chunks = std::move(chunks), http_options,
                                      options]() mutable {
        return run_server_delayed_send_after_close(std::move(promise), std::move(chunks), http_options, options);
    });

    auto status = future.wait_for(std::chrono::seconds(2));
    if (status != std::future_status::ready) {
        group.stop();
        group.join();
        ADD_FAILURE() << "Timed out waiting for delayed send after close task";
        return {};
    }

    ServerDelayedSendAfterCloseOutcome outcome = future.get();
    group.join();
    return outcome;
}

class SendingHttp2Connection final : public fiber::http::Http2Connection {
public:
    SendingHttp2Connection(std::unique_ptr<fiber::http::HttpTransport> transport, FakeHttpTransport *fake_transport,
                           Options options = {}) :
        fiber::http::Http2Connection(options, &test_http2_stream_factory(), TestHttp2StreamFactory::ops()),
        fake_transport_(fake_transport) {
        FIBER_ASSERT(start(std::move(transport)) == fiber::common::IoErr::None);
    }

    fiber::common::IoErr submit_bytes(std::string_view data) noexcept {
        return alloc_and_enqueue_control(data.size(),
                                         [data](std::uint8_t *dst) { std::memcpy(dst, data.data(), data.size()); });
    }

    void request_stop(fiber::common::IoErr reason = fiber::common::IoErr::Canceled) noexcept { shutdown(reason); }

    [[nodiscard]] std::int32_t current_connection_send_window() const noexcept { return connection_send_window(); }
    fiber::async::Task<void> stop_and_join() noexcept { co_await stop_and_wait_closed(); }

    SendOutcome snapshot() const {
        SendOutcome outcome;
        if (fake_transport_) {
            outcome.written = fake_transport_->written();
        }
        return outcome;
    }

private:
    FakeHttpTransport *fake_transport_ = nullptr;
};

DetachedTask run_client_request_header_send(std::shared_ptr<std::promise<ClientRequestHeaderRunOutcome>> promise) {
    ClientRequestHeaderRunOutcome outcome;
    auto fake_transport = std::make_unique<FakeHttpTransport>(std::vector<std::string>{});
    auto *fake_transport_ptr = fake_transport.get();

    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    SendingHttp2Connection connection(std::move(fake_transport), fake_transport_ptr, options);

    fiber::mem::BufPool pool;
    fiber::http::ClientHttp2Exchange exchange(connection, pool);
    fiber::http::HttpHeaders headers(pool);
    if (headers.set("user-agent", "fiber-test") == nullptr) {
        outcome.result = std::unexpected(fiber::common::IoErr::NoMem);
        connection.request_stop();
        co_await connection.stop_and_join();
        promise->set_value(std::move(outcome));
        fiber::event::EventLoop::current().stop();
        co_return;
    }

    outcome.result = co_await exchange.send_request_header(
            {
                    .method = fiber::http::HttpMethod::Post,
                    .scheme = "https",
                    .authority = "example.com",
                    .path = "/submit",
                    .headers = &headers,
            },
            true);
    outcome.stream_id = exchange.stream_id();

    connection.request_stop();
    co_await connection.stop_and_join();
    outcome.written = fake_transport_ptr->written();
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask run_client_exchange_abort(std::shared_ptr<std::promise<ClientAbortRunOutcome>> promise) {
    ClientAbortRunOutcome outcome;
    auto fake_transport =
            std::make_unique<FakeHttpTransport>(std::vector<std::string>{}, std::vector<size_t>{}, false, true);
    auto *fake_transport_ptr = fake_transport.get();

    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    SendingHttp2Connection connection(std::move(fake_transport), fake_transport_ptr, options);

    fiber::mem::BufPool pool;
    fiber::http::ClientHttp2Exchange exchange(connection, pool);
    outcome.header_result = co_await exchange.send_request_header(
            {
                    .method = fiber::http::HttpMethod::Post,
                    .scheme = "https",
                    .authority = "example.com",
                    .path = "/upload",
            },
            false);

    outcome.stream_id = exchange.stream_id();
    if (outcome.header_result) {
        outcome.abort_result = exchange.abort(fiber::common::IoErr::ConnReset);
        outcome.local_rst = exchange.stream() != nullptr && exchange.stream()->local_rst();
        co_await fiber::async::sleep(std::chrono::milliseconds(1));
    }

    connection.request_stop();
    co_await connection.stop_and_join();
    outcome.written = fake_transport_ptr->written();
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask
run_client_extended_connect_header_send(std::shared_ptr<std::promise<ClientExtendedConnectRunOutcome>> promise,
                                        bool peer_enabled) {
    ClientExtendedConnectRunOutcome outcome;
    std::string settings_payload;
    if (peer_enabled) {
        settings_payload.assign(6, '\0');
        settings_payload[1] = '\x08';
        settings_payload[5] = '\x01';
    }
    auto fake_transport = std::make_unique<FakeHttpTransport>(std::vector<std::string>{
            make_frame(static_cast<std::uint32_t>(settings_payload.size()), 0x4, 0x0, 0, settings_payload)});
    auto *fake_transport_ptr = fake_transport.get();

    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    SendingHttp2Connection connection(std::move(fake_transport), fake_transport_ptr, options);

    fiber::mem::BufPool pool;
    fiber::http::ClientHttp2Exchange exchange(connection, pool);
    fiber::http::HttpHeaders headers(pool);
    if (headers.set("sec-websocket-version", "13") == nullptr) {
        outcome.header_result = std::unexpected(fiber::common::IoErr::NoMem);
        connection.request_stop();
        co_await connection.stop_and_join();
        promise->set_value(std::move(outcome));
        fiber::event::EventLoop::current().stop();
        co_return;
    }

    outcome.support_before = exchange.extended_connect_support();
    outcome.header_result = co_await exchange.send_request_header(
            {
                    .method = fiber::http::HttpMethod::Connect,
                    .scheme = "https",
                    .authority = "example.com",
                    .path = "/chat",
                    .protocol = "websocket",
                    .headers = &headers,
            },
            false);
    outcome.stream_id = exchange.stream_id();
    if (outcome.header_result) {
        outcome.run_result = co_await connection.wait_closed();
        outcome.support_after = exchange.extended_connect_support();
    }

    co_await connection.stop_and_join();
    outcome.written = fake_transport_ptr->written();
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask run_client_request_body_send(std::shared_ptr<std::promise<ClientRequestBodyRunOutcome>> promise) {
    ClientRequestBodyRunOutcome outcome;
    auto fake_transport =
            std::make_unique<FakeHttpTransport>(std::vector<std::string>{}, std::vector<size_t>{}, false, true);
    auto *fake_transport_ptr = fake_transport.get();

    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    SendingHttp2Connection connection(std::move(fake_transport), fake_transport_ptr, options);

    fiber::mem::BufPool pool;
    fiber::http::ClientHttp2Exchange exchange(connection, pool);
    outcome.header_result = co_await exchange.send_request_header(
            {
                    .method = fiber::http::HttpMethod::Post,
                    .scheme = "https",
                    .authority = "example.com",
                    .path = "/upload",
            },
            false);
    if (outcome.header_result) {
        outcome.body_result = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>("hello"), 5, true);
    }
    outcome.stream_id = exchange.stream_id();
    outcome.conn_send_window = connection.current_connection_send_window();

    connection.request_stop();
    co_await connection.stop_and_join();
    outcome.written = fake_transport_ptr->written();
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask run_client_body_cancel_before_write(std::shared_ptr<std::promise<ClientBodyCancelRunOutcome>> promise) {
    ClientBodyCancelRunOutcome outcome;
    auto fake_transport =
            std::make_unique<FakeHttpTransport>(std::vector<std::string>{}, std::vector<size_t>{}, false, true);
    auto *fake_transport_ptr = fake_transport.get();

    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    SendingHttp2Connection connection(std::move(fake_transport), fake_transport_ptr, options);
    fiber::mem::BufPool pool;
    fiber::http::ClientHttp2Exchange exchange(connection, pool);
    outcome.header_result = co_await exchange.send_request_header(
            {
                    .method = fiber::http::HttpMethod::Post,
                    .scheme = "https",
                    .authority = "example.com",
                    .path = "/cancel-before-write",
            },
            false);
    if (outcome.header_result) {
        outcome.canceled_body_result = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>("drop"), 4,
                                                                   false, std::chrono::milliseconds::zero());
        outcome.conn_window_after_cancel = connection.current_connection_send_window();
        outcome.stream_window_after_cancel = exchange.stream()->send_window();
        outcome.retried_body_result =
                co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>("keep"), 4, true);
        co_await fiber::async::sleep(std::chrono::milliseconds(1));
    }

    connection.request_stop();
    co_await connection.stop_and_join();
    outcome.written = fake_transport_ptr->written();
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask run_client_partial_request_body_send(std::shared_ptr<std::promise<ClientPartialBodyRunOutcome>> promise) {
    ClientPartialBodyRunOutcome outcome;
    auto fake_transport =
            std::make_unique<FakeHttpTransport>(std::vector<std::string>{}, std::vector<size_t>{}, false, true);
    auto *fake_transport_ptr = fake_transport.get();

    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.initial_connection_send_window = 2;
    SendingHttp2Connection connection(std::move(fake_transport), fake_transport_ptr, options);
    fiber::mem::BufPool pool;
    fiber::http::ClientHttp2Exchange exchange(connection, pool);
    outcome.header_result = co_await exchange.send_request_header(
            {
                    .method = fiber::http::HttpMethod::Post,
                    .scheme = "https",
                    .authority = "example.com",
                    .path = "/partial-body",
            },
            false);
    if (outcome.header_result) {
        constexpr std::string_view kBody = "hello";
        outcome.first_body_result =
                co_await exchange.write(reinterpret_cast<const std::uint8_t *>(kBody.data()), kBody.size(), true);
        if (outcome.first_body_result) {
            fiber::async::spawn([fake_transport_ptr]() -> fiber::async::DetachedTask {
                co_await fiber::async::sleep(std::chrono::milliseconds(1));
                std::string update_payload(4, '\0');
                update_payload[3] = 3;
                fake_transport_ptr->append_read_chunk(make_frame(4, 0x8, 0x0, 0, update_payload));
                co_return;
            });
            outcome.second_body_result =
                    co_await exchange.write(reinterpret_cast<const std::uint8_t *>(kBody.data() + 2), 3, true);
        }
    }

    connection.request_stop();
    co_await connection.stop_and_join();
    outcome.written = fake_transport_ptr->written();
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask
run_client_body_waiting_for_connection_window(std::shared_ptr<std::promise<ClientConnWindowWaitRunOutcome>> promise) {
    ClientConnWindowWaitRunOutcome outcome;
    auto fake_transport = std::make_unique<FakeHttpTransport>(std::vector<std::string>{make_frame(0, 0x4, 0x0, 0, {})},
                                                              std::vector<size_t>{}, false, true);
    auto *fake_transport_ptr = fake_transport.get();

    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.initial_connection_send_window = 0;
    SendingHttp2Connection connection(std::move(fake_transport), fake_transport_ptr, options);
    fiber::mem::BufPool pool;
    fiber::http::ClientHttp2Exchange exchange(connection, pool);
    outcome.header_result = co_await exchange.send_request_header(
            {
                    .method = fiber::http::HttpMethod::Post,
                    .scheme = "https",
                    .authority = "example.com",
                    .path = "/connection-window",
            },
            false);

    if (outcome.header_result) {
        fiber::async::spawn([fake_transport_ptr]() -> fiber::async::DetachedTask {
            co_await fiber::async::sleep(std::chrono::milliseconds(1));
            std::string update_payload(4, '\0');
            update_payload[3] = 5;
            fake_transport_ptr->append_read_chunk(make_frame(4, 0x8, 0x0, 0, update_payload));
            co_return;
        });
        outcome.body_result = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>("hello"), 5, true);
        outcome.conn_send_window = connection.current_connection_send_window();
        outcome.stream_send_window = exchange.stream()->send_window();
    }

    connection.request_stop();
    co_await connection.stop_and_join();
    outcome.written = fake_transport_ptr->written();
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask run_client_request_trailer_send(std::shared_ptr<std::promise<ClientRequestTrailerRunOutcome>> promise) {
    ClientRequestTrailerRunOutcome outcome;
    auto fake_transport =
            std::make_unique<FakeHttpTransport>(std::vector<std::string>{}, std::vector<size_t>{}, false, true);
    auto *fake_transport_ptr = fake_transport.get();

    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    SendingHttp2Connection connection(std::move(fake_transport), fake_transport_ptr, options);

    fiber::mem::BufPool pool;
    fiber::http::ClientHttp2Exchange exchange(connection, pool);
    outcome.header_result = co_await exchange.send_request_header(
            {
                    .method = fiber::http::HttpMethod::Post,
                    .scheme = "https",
                    .authority = "example.com",
                    .path = "/upload",
            },
            false);
    if (outcome.header_result) {
        outcome.body_result = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>("hello"), 5, false);
    }
    if (outcome.body_result) {
        fiber::mem::BufPool pool;
        fiber::http::HttpHeaders trailers(pool);
        if (trailers.set("digest", "sha-256=xyz") == nullptr) {
            outcome.trailer_result = std::unexpected(fiber::common::IoErr::NoMem);
        } else {
            outcome.trailer_result = co_await exchange.write_trailer(trailers);
        }
    }
    outcome.stream_id = exchange.stream_id();

    connection.request_stop();
    co_await connection.stop_and_join();
    outcome.written = fake_transport_ptr->written();
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask run_client_response_body_read(std::shared_ptr<std::promise<ClientResponseBodyRunOutcome>> promise) {
    ClientResponseBodyRunOutcome outcome;
    std::string response;
    response += make_frame(0, 0x4, 0x0, 0, {});
    response += build_headers_frame_bytes(1,
                                          {
                                                  {":status", "200"},
                                                  {"content-type", "text/plain"},
                                          },
                                          false);
    response += make_frame(5, 0x0, 0x1, 1, "hello");

    constexpr std::size_t kSettingsFrameSize = 9;
    constexpr std::size_t kHeadersFrameHeaderSize = 9;
    const std::size_t split = kSettingsFrameSize + kHeadersFrameHeaderSize + 1;
    auto fake_transport = std::make_unique<FakeHttpTransport>(
            std::vector<std::string>{response.substr(0, split), response.substr(split)});
    auto *fake_transport_ptr = fake_transport.get();

    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    SendingHttp2Connection connection(std::move(fake_transport), fake_transport_ptr, options);

    fiber::mem::BufPool pool;
    fiber::http::ClientHttp2Exchange exchange(connection, pool);
    outcome.header_result = co_await exchange.send_request_header(
            {
                    .method = fiber::http::HttpMethod::Get,
                    .scheme = "https",
                    .authority = "example.com",
                    .path = "/download",
            },
            true);
    outcome.stream_id = exchange.stream_id();
    if (outcome.header_result) {
        outcome.run_result = co_await connection.wait_closed();
        outcome.body_result = co_await exchange.read_body(64);
    }

    co_await connection.stop_and_join();
    outcome.written = fake_transport_ptr->written();
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask
run_client_response_headers_and_trailers_read(std::shared_ptr<std::promise<ClientResponseHeaderRunOutcome>> promise) {
    ClientResponseHeaderRunOutcome outcome;
    std::string response;
    response += make_frame(0, 0x4, 0x0, 0, {});
    response += build_headers_frame_bytes(1,
                                          {
                                                  {":status", "103"},
                                                  {"link", "</style.css>; rel=preload"},
                                          },
                                          false);
    response += build_headers_frame_bytes(1,
                                          {
                                                  {":status", "200"},
                                                  {"content-type", "text/plain"},
                                          },
                                          false);
    response += make_frame(5, 0x0, 0x0, 1, "hello");
    response += build_headers_frame_bytes(1,
                                          {
                                                  {"digest", "sha-256=xyz"},
                                          },
                                          true);

    auto fake_transport = std::make_unique<FakeHttpTransport>(std::vector<std::string>{std::move(response)});
    auto *fake_transport_ptr = fake_transport.get();

    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    SendingHttp2Connection connection(std::move(fake_transport), fake_transport_ptr, options);

    fiber::mem::BufPool pool;
    fiber::http::ClientHttp2Exchange exchange(connection, pool);
    outcome.header_result = co_await exchange.send_request_header(
            {
                    .method = fiber::http::HttpMethod::Get,
                    .scheme = "https",
                    .authority = "example.com",
                    .path = "/with-trailers",
            },
            true);
    outcome.stream_id = exchange.stream_id();
    if (outcome.header_result) {
        outcome.run_result = co_await connection.wait_closed();
    }
    if (outcome.run_result) {
        outcome.informational_result = co_await exchange.read_header();
        if (outcome.informational_result) {
            outcome.informational = snapshot_response_head(*outcome.informational_result);
        }
        outcome.final_result = co_await exchange.read_header();
        if (outcome.final_result) {
            outcome.final = snapshot_response_head(*outcome.final_result);
        }
        outcome.body_result = co_await exchange.read_body(64);
        outcome.trailer_result = co_await exchange.read_header();
        if (outcome.trailer_result) {
            outcome.trailer = snapshot_response_head(*outcome.trailer_result);
        }
        outcome.end_result = co_await exchange.read_header();
    }

    co_await connection.stop_and_join();
    outcome.written = fake_transport_ptr->written();
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask
run_client_response_header_end_stream_read(std::shared_ptr<std::promise<ClientResponseHeaderRunOutcome>> promise) {
    ClientResponseHeaderRunOutcome outcome;
    std::string response;
    response += make_frame(0, 0x4, 0x0, 0, {});
    response += build_headers_frame_bytes(1,
                                          {
                                                  {":status", "204"},
                                                  {"content-type", "text/plain"},
                                          },
                                          true);

    auto fake_transport = std::make_unique<FakeHttpTransport>(std::vector<std::string>{std::move(response)});
    auto *fake_transport_ptr = fake_transport.get();

    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    SendingHttp2Connection connection(std::move(fake_transport), fake_transport_ptr, options);

    fiber::mem::BufPool pool;
    fiber::http::ClientHttp2Exchange exchange(connection, pool);
    outcome.header_result = co_await exchange.send_request_header(
            {
                    .method = fiber::http::HttpMethod::Get,
                    .scheme = "https",
                    .authority = "example.com",
                    .path = "/no-body",
            },
            true);
    outcome.stream_id = exchange.stream_id();
    if (outcome.header_result) {
        outcome.run_result = co_await connection.wait_closed();
    }
    if (outcome.run_result) {
        outcome.final_result = co_await exchange.read_header();
        if (outcome.final_result) {
            outcome.final = snapshot_response_head(*outcome.final_result);
        }
        outcome.body_result = co_await exchange.read_body(64);
        outcome.end_result = co_await exchange.read_header();
    }

    co_await connection.stop_and_join();
    outcome.written = fake_transport_ptr->written();
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask
run_client_response_read_after_rst_stream(std::shared_ptr<std::promise<ClientResponseAbortRunOutcome>> promise) {
    ClientResponseAbortRunOutcome outcome;
    std::string response;
    response += make_frame(0, 0x4, 0x0, 0, {});
    std::string rst_payload(4, '\0');
    response += make_frame(4, 0x3, 0x0, 1, rst_payload);

    auto fake_transport = std::make_unique<FakeHttpTransport>(std::vector<std::string>{std::move(response)});
    auto *fake_transport_ptr = fake_transport.get();

    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    SendingHttp2Connection connection(std::move(fake_transport), fake_transport_ptr, options);

    fiber::mem::BufPool pool;
    fiber::http::ClientHttp2Exchange exchange(connection, pool);
    outcome.header_result = co_await exchange.send_request_header(
            {
                    .method = fiber::http::HttpMethod::Get,
                    .scheme = "https",
                    .authority = "example.com",
                    .path = "/rst",
            },
            true);
    outcome.stream_id = exchange.stream_id();
    if (outcome.header_result) {
        outcome.run_result = co_await connection.wait_closed();
        outcome.read_header_result = co_await exchange.read_header();
        outcome.read_body_result = co_await exchange.read_body(64);
    }

    co_await connection.stop_and_join();
    outcome.written = fake_transport_ptr->written();
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask run_client_exchange_open_after_goaway(std::shared_ptr<std::promise<ClientGoawayRunOutcome>> promise) {
    ClientGoawayRunOutcome outcome;
    std::string response;
    response += make_frame(0, 0x4, 0x0, 0, {});
    std::string goaway_payload(8, '\0');
    goaway_payload[3] = '\x1';
    response += make_frame(8, 0x7, 0x0, 0, goaway_payload);

    auto fake_transport = std::make_unique<FakeHttpTransport>(std::vector<std::string>{std::move(response)},
                                                              std::vector<size_t>{}, false, true);
    auto *fake_transport_ptr = fake_transport.get();

    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    SendingHttp2Connection connection(std::move(fake_transport), fake_transport_ptr, options);

    fiber::mem::BufPool first_pool;
    fiber::http::ClientHttp2Exchange first_exchange(connection, first_pool);
    auto first_send_result = co_await first_exchange.send_request_header(
            {
                    .method = fiber::http::HttpMethod::Get,
                    .scheme = "https",
                    .authority = "example.com",
                    .path = "/keep-open",
            },
            true);
    if (!first_send_result) {
        outcome.send_result = std::unexpected(first_send_result.error());
        outcome.state = connection.state();
        connection.request_stop();
        co_await connection.stop_and_join();
        outcome.run_result = fiber::common::IoResult<void>{};
        outcome.written = fake_transport_ptr->written();
        promise->set_value(std::move(outcome));
        fiber::event::EventLoop::current().stop();
        co_return;
    }

    auto run_result = std::make_shared<fiber::common::IoResult<void>>();
    auto run_done = std::make_shared<std::atomic_bool>(false);
    fiber::async::spawn([connection = &connection, run_result, run_done]() {
        return run_http2_connection_capture_task(connection, run_result, run_done);
    });

    for (int i = 0; i < 50 && connection.state() != fiber::http::Http2Connection::State::Draining &&
                    !run_done->load(std::memory_order_acquire);
         ++i) {
        co_await fiber::async::sleep(std::chrono::milliseconds(1));
    }

    outcome.state = connection.state();
    if (outcome.state == fiber::http::Http2Connection::State::Draining) {
        fiber::mem::BufPool pool;
        fiber::http::ClientHttp2Exchange exchange(connection, pool);
        outcome.send_result = co_await exchange.send_request_header(
                {
                        .method = fiber::http::HttpMethod::Get,
                        .scheme = "https",
                        .authority = "example.com",
                        .path = "/after-goaway",
                },
                true);
    } else {
        outcome.send_result = std::unexpected(fiber::common::IoErr::TimedOut);
    }

    connection.request_stop();
    co_await connection.stop_and_join();
    while (!run_done->load(std::memory_order_acquire)) {
        co_await fiber::async::sleep(std::chrono::milliseconds(1));
    }
    outcome.run_result = *run_result;
    outcome.written = fake_transport_ptr->written();
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

class ControlHttp2Connection final : public fiber::http::Http2Connection {
public:
    ControlHttp2Connection(std::unique_ptr<fiber::http::HttpTransport> transport, FakeHttpTransport *fake_transport,
                           Options options = {}) :
        fiber::http::Http2Connection(options, &test_http2_stream_factory(), TestHttp2StreamFactory::ops()),
        fake_transport_(fake_transport) {
        FIBER_ASSERT(start(std::move(transport)) == fiber::common::IoErr::None);
    }

    fiber::http::Http2Stream *open_stream() noexcept {
        auto *owner = TestHttp2StreamOwner::create_owner();
        if (!owner) {
            return nullptr;
        }
        auto lease = try_attach_local_stream(owner->stream);
        if (!lease) {
            delete owner;
            return nullptr;
        }
        return lease->get();
    }
    fiber::http::Http2Stream *open_stream(std::uint32_t) noexcept { return open_stream(); }
    void request_graceful_close() noexcept { graceful_shutdown(); }
    [[nodiscard]] std::int32_t current_connection_send_window() const noexcept { return connection_send_window(); }
    [[nodiscard]] std::uint32_t current_peer_max_frame_size() const noexcept { return peer_max_outbound_frame_size(); }
    [[nodiscard]] std::uint32_t current_peer_max_concurrent_streams() const noexcept {
        return peer_max_concurrent_streams();
    }
    [[nodiscard]] bool current_peer_enable_push() const noexcept { return peer_enable_push(); }
    [[nodiscard]] ConnectionRole current_role() const noexcept { return role(); }
    [[nodiscard]] bool current_has_stream(std::uint32_t stream_id) const noexcept { return has_stream(stream_id); }
    [[nodiscard]] State current_state() const noexcept { return state(); }
    [[nodiscard]] std::int32_t current_stream_send_window(std::uint32_t stream_id) const noexcept {
        const fiber::http::Http2Stream *stream = find_stream(stream_id);
        return stream ? stream->send_window() : 0;
    }
    [[nodiscard]] bool current_stream_remote_end_stream(std::uint32_t stream_id) const noexcept {
        const fiber::http::Http2Stream *stream = find_stream(stream_id);
        return stream ? stream->remote_end_stream() : false;
    }
    [[nodiscard]] bool current_stream_remote_rst(std::uint32_t stream_id) const noexcept {
        const fiber::http::Http2Stream *stream = find_stream(stream_id);
        return stream ? stream->remote_rst() : false;
    }
    void request_stop(fiber::common::IoErr reason = fiber::common::IoErr::Canceled) noexcept { shutdown(reason); }
    fiber::async::Task<void> stop_and_join() noexcept { co_await stop_and_wait_closed(); }
    [[nodiscard]] const std::string &written() const noexcept { return fake_transport_->written(); }

private:
    FakeHttpTransport *fake_transport_ = nullptr;
};

class KeepaliveHttp2Connection final : public fiber::http::Http2Connection {
public:
    KeepaliveHttp2Connection(std::unique_ptr<fiber::http::HttpTransport> transport,
                             ScriptedReadTransport *transport_impl, Options options = {}) :
        fiber::http::Http2Connection(options, &test_http2_stream_factory(), TestHttp2StreamFactory::ops()),
        transport_impl_(transport_impl) {
        FIBER_ASSERT(start(std::move(transport)) == fiber::common::IoErr::None);
    }

    [[nodiscard]] const std::string &written() const noexcept { return transport_impl_->written(); }
    [[nodiscard]] std::size_t transport_close_count() const noexcept { return transport_impl_->close_count(); }
    [[nodiscard]] State current_state() const noexcept { return state(); }
    [[nodiscard]] bool read_buffer_allocated() const noexcept { return static_cast<bool>(inbound_io_.read_buf); }

private:
    ScriptedReadTransport *transport_impl_ = nullptr;
};

using SendScript = std::function<fiber::common::IoErr(SendingHttp2Connection &)>;
using ControlScript =
        std::function<void(ControlHttp2Connection &, fiber::http::Http2Stream *, fiber::http::Http2Stream *)>;

DetachedTask run_http2_connection_task(fiber::http::Http2Connection *connection,
                                       std::shared_ptr<std::atomic_bool> done = nullptr) {
    if (!connection) {
        co_return;
    }
    (void) co_await connection->wait_closed();
    if (done) {
        done->store(true, std::memory_order_release);
    }
}

struct ControlSetupContext {
    ControlHttp2Connection *connection = nullptr;
    FakeHttpTransport *fake_transport = nullptr;
    ControlScript setup;
    bool block_reads_for_setup = false;
    bool strip_initial_flight = false;
    bool snapshot_before_shutdown = false;
    ControlRunOutcome *outcome = nullptr;
    fiber::http::Http2Stream **stream1 = nullptr;
    fiber::http::Http2Stream **stream3 = nullptr;
    std::uint32_t *stream1_id = nullptr;
    std::uint32_t *stream3_id = nullptr;
    fiber::http::Http2Stream *local_stream1 = nullptr;
    fiber::http::Http2Stream *local_stream3 = nullptr;
};

void set_control_stream_slot(fiber::http::Http2Stream **slot, std::uint32_t *stream_id_slot,
                             fiber::http::Http2Stream *stream) noexcept {
    if (slot) {
        *slot = stream;
    }
    if (stream_id_slot) {
        *stream_id_slot = stream ? stream->stream_id() : 0;
    }
}

void capture_control_outcome(const ControlSetupContext &ctx) {
    ControlRunOutcome &outcome = *ctx.outcome;
    const bool strip_initial_flight =
            ctx.connection->current_role() == fiber::http::Http2Connection::ConnectionRole::Client &&
            ctx.strip_initial_flight;
    outcome.written =
            strip_initial_flight ? strip_client_initial_flight(ctx.connection->written()) : ctx.connection->written();
    outcome.state = ctx.connection->current_state();
    outcome.transport_close_count = ctx.fake_transport->close_count();
    outcome.conn_send_window = ctx.connection->current_connection_send_window();
    outcome.peer_max_frame_size = ctx.connection->current_peer_max_frame_size();
    outcome.peer_max_concurrent_streams = ctx.connection->current_peer_max_concurrent_streams();
    outcome.peer_enable_push = ctx.connection->current_peer_enable_push();
    if (*ctx.stream1_id != 0) {
        outcome.stream1_send_window = ctx.connection->current_stream_send_window(*ctx.stream1_id);
        outcome.stream1_registered = ctx.connection->current_has_stream(*ctx.stream1_id);
        outcome.stream1_remote_end_stream = ctx.connection->current_stream_remote_end_stream(*ctx.stream1_id);
        outcome.stream1_remote_rst = ctx.connection->current_stream_remote_rst(*ctx.stream1_id);
    }
    outcome.stream2_registered = ctx.connection->current_has_stream(2);
    outcome.stream2_remote_end_stream = ctx.connection->current_stream_remote_end_stream(2);
    if (*ctx.stream3_id != 0) {
        outcome.stream3_send_window = ctx.connection->current_stream_send_window(*ctx.stream3_id);
        outcome.stream3_registered = ctx.connection->current_has_stream(*ctx.stream3_id);
    }
}

DetachedTask run_control_setup_task(ControlSetupContext ctx) {
    if (!ctx.connection) {
        co_return;
    }

    if (ctx.block_reads_for_setup) {
        set_control_stream_slot(ctx.stream1, ctx.stream1_id, ctx.local_stream1);
        set_control_stream_slot(ctx.stream3, ctx.stream3_id, ctx.local_stream3);
        ctx.setup(*ctx.connection, *ctx.stream1, *ctx.stream3);
        ctx.fake_transport->release_reads();
    } else if (ctx.setup) {
        ctx.setup(*ctx.connection, ctx.local_stream1, ctx.local_stream3);
        set_control_stream_slot(ctx.stream1, ctx.stream1_id, ctx.local_stream1);
        set_control_stream_slot(ctx.stream3, ctx.stream3_id, ctx.local_stream3);
    }

    if (ctx.snapshot_before_shutdown) {
        co_await fiber::async::sleep(std::chrono::milliseconds(5));
        capture_control_outcome(ctx);
        ctx.connection->request_stop(fiber::common::IoErr::None);
    }
}

DetachedTask run_send_connection(std::shared_ptr<std::promise<SendOutcome>> promise, std::vector<size_t> write_steps,
                                 std::size_t expected_written, SendScript submit,
                                 fiber::http::Http2Connection::Options options = {}) {
    options.role = fiber::http::Http2Connection::ConnectionRole::Server;
    auto transport = std::make_unique<FakeHttpTransport>(std::vector<std::string>{}, std::move(write_steps), true);
    auto *fake_transport = transport.get();
    SendingHttp2Connection connection(std::move(transport), fake_transport, options);
    auto run_done = std::make_shared<std::atomic_bool>(false);
    fiber::async::spawn(
            [connection = &connection, run_done]() { return run_http2_connection_task(connection, run_done); });
    co_await fiber::async::sleep(std::chrono::milliseconds(1));

    SendOutcome outcome;
    outcome.submit_error = submit(connection);
    if (outcome.submit_error != fiber::common::IoErr::None) {
        promise->set_value(std::move(outcome));
        fiber::event::EventLoop::current().stop();
        co_return;
    }

    for (int i = 0; i < 50 && fake_transport->written().size() < expected_written; ++i) {
        co_await fiber::async::sleep(std::chrono::milliseconds(1));
    }
    co_await fiber::async::sleep(std::chrono::milliseconds(1));
    outcome = connection.snapshot();
    co_await connection.stop_and_join();
    while (!run_done->load(std::memory_order_acquire)) {
        co_await fiber::async::sleep(std::chrono::milliseconds(1));
    }
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

SendOutcome execute_send_connection(SendScript submit, std::size_t expected_written,
                                    std::vector<size_t> write_steps = {},
                                    fiber::http::Http2Connection::Options options = {}) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<SendOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise = std::move(promise), write_steps = std::move(write_steps),
                                      expected_written, submit = std::move(submit), options]() mutable {
        return run_send_connection(std::move(promise), std::move(write_steps), expected_written, std::move(submit),
                                   options);
    });

    auto status = future.wait_for(std::chrono::seconds(2));
    if (status != std::future_status::ready) {
        group.stop();
        group.join();
        ADD_FAILURE() << "Timed out waiting for http2 send task";
        return {};
    }

    SendOutcome outcome = future.get();
    group.join();
    return outcome;
}

DetachedTask run_control_connection(std::shared_ptr<std::promise<ControlRunOutcome>> promise,
                                    std::vector<std::string> chunks, ControlScript setup,
                                    fiber::http::Http2Connection::Options options = {},
                                    bool preserve_initial_flight = false, bool open_streams_for_setup = true,
                                    bool snapshot_before_shutdown = false) {
    const bool block_reads_for_setup =
            static_cast<bool>(setup) && options.role == fiber::http::Http2Connection::ConnectionRole::Client;
    auto transport = std::make_unique<FakeHttpTransport>(std::move(chunks), std::vector<size_t>{},
                                                         block_reads_for_setup, snapshot_before_shutdown);
    auto *fake_transport = transport.get();
    ControlHttp2Connection connection(std::move(transport), fake_transport, options);
    fiber::http::Http2Stream *stream1 = nullptr;
    fiber::http::Http2Stream *stream3 = nullptr;
    std::uint32_t stream1_id = 0;
    std::uint32_t stream3_id = 0;
    ControlRunOutcome outcome;
    fiber::http::Http2Stream::Lease local_stream1;
    fiber::http::Http2Stream::Lease local_stream3;
    if (options.role == fiber::http::Http2Connection::ConnectionRole::Client && open_streams_for_setup && setup) {
        auto *local_owner1 = TestHttp2StreamOwner::create_owner();
        auto *local_owner3 = TestHttp2StreamOwner::create_owner();
        if (!local_owner1 || !local_owner3) {
            delete local_owner1;
            delete local_owner3;
            outcome.result = std::unexpected(fiber::common::IoErr::NoMem);
            promise->set_value(std::move(outcome));
            fiber::event::EventLoop::current().stop();
            co_return;
        }
        auto local_stream1_result = connection.try_attach_local_stream(local_owner1->stream);
        auto local_stream3_result = connection.try_attach_local_stream(local_owner3->stream);
        if (!local_stream1_result || !local_stream3_result) {
            if (!local_stream1_result) {
                delete local_owner1;
            }
            if (!local_stream3_result) {
                delete local_owner3;
            }
            outcome.result = std::unexpected(!local_stream1_result ? local_stream1_result.error()
                                                                   : local_stream3_result.error());
            promise->set_value(std::move(outcome));
            fiber::event::EventLoop::current().stop();
            co_return;
        }
        local_stream1 = std::move(*local_stream1_result);
        local_stream3 = std::move(*local_stream3_result);
    }

    auto capture_outcome = [&]() {
        ControlSetupContext ctx;
        ctx.connection = &connection;
        ctx.fake_transport = fake_transport;
        ctx.block_reads_for_setup = block_reads_for_setup;
        ctx.strip_initial_flight = options.role == fiber::http::Http2Connection::ConnectionRole::Client &&
                                   (static_cast<bool>(setup) || !preserve_initial_flight);
        ctx.outcome = &outcome;
        ctx.stream1 = &stream1;
        ctx.stream3 = &stream3;
        ctx.stream1_id = &stream1_id;
        ctx.stream3_id = &stream3_id;
        capture_control_outcome(ctx);
    };

    if (snapshot_before_shutdown) {
        ControlSetupContext setup_ctx{&connection,
                                      fake_transport,
                                      setup,
                                      block_reads_for_setup,
                                      options.role == fiber::http::Http2Connection::ConnectionRole::Client &&
                                              (static_cast<bool>(setup) || !preserve_initial_flight),
                                      true,
                                      &outcome,
                                      &stream1,
                                      &stream3,
                                      &stream1_id,
                                      &stream3_id,
                                      local_stream1.get(),
                                      local_stream3.get()};
        fiber::async::spawn([setup_ctx]() mutable { return run_control_setup_task(std::move(setup_ctx)); });

        outcome.result = co_await connection.wait_closed();
        promise->set_value(std::move(outcome));
        fiber::event::EventLoop::current().stop();
        co_return;
    }

    if (block_reads_for_setup) {
        ControlSetupContext setup_ctx{&connection,
                                      fake_transport,
                                      setup,
                                      block_reads_for_setup,
                                      options.role == fiber::http::Http2Connection::ConnectionRole::Client &&
                                              (static_cast<bool>(setup) || !preserve_initial_flight),
                                      false,
                                      &outcome,
                                      &stream1,
                                      &stream3,
                                      &stream1_id,
                                      &stream3_id,
                                      local_stream1.get(),
                                      local_stream3.get()};
        fiber::async::spawn([setup_ctx]() mutable { return run_control_setup_task(std::move(setup_ctx)); });
        outcome.result = co_await connection.wait_closed();
    } else {
        if (setup) {
            setup(connection, local_stream1.get(), local_stream3.get());
            stream1 = local_stream1.get();
            stream3 = local_stream3.get();
            stream1_id = local_stream1->stream_id();
            stream3_id = local_stream3->stream_id();
        }
        outcome.result = co_await connection.wait_closed();
    }
    co_await fiber::async::sleep(std::chrono::milliseconds(1));
    capture_outcome();
    co_await connection.stop_and_join();
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

ControlRunOutcome execute_control_connection(std::vector<std::string> chunks, ControlScript setup = {},
                                             fiber::http::Http2Connection::Options options = {},
                                             bool preserve_initial_flight = false, bool open_streams_for_setup = true,
                                             bool snapshot_before_shutdown = false) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ControlRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise = std::move(promise), chunks = std::move(chunks),
                                      setup = std::move(setup), options, preserve_initial_flight,
                                      open_streams_for_setup, snapshot_before_shutdown]() mutable {
        return run_control_connection(std::move(promise), std::move(chunks), std::move(setup), options,
                                      preserve_initial_flight, open_streams_for_setup, snapshot_before_shutdown);
    });

    auto status = future.wait_for(std::chrono::seconds(2));
    if (status != std::future_status::ready) {
        group.stop();
        group.join();
        ADD_FAILURE() << "Timed out waiting for http2 control task";
        return {};
    }

    ControlRunOutcome outcome = future.get();
    group.join();
    return outcome;
}

struct ClientConcurrentLimitOutcome {
    bool first_opened = false;
    fiber::common::IoErr second_open_error = fiber::common::IoErr::None;
    bool third_opened = false;
};

DetachedTask run_client_concurrent_limit(std::shared_ptr<std::promise<ClientConcurrentLimitOutcome>> promise,
                                         fiber::http::Http2Connection::Options options = {}) {
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    auto transport =
            std::make_unique<FakeHttpTransport>(std::vector<std::string>{}, std::vector<size_t>{}, true, false);
    auto *fake_transport = transport.get();
    ControlHttp2Connection connection(std::move(transport), fake_transport, options);

    ClientConcurrentLimitOutcome outcome;
    EXPECT_EQ(connection.apply_settings_parameter(0x3, 1), fiber::common::IoErr::None);

    auto *owner1 = TestHttp2StreamOwner::create_owner();
    if (!owner1) {
        connection.request_stop(fiber::common::IoErr::NoMem);
        co_await connection.stop_and_join();
        promise->set_value(outcome);
        fiber::event::EventLoop::current().stop();
        co_return;
    }
    auto first = connection.try_attach_local_stream(owner1->stream);
    outcome.first_opened = first.has_value();

    auto *owner2 = TestHttp2StreamOwner::create_owner();
    if (!owner2) {
        if (first) {
            first->get()->close(fiber::common::IoErr::Canceled);
            connection.try_release_stream(*first->get());
            first->reset();
        } else {
            delete owner1;
        }
        connection.request_stop(fiber::common::IoErr::NoMem);
        co_await connection.stop_and_join();
        promise->set_value(outcome);
        fiber::event::EventLoop::current().stop();
        co_return;
    }
    auto second = connection.try_attach_local_stream(owner2->stream);
    if (!second) {
        delete owner2;
        outcome.second_open_error = second.error();
    } else {
        second->get()->close(fiber::common::IoErr::Canceled);
        connection.try_release_stream(*second->get());
        second->reset();
    }

    if (first) {
        first->get()->close(fiber::common::IoErr::Canceled);
        connection.try_release_stream(*first->get());
        first->reset();
    } else {
        delete owner1;
    }

    auto *owner3 = TestHttp2StreamOwner::create_owner();
    if (!owner3) {
        connection.request_stop(fiber::common::IoErr::NoMem);
        co_await connection.stop_and_join();
        promise->set_value(outcome);
        fiber::event::EventLoop::current().stop();
        co_return;
    }
    auto third = connection.try_attach_local_stream(owner3->stream);
    outcome.third_opened = third.has_value();
    if (third) {
        third->get()->close(fiber::common::IoErr::Canceled);
        connection.try_release_stream(*third->get());
        third->reset();
    } else {
        delete owner3;
    }

    connection.request_stop();
    co_await connection.stop_and_join();
    promise->set_value(outcome);
    fiber::event::EventLoop::current().stop();
    co_return;
}

ClientConcurrentLimitOutcome execute_client_concurrent_limit(fiber::http::Http2Connection::Options options = {}) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ClientConcurrentLimitOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise = std::move(promise), options]() mutable {
        return run_client_concurrent_limit(std::move(promise), options);
    });

    auto status = future.wait_for(std::chrono::seconds(2));
    if (status != std::future_status::ready) {
        group.stop();
        group.join();
        ADD_FAILURE() << "Timed out waiting for http2 client concurrent limit task";
        return {};
    }

    ClientConcurrentLimitOutcome outcome = future.get();
    group.join();
    return outcome;
}

enum class LocalAttachWakeAction : std::uint8_t {
    None,
    CloseFirstStream,
    IncreaseLimit,
    Shutdown,
    PeerGoaway,
};

struct LocalAttachWaitOutcome {
    fiber::common::IoErr result = fiber::common::IoErr::Unknown;
    fiber::http::Http2Connection::State state = fiber::http::Http2Connection::State::Init;
    std::uint32_t stream_id = 0;
    std::uint32_t next_stream_id = 0;
    std::size_t waiter_count = 0;
    std::size_t granted_count = 0;
    bool wait_observed = false;
};

DetachedTask run_local_attach_wake_action(ControlHttp2Connection *connection, fiber::http::Http2Stream *first_stream,
                                          LocalAttachWakeAction action, bool *wait_observed, bool *action_done) {
    co_await fiber::async::sleep(std::chrono::milliseconds(2));
    *wait_observed = connection->local_stream_attach_waiter_count_ == 1;

    switch (action) {
        case LocalAttachWakeAction::None:
            break;
        case LocalAttachWakeAction::CloseFirstStream:
            FIBER_ASSERT(first_stream != nullptr);
            first_stream->close(fiber::common::IoErr::Canceled);
            connection->try_release_stream(*first_stream);
            break;
        case LocalAttachWakeAction::IncreaseLimit:
            FIBER_ASSERT(connection->apply_settings_parameter(0x3, 1) == fiber::common::IoErr::None);
            break;
        case LocalAttachWakeAction::Shutdown:
            connection->request_stop();
            break;
        case LocalAttachWakeAction::PeerGoaway:
            connection->handle_peer_goaway(0, fiber::http::Http2ErrorCode::NoError);
            break;
    }

    *action_done = true;
    co_return;
}

DetachedTask run_local_attach_wait(std::shared_ptr<std::promise<LocalAttachWaitOutcome>> promise,
                                   std::uint32_t initial_limit, bool open_first_stream, LocalAttachWakeAction action,
                                   std::chrono::milliseconds timeout) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    auto transport =
            std::make_unique<FakeHttpTransport>(std::vector<std::string>{}, std::vector<size_t>{}, true, false);
    auto *fake_transport = transport.get();
    ControlHttp2Connection connection(std::move(transport), fake_transport, options);
    FIBER_ASSERT(connection.apply_settings_parameter(0x3, initial_limit) == fiber::common::IoErr::None);

    fiber::http::Http2Stream::Lease first;
    if (open_first_stream) {
        std::unique_ptr<TestHttp2StreamOwner> owner(TestHttp2StreamOwner::create_owner());
        FIBER_ASSERT(owner != nullptr);
        auto attached = connection.try_attach_local_stream(owner->stream);
        FIBER_ASSERT(attached.has_value());
        first = std::move(*attached);
        (void) owner.release();
    }

    std::unique_ptr<TestHttp2StreamOwner> pending(TestHttp2StreamOwner::create_owner());
    FIBER_ASSERT(pending != nullptr);
    bool wait_observed = false;
    bool action_done = action == LocalAttachWakeAction::None;
    if (action != LocalAttachWakeAction::None) {
        fiber::async::spawn([&connection, first_stream = first.get(), action, &wait_observed, &action_done]() {
            return run_local_attach_wake_action(&connection, first_stream, action, &wait_observed, &action_done);
        });
    }

    auto attached = co_await connection.attach_local_stream(pending->stream, timeout);
    fiber::http::Http2Stream::Lease second;
    LocalAttachWaitOutcome outcome;
    if (attached) {
        second = std::move(*attached);
        (void) pending.release();
        outcome.result = fiber::common::IoErr::None;
        outcome.stream_id = second->stream_id();
    } else {
        outcome.result = attached.error();
    }
    while (!action_done) {
        co_await fiber::async::sleep(std::chrono::milliseconds(1));
    }

    outcome.wait_observed = wait_observed;
    outcome.waiter_count = connection.local_stream_attach_waiter_count_;
    outcome.granted_count = connection.local_stream_attach_granted_count_;
    outcome.next_stream_id = connection.next_local_stream_id_;
    outcome.state = connection.state();

    if (second) {
        second->close(fiber::common::IoErr::Canceled);
        connection.try_release_stream(*second);
        second.reset();
    }
    if (first) {
        if (first->attached_to_connection()) {
            first->close(fiber::common::IoErr::Canceled);
            connection.try_release_stream(*first);
        }
        first.reset();
    }
    connection.request_stop();
    co_await connection.stop_and_join();

    promise->set_value(outcome);
    fiber::event::EventLoop::current().stop();
    co_return;
}

LocalAttachWaitOutcome execute_local_attach_wait(std::uint32_t initial_limit, bool open_first_stream,
                                                 LocalAttachWakeAction action, std::chrono::milliseconds timeout) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<LocalAttachWaitOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise, initial_limit, open_first_stream, action, timeout]() mutable {
        return run_local_attach_wait(std::move(promise), initial_limit, open_first_stream, action, timeout);
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        ADD_FAILURE() << "Timed out waiting for local HTTP/2 stream attach";
        return {};
    }

    LocalAttachWaitOutcome outcome = future.get();
    group.join();
    return outcome;
}

struct LocalAttachFifoOutcome {
    std::array<fiber::common::IoErr, 2> results{fiber::common::IoErr::Unknown, fiber::common::IoErr::Unknown};
    std::array<std::uint32_t, 2> stream_ids{};
    std::array<std::size_t, 2> completion_order{};
    fiber::common::IoErr barging_result = fiber::common::IoErr::Unknown;
    std::size_t waiter_count = 0;
    std::size_t granted_count = 0;
    bool both_queued = false;
    bool first_granted_alone = false;
};

DetachedTask run_fifo_local_attach(ControlHttp2Connection *connection, std::size_t index,
                                   LocalAttachFifoOutcome *outcome,
                                   std::array<fiber::http::Http2Stream::Lease, 2> *leases, std::array<bool, 2> *done,
                                   std::size_t *completion_count) {
    std::unique_ptr<TestHttp2StreamOwner> owner(TestHttp2StreamOwner::create_owner());
    FIBER_ASSERT(owner != nullptr);

    auto attached = co_await connection->attach_local_stream(owner->stream, std::chrono::seconds(1));
    if (attached) {
        (*leases)[index] = std::move(*attached);
        (void) owner.release();
        outcome->results[index] = fiber::common::IoErr::None;
        outcome->stream_ids[index] = (*leases)[index]->stream_id();
        outcome->completion_order[index] = ++(*completion_count);
    } else {
        outcome->results[index] = attached.error();
    }
    (*done)[index] = true;
    co_return;
}

DetachedTask run_local_attach_fifo(std::shared_ptr<std::promise<LocalAttachFifoOutcome>> promise) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    auto transport =
            std::make_unique<FakeHttpTransport>(std::vector<std::string>{}, std::vector<size_t>{}, true, false);
    auto *fake_transport = transport.get();
    ControlHttp2Connection connection(std::move(transport), fake_transport, options);
    FIBER_ASSERT(connection.apply_settings_parameter(0x3, 0) == fiber::common::IoErr::None);

    LocalAttachFifoOutcome outcome;
    std::array<fiber::http::Http2Stream::Lease, 2> leases;
    std::array<bool, 2> done{};
    std::size_t completion_count = 0;
    fiber::async::spawn(
            [&]() { return run_fifo_local_attach(&connection, 0, &outcome, &leases, &done, &completion_count); });
    fiber::async::spawn(
            [&]() { return run_fifo_local_attach(&connection, 1, &outcome, &leases, &done, &completion_count); });

    for (int i = 0; i < 50 && connection.local_stream_attach_waiter_count_ != 2; ++i) {
        co_await fiber::async::sleep(std::chrono::milliseconds(1));
    }
    outcome.both_queued = connection.local_stream_attach_waiter_count_ == 2;

    FIBER_ASSERT(connection.apply_settings_parameter(0x3, 1) == fiber::common::IoErr::None);
    std::unique_ptr<TestHttp2StreamOwner> barging_owner(TestHttp2StreamOwner::create_owner());
    FIBER_ASSERT(barging_owner != nullptr);
    auto barging = connection.try_attach_local_stream(barging_owner->stream);
    outcome.barging_result = barging ? fiber::common::IoErr::None : barging.error();
    if (barging) {
        (void) barging_owner.release();
        barging->get()->close(fiber::common::IoErr::Canceled);
        connection.try_release_stream(*barging->get());
        barging->reset();
    }
    for (int i = 0; i < 50 && !done[0] && !done[1]; ++i) {
        co_await fiber::async::sleep(std::chrono::milliseconds(1));
    }
    outcome.first_granted_alone = done[0] && !done[1] && connection.local_stream_attach_waiter_count_ == 1;

    if (leases[0]) {
        leases[0]->close(fiber::common::IoErr::Canceled);
        connection.try_release_stream(*leases[0]);
        leases[0].reset();
    }
    for (int i = 0; i < 50 && (!done[0] || !done[1]); ++i) {
        co_await fiber::async::sleep(std::chrono::milliseconds(1));
    }

    if (leases[1]) {
        leases[1]->close(fiber::common::IoErr::Canceled);
        connection.try_release_stream(*leases[1]);
        leases[1].reset();
    }
    connection.request_stop();
    co_await connection.stop_and_join();
    while (!done[0] || !done[1]) {
        co_await fiber::async::sleep(std::chrono::milliseconds(1));
    }

    outcome.waiter_count = connection.local_stream_attach_waiter_count_;
    outcome.granted_count = connection.local_stream_attach_granted_count_;
    promise->set_value(outcome);
    fiber::event::EventLoop::current().stop();
    co_return;
}

LocalAttachFifoOutcome execute_local_attach_fifo() {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<LocalAttachFifoOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise]() mutable { return run_local_attach_fifo(std::move(promise)); });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        ADD_FAILURE() << "Timed out waiting for FIFO local HTTP/2 stream attaches";
        return {};
    }

    LocalAttachFifoOutcome outcome = future.get();
    group.join();
    return outcome;
}

struct ClientExchangeAttachWaitOutcome {
    fiber::common::IoResult<void> first_result;
    fiber::common::IoResult<void> second_result;
    std::uint32_t first_stream_id = 0;
    std::uint32_t second_stream_id = 0;
    std::uint32_t next_stream_id = 0;
    std::size_t waiter_count = 0;
    bool wait_observed = false;
};

DetachedTask release_client_exchange_stream(ControlHttp2Connection *connection,
                                            fiber::http::ClientHttp2Exchange *exchange, bool *wait_observed,
                                            bool *action_done) {
    co_await fiber::async::sleep(std::chrono::milliseconds(2));
    *wait_observed = connection->local_stream_attach_waiter_count_ == 1;
    (void) exchange->abort();
    *action_done = true;
    co_return;
}

DetachedTask run_client_exchange_attach_wait(std::shared_ptr<std::promise<ClientExchangeAttachWaitOutcome>> promise,
                                             bool release_first, std::chrono::milliseconds timeout) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.max_peer_concurrent_streams = 1;
    auto transport =
            std::make_unique<FakeHttpTransport>(std::vector<std::string>{}, std::vector<size_t>{}, true, false);
    auto *fake_transport = transport.get();
    ControlHttp2Connection connection(std::move(transport), fake_transport, options);

    ClientExchangeAttachWaitOutcome outcome;
    fiber::mem::BufPool first_pool;
    fiber::http::ClientHttp2Exchange first_exchange(connection, first_pool);
    outcome.first_result = co_await first_exchange.send_request_header(
            {
                    .method = fiber::http::HttpMethod::Get,
                    .scheme = "https",
                    .authority = "example.com",
                    .path = "/first",
            },
            true, std::chrono::seconds(1));
    outcome.first_stream_id = first_exchange.stream_id();

    bool action_done = !release_first;
    if (release_first) {
        fiber::async::spawn([&]() {
            return release_client_exchange_stream(&connection, &first_exchange, &outcome.wait_observed, &action_done);
        });
    }

    fiber::mem::BufPool second_pool;
    fiber::http::ClientHttp2Exchange second_exchange(connection, second_pool);
    outcome.second_result = co_await second_exchange.send_request_header(
            {
                    .method = fiber::http::HttpMethod::Get,
                    .scheme = "https",
                    .authority = "example.com",
                    .path = "/second",
            },
            true, timeout);
    outcome.second_stream_id = second_exchange.stream_id();
    while (!action_done) {
        co_await fiber::async::sleep(std::chrono::milliseconds(1));
    }

    outcome.next_stream_id = connection.next_local_stream_id_;
    outcome.waiter_count = connection.local_stream_attach_waiter_count_;
    if (second_exchange.stream()) {
        (void) second_exchange.abort();
    }
    if (first_exchange.stream() && first_exchange.stream()->attached_to_connection()) {
        (void) first_exchange.abort();
    }
    co_await fiber::async::sleep(std::chrono::milliseconds(2));
    connection.request_stop();
    co_await connection.stop_and_join();

    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

ClientExchangeAttachWaitOutcome execute_client_exchange_attach_wait(bool release_first,
                                                                    std::chrono::milliseconds timeout) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ClientExchangeAttachWaitOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise, release_first, timeout]() mutable {
        return run_client_exchange_attach_wait(std::move(promise), release_first, timeout);
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        ADD_FAILURE() << "Timed out waiting for ClientHttp2Exchange stream capacity";
        return {};
    }

    ClientExchangeAttachWaitOutcome outcome = future.get();
    group.join();
    return outcome;
}

DetachedTask run_keepalive_connection(std::shared_ptr<std::promise<KeepaliveRunOutcome>> promise,
                                      std::vector<ScriptedReadTransport::ReadAction> actions,
                                      fiber::http::Http2Connection::Options options = {},
                                      bool inspect_idle_release = false) {
    auto transport = std::make_unique<ScriptedReadTransport>(std::move(actions));
    auto *transport_impl = transport.get();
    KeepaliveHttp2Connection connection(std::move(transport), transport_impl, options);

    KeepaliveRunOutcome outcome;
    if (inspect_idle_release) {
        co_await fiber::async::sleep(std::chrono::milliseconds(5));
        outcome.read_buffer_released = !connection.read_buffer_allocated();
        connection.shutdown(fiber::common::IoErr::None);
    }
    outcome.result = co_await connection.wait_closed();
    outcome.written = options.role == fiber::http::Http2Connection::ConnectionRole::Client
                              ? strip_client_initial_flight(connection.written())
                              : connection.written();
    outcome.state = connection.current_state();
    outcome.transport_close_count = connection.transport_close_count();
    outcome.wait_readable_call_count = transport_impl->wait_readable_call_count();
    outcome.read_into_call_count = transport_impl->read_into_call_count();
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

KeepaliveRunOutcome execute_keepalive_connection(std::vector<ScriptedReadTransport::ReadAction> actions,
                                                 fiber::http::Http2Connection::Options options = {},
                                                 bool inspect_idle_release = false) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<KeepaliveRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise = std::move(promise), actions = std::move(actions), options,
                                      inspect_idle_release]() mutable {
        return run_keepalive_connection(std::move(promise), std::move(actions), options, inspect_idle_release);
    });

    auto status = future.wait_for(std::chrono::seconds(2));
    if (status != std::future_status::ready) {
        group.stop();
        group.join();
        ADD_FAILURE() << "Timed out waiting for http2 keepalive task";
        return {};
    }

    KeepaliveRunOutcome outcome = future.get();
    group.join();
    return outcome;
}

DetachedTask run_retained_stream_connection(std::shared_ptr<std::promise<RetainedStreamOutcome>> promise,
                                            fiber::http::Http2Connection::Options options = {}) {
    auto transport =
            std::make_unique<FakeHttpTransport>(std::vector<std::string>{}, std::vector<size_t>{}, true, false);
    auto *fake_transport = transport.get();
    std::optional<fiber::http::Http2Stream::Lease> retained_stream;
    ControlHttp2Connection connection(std::move(transport), fake_transport, options);

    fiber::async::spawn([&connection, fake_transport, &retained_stream]() -> DetachedTask {
        fiber::http::Http2Stream *stream = nullptr;
        for (int i = 0; i < 20 && !stream; ++i) {
            stream = connection.open_stream(1);
            if (!stream) {
                co_await fiber::async::sleep(std::chrono::milliseconds(1));
            }
        }
        if (stream) {
            retained_stream.emplace(stream->lease());
            connection.request_stop();
        } else {
            connection.request_stop(fiber::common::IoErr::Invalid);
        }
        fake_transport->release_reads();
    });

    RetainedStreamOutcome outcome;
    outcome.result = co_await connection.wait_closed();
    co_await fiber::async::sleep(std::chrono::milliseconds(1));

    outcome.stream_opened = retained_stream.has_value();
    outcome.stream_registered_after_run = connection.current_has_stream(1);
    if (retained_stream) {
        outcome.lease_valid_after_run = static_cast<bool>(*retained_stream);
        outcome.attached_after_run = retained_stream->get()->attached_to_connection();
        outcome.close_reason = retained_stream->get()->close_reason();
    }

    co_await connection.stop_and_join();
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

RetainedStreamOutcome execute_retained_stream_connection(fiber::http::Http2Connection::Options options = {}) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<RetainedStreamOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise = std::move(promise), options]() mutable {
        return run_retained_stream_connection(std::move(promise), options);
    });

    auto status = future.wait_for(std::chrono::seconds(2));
    if (status != std::future_status::ready) {
        group.stop();
        group.join();
        ADD_FAILURE() << "Timed out waiting for retained stream task";
        return {};
    }

    RetainedStreamOutcome outcome = future.get();
    group.join();
    return outcome;
}

} // namespace

TEST(Http2ConnectionTest, ReportsPayloadChunksWithFrameOffsets) {
    constexpr std::string_view preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    std::string data = make_frame(11, 0xA, 0x1, 1, "hello world");

    std::vector<std::string> chunks = {
            std::string(preface),
            data.substr(0, 12),
            data.substr(12, 4),
            data.substr(16),
    };

    RunOutcome outcome = execute_connection(std::move(chunks));

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_EQ(outcome.chunks.size(), 3U);
    EXPECT_EQ(outcome.chunks[0].header.length, 11U);
    EXPECT_EQ(outcome.chunks[0].offset, 0U);
    EXPECT_EQ(outcome.chunks[0].length, 3U);
    EXPECT_EQ(iobuf_to_string(outcome.chunks[0].payload), "hel");
    EXPECT_EQ(outcome.chunks[1].offset, 3U);
    EXPECT_EQ(outcome.chunks[1].length, 4U);
    EXPECT_EQ(iobuf_to_string(outcome.chunks[1].payload), "lo w");
    EXPECT_EQ(outcome.chunks[2].offset, 7U);
    EXPECT_EQ(outcome.chunks[2].length, 4U);
    EXPECT_EQ(iobuf_to_string(outcome.chunks[2].payload), "orld");
}

TEST(Http2ConnectionTest, AllowsClientsToParseFramesWithoutPeerPreface) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    std::string data = make_frame(4, 0xA, 0x1, 1, "pong");
    RunOutcome outcome = execute_connection({data}, options);

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_EQ(outcome.chunks.size(), 1U);
    EXPECT_EQ(static_cast<std::uint8_t>(outcome.chunks[0].header.type), 0xAU);
    EXPECT_EQ(outcome.chunks[0].offset, 0U);
    EXPECT_EQ(outcome.chunks[0].length, 4U);
    EXPECT_EQ(iobuf_to_string(outcome.chunks[0].payload), "pong");
}

TEST(Http2ConnectionTest, StartDrivesIoAndNotifiesClosureWithoutRunCoroutine) {
    struct CallbackContext {
        std::promise<fiber::common::IoResult<void>> *promise = nullptr;
        fiber::http::Http2Connection::State state = fiber::http::Http2Connection::State::Init;
    };

    fiber::event::EventLoopGroup group(1);
    std::promise<fiber::common::IoResult<void>> promise;
    auto future = promise.get_future();
    CallbackContext callback_ctx{&promise};
    group.start();
    fiber::async::spawn(group.at(0), [&callback_ctx]() -> fiber::async::DetachedTask {
        fiber::http::Http2Connection::Options options;
        options.role = fiber::http::Http2Connection::ConnectionRole::Client;
        auto *connection = new (std::nothrow)
                fiber::http::Http2Connection(options, &test_http2_stream_factory(), TestHttp2StreamFactory::ops());
        if (!connection) {
            callback_ctx.promise->set_value(std::unexpected(fiber::common::IoErr::NoMem));
            fiber::event::EventLoop::current().stop();
            co_return;
        }

        auto on_closed = [](void *ctx, fiber::http::Http2Connection &closed_connection,
                            fiber::http::Http2Connection::CloseResult result) noexcept {
            auto *callback = static_cast<CallbackContext *>(ctx);
            auto &event_loop = closed_connection.loop();
            callback->state = closed_connection.state();
            callback->promise->set_value(std::move(result));
            delete &closed_connection;
            event_loop.stop();
        };
        auto transport = std::make_unique<FakeHttpTransport>(std::vector<std::string>{});
        fiber::common::IoErr err = connection->start(std::move(transport), on_closed, &callback_ctx);
        if (err != fiber::common::IoErr::None) {
            delete connection;
            callback_ctx.promise->set_value(std::unexpected(err));
            fiber::event::EventLoop::current().stop();
        }
        co_return;
    });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_TRUE(future.get().has_value());
    group.join();
    EXPECT_EQ(callback_ctx.state, fiber::http::Http2Connection::State::Closed);
}

TEST(Http2ConnectionTest, ReadinessCallbackDrainsTransportUntilWouldBlock) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<RunOutcome>>();
    auto future = promise->get_future();
    group.start();
    fiber::async::spawn(group.at(0), [promise]() -> fiber::async::DetachedTask {
        fiber::http::Http2Connection::Options options;
        options.role = fiber::http::Http2Connection::ConnectionRole::Client;
        std::vector<std::string> chunks{
                make_frame(3, 0xA, 0x0, 1, "one"),
                make_frame(3, 0xA, 0x0, 1, "two"),
        };
        auto transport =
                std::make_unique<FakeHttpTransport>(std::move(chunks), std::vector<size_t>{}, true, true, false);
        FakeHttpTransport *transport_impl = transport.get();
        RecordingHttp2Connection connection(std::move(transport), options);

        co_await fiber::async::sleep(std::chrono::milliseconds(1));
        transport_impl->release_reads();

        RunOutcome outcome;
        outcome.chunks = connection.chunks();
        outcome.read_into_call_count = transport_impl->read_into_call_count();
        connection.shutdown();
        outcome.result = co_await connection.wait_closed();
        promise->set_value(std::move(outcome));
        fiber::event::EventLoop::current().stop();
    });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    RunOutcome outcome = future.get();
    group.join();
    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_EQ(outcome.chunks.size(), 2U);
    EXPECT_EQ(iobuf_to_string(outcome.chunks[0].payload), "one");
    EXPECT_EQ(iobuf_to_string(outcome.chunks[1].payload), "two");
    EXPECT_EQ(outcome.read_into_call_count, 3U);
}

TEST(Http2ConnectionTest, ReportsZeroLengthFrames) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    std::string data = make_frame(0, 0xA, 0x0, 1, "");
    RunOutcome outcome = execute_connection({data}, options);

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_EQ(outcome.chunks.size(), 1U);
    EXPECT_EQ(static_cast<std::uint8_t>(outcome.chunks[0].header.type), 0xAU);
    EXPECT_EQ(outcome.chunks[0].offset, 0U);
    EXPECT_EQ(outcome.chunks[0].length, 0U);
    EXPECT_EQ(outcome.chunks[0].payload.readable(), 0U);
    EXPECT_EQ(outcome.wait_readable_call_count, 0U);
    EXPECT_EQ(outcome.read_into_call_count, 2U);
}

TEST(Http2ConnectionTest, ReschedulesAfterParsingExactlyOneOperationBudget) {
    std::string input(kClientConnectionPreface);
    for (std::size_t i = 0; i < 62; ++i) {
        input += make_frame(0, 0xA, 0x0, 1, {});
    }

    RunOutcome outcome = execute_connection({std::move(input)});

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_EQ(outcome.chunks.size(), 62U);
}

TEST(Http2ConnectionTest, ReallocatesReadBufferWhenPayloadIsRetained) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    std::string first = make_frame(3, 0xA, 0x0, 1, "abc");
    std::string second = make_frame(4, 0xA, 0x0, 1, "wxyz");

    RunOutcome outcome = execute_connection({first, second}, options);

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_EQ(outcome.chunks.size(), 2U);
    EXPECT_EQ(iobuf_to_string(outcome.chunks[0].payload), "abc");
    EXPECT_EQ(iobuf_to_string(outcome.chunks[1].payload), "wxyz");
    EXPECT_EQ(outcome.wait_readable_call_count, 0U);
    EXPECT_EQ(outcome.read_into_call_count, 3U);
}

TEST(Http2ConnectionTest, RejectsPrefaceMismatch) {
    std::vector<std::string> chunks = {
            "PRI * HTTP/1.1\r\n\r\nSM\r\n\r\n",
    };

    RunOutcome outcome = execute_connection(std::move(chunks));

    ASSERT_FALSE(outcome.result.has_value());
    EXPECT_EQ(outcome.result.error(), fiber::common::IoErr::Invalid);
    EXPECT_TRUE(outcome.chunks.empty());
}

TEST(Http2ConnectionTest, RejectsFramesLargerThanConfiguredMax) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.max_frame_size = 3;

    std::string frame = make_frame(4, 0xA, 0x0, 1, "data");
    RunOutcome outcome = execute_connection({frame}, options);

    ASSERT_FALSE(outcome.result.has_value());
    EXPECT_EQ(outcome.result.error(), fiber::common::IoErr::Invalid);
    EXPECT_TRUE(outcome.chunks.empty());
}

TEST(Http2ConnectionTest, PropagatesPayloadCallbackErrors) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    std::string frame = make_frame(4, 0xA, 0x0, 1, "data");
    RunOutcome outcome = execute_connection({frame}, options, fiber::common::IoErr::Busy);

    ASSERT_FALSE(outcome.result.has_value());
    EXPECT_EQ(outcome.result.error(), fiber::common::IoErr::Busy);
    ASSERT_EQ(outcome.chunks.size(), 1U);
    EXPECT_EQ(iobuf_to_string(outcome.chunks[0].payload), "data");
}

TEST(Http2ConnectionTest, SettingsFrameUpdatesPeerStateAndSendsAck) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.initial_connection_recv_window = 65535;

    std::string payload;
    payload.push_back('\0');
    payload.push_back('\x4');
    payload.push_back('\0');
    payload.push_back('\x01');
    payload.push_back('\x11');
    payload.push_back('\x70');
    payload.push_back('\0');
    payload.push_back('\x5');
    payload.push_back('\0');
    payload.push_back('\0');
    payload.push_back('\x80');
    payload.push_back('\0');
    std::string settings = make_frame(static_cast<std::uint32_t>(payload.size()), 0x4, 0x0, 0, payload);

    ControlRunOutcome outcome = execute_control_connection(
            {settings}, [](ControlHttp2Connection &, fiber::http::Http2Stream *, fiber::http::Http2Stream *) {},
            options, true, true, true);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_EQ(outcome.peer_max_frame_size, 32768U);
    EXPECT_EQ(outcome.stream1_send_window, 70000);
    EXPECT_EQ(outcome.stream3_send_window, 70000);
    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    ASSERT_EQ(frames.size(), 1U) << describe_frames(frames);
    EXPECT_EQ(frames[0].type, 0x4);
    EXPECT_EQ(frames[0].flags, 0x1);
    EXPECT_EQ(frames[0].stream_id, 0U);
    EXPECT_TRUE(frames[0].payload.empty());
}

TEST(Http2ConnectionTest, InitialStreamWindowOverflowIsConnectionFatal) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    std::string payload("\0\x04\x7f\xff\xff\xff", 6);
    ControlRunOutcome outcome = execute_control_connection(
            {make_frame(6, 0x4, 0x0, 0, payload)},
            [](ControlHttp2Connection &, fiber::http::Http2Stream *stream1, fiber::http::Http2Stream *) {
                ASSERT_NE(stream1, nullptr);
                stream1->send_window_ = 0x7fffffff;
            },
            options);

    ASSERT_FALSE(outcome.result.has_value());
    EXPECT_EQ(outcome.result.error(), fiber::common::IoErr::Invalid);
}

TEST(Http2ConnectionTest, PingFrameRepliesWithAckAndSamePayload) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    std::string ping = make_frame(8, 0x6, 0x0, 0, "12345678");
    ControlRunOutcome outcome = execute_control_connection({ping}, {}, options);

    ASSERT_TRUE(outcome.result.has_value());
    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    ASSERT_EQ(frames.size(), 1U) << describe_frames(frames);
    EXPECT_EQ(frames[0].type, 0x6);
    EXPECT_EQ(frames[0].flags, 0x1);
    EXPECT_EQ(frames[0].stream_id, 0U);
    EXPECT_EQ(frames[0].payload, "12345678");
}

TEST(Http2ConnectionTest, ReadTimeoutSendsKeepalivePingAndAckClearsOutstanding) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.keepalive_ping_interval = std::chrono::milliseconds(1);
    options.read_timeout = std::chrono::milliseconds(10);

    std::string keepalive_payload(8, '\0');
    keepalive_payload[7] = '\x01';
    std::string ack = make_frame(8, 0x6, 0x1, 0, keepalive_payload);

    KeepaliveRunOutcome outcome = execute_keepalive_connection(
            {
                    {ScriptedReadTransport::ReadActionKind::TimedOut, {}},
                    {ScriptedReadTransport::ReadActionKind::Chunk, ack},
                    {ScriptedReadTransport::ReadActionKind::Eof, {}},
            },
            options);

    ASSERT_TRUE(outcome.result.has_value());
    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    ASSERT_EQ(frames.size(), 1U) << describe_frames(frames);
    EXPECT_EQ(frames[0].type, 0x6);
    EXPECT_EQ(frames[0].flags, 0x0);
    EXPECT_EQ(frames[0].stream_id, 0U);
    EXPECT_EQ(frames[0].payload, keepalive_payload);
    EXPECT_EQ(outcome.wait_readable_call_count, 0U);
    EXPECT_EQ(outcome.read_into_call_count, 3U);
}

TEST(Http2ConnectionTest, SecondReadTimeoutWithoutPingAckClosesConnection) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.keepalive_ping_interval = std::chrono::milliseconds(1);
    options.read_timeout = std::chrono::milliseconds(10);

    KeepaliveRunOutcome outcome = execute_keepalive_connection(
            {
                    {ScriptedReadTransport::ReadActionKind::TimedOut, {}},
                    {ScriptedReadTransport::ReadActionKind::TimedOut, {}},
            },
            options);

    ASSERT_FALSE(outcome.result.has_value());
    EXPECT_EQ(outcome.result.error(), fiber::common::IoErr::TimedOut);
    EXPECT_EQ(outcome.wait_readable_call_count, 0U);
    EXPECT_EQ(outcome.read_into_call_count, 2U);
}

TEST(Http2ConnectionTest, IdleReadBufferReleaseTimeoutDoesNotCloseConnection) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.read_timeout = std::chrono::milliseconds(100);
    options.read_buffer_idle_release_timeout = std::chrono::milliseconds(1);

    std::string frame = make_frame(0, 0xA, 0x0, 1, {});
    KeepaliveRunOutcome outcome = execute_keepalive_connection(
            {
                    {ScriptedReadTransport::ReadActionKind::Chunk, std::move(frame)},
                    {ScriptedReadTransport::ReadActionKind::TimedOut, {}},
            },
            options, true);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_TRUE(outcome.read_buffer_released);
    EXPECT_EQ(outcome.wait_readable_call_count, 0U);
    EXPECT_EQ(outcome.read_into_call_count, 2U);
}

TEST(Http2ConnectionTest, PartialFrameIsNotReleasedByIdleReadBufferTimeout) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.read_timeout = std::chrono::milliseconds(100);
    options.read_buffer_idle_release_timeout = std::chrono::milliseconds(1);

    std::string partial_frame = make_frame(0, 0xA, 0x0, 1, {}).substr(0, 1);
    KeepaliveRunOutcome outcome = execute_keepalive_connection(
            {
                    {ScriptedReadTransport::ReadActionKind::Chunk, std::move(partial_frame)},
                    {ScriptedReadTransport::ReadActionKind::TimedOut, {}},
            },
            options);

    ASSERT_FALSE(outcome.result.has_value());
    EXPECT_EQ(outcome.result.error(), fiber::common::IoErr::TimedOut);
    EXPECT_EQ(outcome.wait_readable_call_count, 0U);
    EXPECT_EQ(outcome.read_into_call_count, 2U);
}

TEST(Http2ConnectionTest, WindowUpdateIncreasesConnectionAndStreamSendWindow) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.initial_connection_recv_window = 65535;

    std::string stream_update_payload;
    stream_update_payload.push_back('\0');
    stream_update_payload.push_back('\0');
    stream_update_payload.push_back('\0');
    stream_update_payload.push_back('\x64');
    std::string conn_update_payload;
    conn_update_payload.push_back('\0');
    conn_update_payload.push_back('\0');
    conn_update_payload.push_back('\0');
    conn_update_payload.push_back('\x32');

    ControlRunOutcome outcome = execute_control_connection(
            {make_frame(4, 0x8, 0x0, 1, stream_update_payload), make_frame(4, 0x8, 0x0, 0, conn_update_payload)},
            [](ControlHttp2Connection &, fiber::http::Http2Stream *, fiber::http::Http2Stream *) {}, options, true,
            true, true);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_EQ(outcome.stream1_send_window, 65635);
    EXPECT_EQ(outcome.conn_send_window, 65585);
}

TEST(Http2ConnectionTest, ZeroIncrementWindowUpdateOnStreamSendsRstStream) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    std::string payload(4, '\0');
    ControlRunOutcome outcome = execute_control_connection({make_frame(4, 0x8, 0x0, 1, payload)}, {}, options);

    ASSERT_TRUE(outcome.result.has_value());
    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    ASSERT_EQ(frames.size(), 1U) << describe_frames(frames);
    EXPECT_EQ(frames[0].type, 0x3);
    EXPECT_EQ(frames[0].stream_id, 1U);
    ASSERT_EQ(frames[0].payload.size(), 4U);
    EXPECT_EQ(static_cast<unsigned char>(frames[0].payload[3]), 0x1U);
}

TEST(Http2ConnectionTest, RstStreamClosesActiveStream) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.initial_connection_recv_window = 65535;

    std::string payload(4, '\0');
    payload[3] = '\x8';
    ControlRunOutcome outcome = execute_control_connection(
            {make_frame(4, 0x3, 0x0, 1, payload)},
            [](ControlHttp2Connection &, fiber::http::Http2Stream *, fiber::http::Http2Stream *) {}, options, true,
            true, true);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_FALSE(outcome.stream1_registered);
}

TEST(Http2ConnectionTest, HeadersCreatePeerStreamAndOpenIt) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    ControlRunOutcome outcome = execute_control_connection({make_frame(1, 0x1, 0x4, 2, std::string("\x82", 1))}, {},
                                                           options, false, true, true);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_TRUE(outcome.stream2_registered);
    EXPECT_FALSE(outcome.stream2_remote_end_stream);
}

TEST(Http2ConnectionTest, HeadersWithContinuationCompleteExistingLocalStreamHeaders) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    ControlRunOutcome outcome = execute_control_connection(
            {make_frame(1, 0x1, 0x0, 1, std::string("\x82", 1)), make_frame(1, 0x9, 0x4, 1, std::string("\x84", 1))},
            [](ControlHttp2Connection &, fiber::http::Http2Stream *, fiber::http::Http2Stream *) {}, options, false,
            true, true);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_TRUE(outcome.stream1_registered);
    EXPECT_FALSE(outcome.stream1_remote_end_stream);
}

TEST(Http2ConnectionTest, HeadersWithEndStreamCreateHalfClosedRemoteStream) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    ControlRunOutcome outcome =
            execute_control_connection({make_frame(0, 0x1, 0x5, 2, "")}, {}, options, false, true, true);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_TRUE(outcome.stream2_registered);
    EXPECT_TRUE(outcome.stream2_remote_end_stream);
}

TEST(Http2ConnectionTest, HeadersWithEndStreamContinuationCloseRemoteStreamAfterBlockComplete) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    ControlRunOutcome outcome = execute_control_connection(
            {make_frame(1, 0x1, 0x1, 2, std::string("\x82", 1)), make_frame(1, 0x9, 0x4, 2, std::string("\x84", 1))},
            {}, options, false, true, true);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_TRUE(outcome.stream2_registered);
    EXPECT_TRUE(outcome.stream2_remote_end_stream);
}

TEST(Http2ConnectionTest, TrailerHeadersRequireEndStream) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    ControlRunOutcome outcome = execute_control_connection(
            {make_frame(1, 0x1, 0x4, 2, std::string("\x82", 1)), make_frame(1, 0x1, 0x4, 2, std::string("\x84", 1))},
            {}, options);

    ASSERT_TRUE(outcome.result.has_value());
}

TEST(Http2ConnectionTest, TrailerHeadersWithContinuationCloseRemoteStream) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    ControlRunOutcome outcome = execute_control_connection({make_frame(1, 0x1, 0x4, 2, std::string("\x82", 1)),
                                                            make_frame(1, 0x1, 0x1, 2, std::string("\x84", 1)),
                                                            make_frame(1, 0x9, 0x4, 2, std::string("\x86", 1))},
                                                           {}, options, false, true, true);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_TRUE(outcome.stream2_registered);
    EXPECT_TRUE(outcome.stream2_remote_end_stream);
}

TEST(Http2ConnectionTest, GoawayClosesOnlyLocalStreamsAfterLastStreamId) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.initial_connection_recv_window = 65535;

    std::string payload(8, '\0');
    payload[3] = '\x1';
    ControlRunOutcome outcome = execute_control_connection(
            {make_frame(8, 0x7, 0x0, 0, payload)},
            [](ControlHttp2Connection &, fiber::http::Http2Stream *, fiber::http::Http2Stream *) {}, options, true,
            true, true);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_TRUE(outcome.stream1_registered);
    EXPECT_FALSE(outcome.stream1_remote_end_stream);
    EXPECT_FALSE(outcome.stream1_remote_rst);
    EXPECT_FALSE(outcome.stream3_registered);

    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    ASSERT_EQ(frames.size(), 1U) << describe_frames(frames);
    EXPECT_EQ(frames[0].type, 0x7);
    EXPECT_EQ(frames[0].stream_id, 0U);
    ASSERT_EQ(frames[0].payload.size(), 8U);
    EXPECT_EQ(static_cast<unsigned char>(frames[0].payload[3]), 0x0U);
    EXPECT_EQ(outcome.state, fiber::http::Http2Connection::State::Draining);
    EXPECT_EQ(outcome.transport_close_count, 0U);
}

TEST(Http2ConnectionTest, CloseAllStreamsTraversesOwnedListWhileDetaching) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    fiber::http::Http2Connection connection(options, &test_http2_stream_factory(), TestHttp2StreamFactory::ops());
    connection.state_ = fiber::http::Http2Connection::State::Running;

    auto *owner1 = TestHttp2StreamOwner::create_owner();
    auto *owner3 = TestHttp2StreamOwner::create_owner();
    auto *owner5 = TestHttp2StreamOwner::create_owner();
    ASSERT_NE(owner1, nullptr);
    ASSERT_NE(owner3, nullptr);
    ASSERT_NE(owner5, nullptr);

    auto stream1 = connection.try_attach_local_stream(owner1->stream);
    auto stream3 = connection.try_attach_local_stream(owner3->stream);
    auto stream5 = connection.try_attach_local_stream(owner5->stream);
    ASSERT_TRUE(stream1.has_value());
    ASSERT_TRUE(stream3.has_value());
    ASSERT_TRUE(stream5.has_value());

    connection.close_all_streams(fiber::common::IoErr::NoMem);

    EXPECT_TRUE(connection.streams_.empty());
    EXPECT_TRUE(connection.owned_stream_list_.empty());
    EXPECT_EQ((*stream1)->close_reason(), fiber::common::IoErr::NoMem);
    EXPECT_EQ((*stream3)->close_reason(), fiber::common::IoErr::NoMem);
    EXPECT_EQ((*stream5)->close_reason(), fiber::common::IoErr::NoMem);
    EXPECT_FALSE((*stream1)->attached_to_connection());
    EXPECT_FALSE((*stream3)->attached_to_connection());
    EXPECT_FALSE((*stream5)->attached_to_connection());
}

TEST(Http2ConnectionTest, CloseStreamsAfterGoawayDoesNotSkipAdjacentOwnedStreams) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    fiber::http::Http2Connection connection(options, &test_http2_stream_factory(), TestHttp2StreamFactory::ops());
    connection.state_ = fiber::http::Http2Connection::State::Running;

    auto *owner1 = TestHttp2StreamOwner::create_owner();
    auto *owner3 = TestHttp2StreamOwner::create_owner();
    auto *owner5 = TestHttp2StreamOwner::create_owner();
    ASSERT_NE(owner1, nullptr);
    ASSERT_NE(owner3, nullptr);
    ASSERT_NE(owner5, nullptr);

    auto stream1 = connection.try_attach_local_stream(owner1->stream);
    auto stream3 = connection.try_attach_local_stream(owner3->stream);
    auto stream5 = connection.try_attach_local_stream(owner5->stream);
    ASSERT_TRUE(stream1.has_value());
    ASSERT_TRUE(stream3.has_value());
    ASSERT_TRUE(stream5.has_value());

    connection.close_streams_after_goaway(1);

    EXPECT_NE(connection.streams_.find(1), nullptr);
    EXPECT_EQ(connection.streams_.find(3), nullptr);
    EXPECT_EQ(connection.streams_.find(5), nullptr);
    EXPECT_EQ((*stream1)->close_reason(), fiber::common::IoErr::None);
    EXPECT_EQ((*stream3)->close_reason(), fiber::common::IoErr::Canceled);
    EXPECT_EQ((*stream5)->close_reason(), fiber::common::IoErr::Canceled);

    connection.close_all_streams(fiber::common::IoErr::Canceled);
}

TEST(Http2ConnectionTest, InitialStreamWindowDecreaseUpdatesEveryStream) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    fiber::http::Http2Connection connection(options, &test_http2_stream_factory(), TestHttp2StreamFactory::ops());
    connection.state_ = fiber::http::Http2Connection::State::Running;

    auto *owner1 = TestHttp2StreamOwner::create_owner();
    auto *owner3 = TestHttp2StreamOwner::create_owner();
    ASSERT_NE(owner1, nullptr);
    ASSERT_NE(owner3, nullptr);

    auto stream1 = connection.try_attach_local_stream(owner1->stream);
    auto stream3 = connection.try_attach_local_stream(owner3->stream);
    ASSERT_TRUE(stream1.has_value());
    ASSERT_TRUE(stream3.has_value());
    (*stream3)->send_window_ = 1000;

    EXPECT_EQ(connection.apply_peer_initial_stream_window(32768), fiber::common::IoErr::None);
    EXPECT_EQ((*stream1)->send_window(), 32768);
    EXPECT_EQ((*stream3)->send_window(), -31767);
    EXPECT_EQ(connection.peer_initial_stream_send_window_, 32768);

    connection.close_all_streams(fiber::common::IoErr::Canceled);
}

TEST(Http2ConnectionTest, RejectsInvalidPingLengthAsConnectionError) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    ControlRunOutcome outcome = execute_control_connection({make_frame(4, 0x6, 0x0, 0, "pong")}, {}, options);

    ASSERT_FALSE(outcome.result.has_value());
    EXPECT_EQ(outcome.result.error(), fiber::common::IoErr::Invalid);
}

TEST(Http2ConnectionTest, ClientConnectionPrefaceSendsPrefaceSettingsAndWindowUpdate) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.max_frame_size = 0x00ffffffU;
    options.local_max_concurrent_streams = 128;
    options.initial_stream_send_window = 65535;
    options.initial_connection_recv_window = 0x7fffffffU;

    ControlRunOutcome outcome = execute_control_connection({}, {}, options, true);

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_GE(outcome.written.size(), kClientConnectionPreface.size());
    EXPECT_EQ(outcome.written.substr(0, kClientConnectionPreface.size()), kClientConnectionPreface);

    std::string_view frames_view(outcome.written.data() + kClientConnectionPreface.size(),
                                 outcome.written.size() - kClientConnectionPreface.size());
    std::vector<EncodedFrame> frames = parse_frames(frames_view);
    ASSERT_EQ(frames.size(), 2U) << describe_frames(frames);
    EXPECT_EQ(frames[0].type, 0x4);
    EXPECT_EQ(frames[0].flags, 0x0);
    EXPECT_EQ(frames[0].stream_id, 0U);
    EXPECT_EQ(frames[0].length, 18U);
    ASSERT_TRUE(parse_settings_parameter(frames[0], 0x3).has_value());
    EXPECT_EQ(*parse_settings_parameter(frames[0], 0x3), 128U);
    EXPECT_EQ(frames[1].type, 0x8);
    EXPECT_EQ(frames[1].stream_id, 0U);
    EXPECT_EQ(frames[1].length, 4U);

    ASSERT_EQ(frames[1].payload.size(), 4U);
    std::uint32_t increment =
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(frames[1].payload[0]) & 0x7fU) << 24) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(frames[1].payload[1])) << 16) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(frames[1].payload[2])) << 8) |
            static_cast<std::uint32_t>(static_cast<std::uint8_t>(frames[1].payload[3]));
    EXPECT_EQ(increment, 0x7fffffffU - 65535U);
}

TEST(Http2ConnectionTest, ServerConnectionPrefaceSendsSettingsAndWindowUpdateAfterPeerPreface) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Server;
    options.max_frame_size = 0x00ffffffU;
    options.local_max_concurrent_streams = 1;
    options.initial_stream_send_window = 65535;
    options.initial_connection_recv_window = 0x7fffffffU;

    ControlRunOutcome outcome = execute_control_connection({std::string(kClientConnectionPreface)}, {}, options, true);

    ASSERT_TRUE(outcome.result.has_value());
    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    ASSERT_EQ(frames.size(), 2U) << describe_frames(frames);
    EXPECT_EQ(frames[0].type, 0x4);
    EXPECT_EQ(frames[0].flags, 0x0);
    EXPECT_EQ(frames[0].stream_id, 0U);
    EXPECT_EQ(frames[0].length, 18U);
    ASSERT_TRUE(parse_settings_parameter(frames[0], 0x3).has_value());
    EXPECT_EQ(*parse_settings_parameter(frames[0], 0x3), 1U);
    EXPECT_EQ(frames[1].type, 0x8);
    EXPECT_EQ(frames[1].stream_id, 0U);
    EXPECT_EQ(frames[1].length, 4U);
}

TEST(Http2ConnectionTest, ClientLocalStreamsRespectPeerAdvertisedConcurrentLimit) {
    fiber::http::Http2Connection::Options options;
    options.max_peer_concurrent_streams = 100;

    ClientConcurrentLimitOutcome outcome = execute_client_concurrent_limit(options);

    EXPECT_TRUE(outcome.first_opened);
    EXPECT_EQ(outcome.second_open_error, fiber::common::IoErr::Busy);
    EXPECT_TRUE(outcome.third_opened);
}

TEST(Http2ConnectionTest, AwaitedLocalStreamAttachResumesAfterActiveStreamDetaches) {
    LocalAttachWaitOutcome outcome =
            execute_local_attach_wait(1, true, LocalAttachWakeAction::CloseFirstStream, std::chrono::seconds(1));

    EXPECT_TRUE(outcome.wait_observed);
    EXPECT_EQ(outcome.result, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.stream_id, 3U);
    EXPECT_EQ(outcome.next_stream_id, 5U);
    EXPECT_EQ(outcome.waiter_count, 0U);
    EXPECT_EQ(outcome.granted_count, 0U);
}

TEST(Http2ConnectionTest, AwaitedLocalStreamAttachResumesAfterPeerIncreasesLimit) {
    LocalAttachWaitOutcome outcome =
            execute_local_attach_wait(0, false, LocalAttachWakeAction::IncreaseLimit, std::chrono::seconds(1));

    EXPECT_TRUE(outcome.wait_observed);
    EXPECT_EQ(outcome.result, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.stream_id, 1U);
    EXPECT_EQ(outcome.next_stream_id, 3U);
    EXPECT_EQ(outcome.waiter_count, 0U);
    EXPECT_EQ(outcome.granted_count, 0U);
}

TEST(Http2ConnectionTest, AwaitedLocalStreamAttachTimesOutWithoutConsumingStreamId) {
    LocalAttachWaitOutcome outcome =
            execute_local_attach_wait(0, false, LocalAttachWakeAction::None, std::chrono::milliseconds(5));

    EXPECT_EQ(outcome.result, fiber::common::IoErr::TimedOut);
    EXPECT_EQ(outcome.stream_id, 0U);
    EXPECT_EQ(outcome.next_stream_id, 1U);
    EXPECT_EQ(outcome.waiter_count, 0U);
    EXPECT_EQ(outcome.granted_count, 0U);
}

TEST(Http2ConnectionTest, AwaitedLocalStreamAttachIsCanceledByShutdown) {
    LocalAttachWaitOutcome outcome =
            execute_local_attach_wait(0, false, LocalAttachWakeAction::Shutdown, std::chrono::seconds(1));

    EXPECT_TRUE(outcome.wait_observed);
    EXPECT_EQ(outcome.result, fiber::common::IoErr::Canceled);
    EXPECT_EQ(outcome.stream_id, 0U);
    EXPECT_EQ(outcome.next_stream_id, 1U);
    EXPECT_EQ(outcome.waiter_count, 0U);
    EXPECT_EQ(outcome.granted_count, 0U);
}

TEST(Http2ConnectionTest, AwaitedLocalStreamAttachIsCanceledByPeerGoaway) {
    LocalAttachWaitOutcome outcome =
            execute_local_attach_wait(0, false, LocalAttachWakeAction::PeerGoaway, std::chrono::seconds(1));

    EXPECT_TRUE(outcome.wait_observed);
    EXPECT_EQ(outcome.result, fiber::common::IoErr::Canceled);
    EXPECT_EQ(outcome.stream_id, 0U);
    EXPECT_EQ(outcome.next_stream_id, 1U);
    EXPECT_EQ(outcome.waiter_count, 0U);
    EXPECT_EQ(outcome.granted_count, 0U);
}

TEST(Http2ConnectionTest, AwaitedLocalStreamAttachGrantsCapacityInFifoOrder) {
    LocalAttachFifoOutcome outcome = execute_local_attach_fifo();

    EXPECT_TRUE(outcome.both_queued);
    EXPECT_TRUE(outcome.first_granted_alone);
    EXPECT_EQ(outcome.results[0], fiber::common::IoErr::None);
    EXPECT_EQ(outcome.results[1], fiber::common::IoErr::None);
    EXPECT_EQ(outcome.stream_ids[0], 1U);
    EXPECT_EQ(outcome.stream_ids[1], 3U);
    EXPECT_EQ(outcome.completion_order[0], 1U);
    EXPECT_EQ(outcome.completion_order[1], 2U);
    EXPECT_EQ(outcome.barging_result, fiber::common::IoErr::Busy);
    EXPECT_EQ(outcome.waiter_count, 0U);
    EXPECT_EQ(outcome.granted_count, 0U);
}

TEST(Http2ConnectionTest, TryAttachLocalStreamRespectsFixedStreamTableCapacity) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.local_max_concurrent_streams = 0;
    options.max_peer_concurrent_streams = 2;
    fiber::http::Http2Connection connection(options, &test_http2_stream_factory(), TestHttp2StreamFactory::ops());
    connection.state_ = fiber::http::Http2Connection::State::Running;
    ASSERT_EQ(connection.apply_settings_parameter(0x3, 3), fiber::common::IoErr::None);

    auto *owner1 = TestHttp2StreamOwner::create_owner();
    auto *owner3 = TestHttp2StreamOwner::create_owner();
    auto *blocked_owner = TestHttp2StreamOwner::create_owner();
    ASSERT_NE(owner1, nullptr);
    ASSERT_NE(owner3, nullptr);
    ASSERT_NE(blocked_owner, nullptr);

    auto stream1 = connection.try_attach_local_stream(owner1->stream);
    auto stream3 = connection.try_attach_local_stream(owner3->stream);
    ASSERT_TRUE(stream1.has_value());
    ASSERT_TRUE(stream3.has_value());
    auto blocked = connection.try_attach_local_stream(blocked_owner->stream);
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error(), fiber::common::IoErr::Busy);

    (*stream1)->close(fiber::common::IoErr::Canceled);
    connection.try_release_stream(**stream1);
    stream1->reset();
    auto stream5 = connection.try_attach_local_stream(blocked_owner->stream);
    ASSERT_TRUE(stream5.has_value());
    EXPECT_EQ((*stream5)->stream_id(), 5U);

    connection.close_all_streams(fiber::common::IoErr::Canceled);
}

TEST(Http2ConnectionTest, ReducedPeerLimitWaitsForActiveStreamCountToFallBelowIt) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.max_peer_concurrent_streams = 2;
    fiber::http::Http2Connection connection(options, &test_http2_stream_factory(), TestHttp2StreamFactory::ops());
    connection.state_ = fiber::http::Http2Connection::State::Running;

    auto *owner1 = TestHttp2StreamOwner::create_owner();
    auto *owner3 = TestHttp2StreamOwner::create_owner();
    auto *pending_owner = TestHttp2StreamOwner::create_owner();
    ASSERT_NE(owner1, nullptr);
    ASSERT_NE(owner3, nullptr);
    ASSERT_NE(pending_owner, nullptr);

    auto stream1 = connection.try_attach_local_stream(owner1->stream);
    auto stream3 = connection.try_attach_local_stream(owner3->stream);
    ASSERT_TRUE(stream1.has_value());
    ASSERT_TRUE(stream3.has_value());
    ASSERT_EQ(connection.apply_settings_parameter(0x3, 1), fiber::common::IoErr::None);

    (*stream1)->close(fiber::common::IoErr::Canceled);
    connection.try_release_stream(**stream1);
    stream1->reset();
    auto still_blocked = connection.try_attach_local_stream(pending_owner->stream);
    ASSERT_FALSE(still_blocked.has_value());
    EXPECT_EQ(still_blocked.error(), fiber::common::IoErr::Busy);

    (*stream3)->close(fiber::common::IoErr::Canceled);
    connection.try_release_stream(**stream3);
    stream3->reset();
    auto stream5 = connection.try_attach_local_stream(pending_owner->stream);
    ASSERT_TRUE(stream5.has_value());
    EXPECT_EQ((*stream5)->stream_id(), 5U);

    connection.close_all_streams(fiber::common::IoErr::Canceled);
}

TEST(Http2ConnectionTest, LocalStreamIdExhaustionDoesNotWrapOrWait) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    fiber::http::Http2Connection connection(options, &test_http2_stream_factory(), TestHttp2StreamFactory::ops());
    connection.state_ = fiber::http::Http2Connection::State::Running;
    connection.next_local_stream_id_ = 0x7fffffffU;

    auto *last_owner = TestHttp2StreamOwner::create_owner();
    auto *overflow_owner = TestHttp2StreamOwner::create_owner();
    ASSERT_NE(last_owner, nullptr);
    ASSERT_NE(overflow_owner, nullptr);

    auto last = connection.try_attach_local_stream(last_owner->stream);
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ((*last)->stream_id(), 0x7fffffffU);
    EXPECT_TRUE(connection.local_stream_ids_exhausted_);
    EXPECT_EQ(connection.next_local_stream_id_, 0x7fffffffU);

    auto overflow = connection.try_attach_local_stream(overflow_owner->stream);
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error(), fiber::common::IoErr::Canceled);
    EXPECT_EQ(connection.next_local_stream_id_, 0x7fffffffU);
    delete overflow_owner;
    connection.close_all_streams(fiber::common::IoErr::Canceled);
}

TEST(Http2ConnectionTest, ClientExchangeWaitsForLocalStreamCapacity) {
    ClientExchangeAttachWaitOutcome outcome = execute_client_exchange_attach_wait(true, std::chrono::seconds(1));

    ASSERT_TRUE(outcome.first_result.has_value());
    ASSERT_TRUE(outcome.second_result.has_value());
    EXPECT_TRUE(outcome.wait_observed);
    EXPECT_EQ(outcome.first_stream_id, 1U);
    EXPECT_EQ(outcome.second_stream_id, 3U);
    EXPECT_EQ(outcome.next_stream_id, 5U);
    EXPECT_EQ(outcome.waiter_count, 0U);
}

TEST(Http2ConnectionTest, ClientExchangeStreamCapacityTimeoutDoesNotBindRequest) {
    ClientExchangeAttachWaitOutcome outcome = execute_client_exchange_attach_wait(false, std::chrono::milliseconds(5));

    ASSERT_TRUE(outcome.first_result.has_value());
    ASSERT_FALSE(outcome.second_result.has_value());
    EXPECT_EQ(outcome.second_result.error(), fiber::common::IoErr::TimedOut);
    EXPECT_EQ(outcome.first_stream_id, 1U);
    EXPECT_EQ(outcome.second_stream_id, 0U);
    EXPECT_EQ(outcome.next_stream_id, 3U);
    EXPECT_EQ(outcome.waiter_count, 0U);
}

TEST(Http2ConnectionTest, ServerRejectsPeerStreamsBeyondAdvertisedConcurrentLimit) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Server;
    options.local_max_concurrent_streams = 1;
    options.max_peer_concurrent_streams = 100;

    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "GET"},
                                                 {":scheme", "https"},
                                                 {":path", "/one"},
                                                 {":authority", "example.com"},
                                         },
                                         false);
    request += build_headers_frame_bytes(3,
                                         {
                                                 {":method", "GET"},
                                                 {":scheme", "https"},
                                                 {":path", "/two"},
                                                 {":authority", "example.com"},
                                         },
                                         true);

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)}, options);

    ASSERT_FALSE(outcome.result.has_value());
    EXPECT_EQ(outcome.result.error(), fiber::common::IoErr::Invalid);
}

TEST(Http2ConnectionTest, ServerParsesPathQueryAndExtensionFromPseudoPath) {
    struct UriSnapshot {
        std::string unparsed_uri;
        std::string path;
        std::string query;
        std::string exten;
    };

    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "GET"},
                                                 {":scheme", "https"},
                                                 {":path", "/assets/app.js?v=42#fragment"},
                                                 {":authority", "example.com"},
                                         },
                                         true);

    auto snapshot_promise = std::make_shared<std::promise<UriSnapshot>>();
    auto snapshot_future = snapshot_promise->get_future();
    fiber::http::HttpHandler handler =
            [snapshot_promise](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        snapshot_promise->set_value(UriSnapshot{
                .unparsed_uri = std::string(exchange.uri().unparsed_uri),
                .path = std::string(exchange.uri().path),
                .query = std::string(exchange.uri().query),
                .exten = std::string(exchange.uri().exten),
        });
        (void) co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 204,
                .end_stream = true,
        });
        co_return;
    };

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)}, std::move(handler));

    ASSERT_TRUE(outcome.result.has_value());
    UriSnapshot snapshot = snapshot_future.get();
    EXPECT_EQ(snapshot.unparsed_uri, "/assets/app.js?v=42#fragment");
    EXPECT_EQ(snapshot.path, "/assets/app.js");
    EXPECT_EQ(snapshot.query, "v=42");
    EXPECT_EQ(snapshot.exten, "js");
}

TEST(Http2ConnectionTest, ServerExposesExtendedConnectProtocolWhenEnabled) {
    struct ExtendedConnectSnapshot {
        fiber::http::HttpMethod method = fiber::http::HttpMethod::Unknown;
        std::string scheme;
        std::string protocol;
    };

    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "CONNECT"},
                                                 {":scheme", "https"},
                                                 {":path", "/chat"},
                                                 {":authority", "example.com"},
                                                 {":protocol", "websocket"},
                                         },
                                         true);

    auto snapshot_promise = std::make_shared<std::promise<ExtendedConnectSnapshot>>();
    auto snapshot_future = snapshot_promise->get_future();
    fiber::http::HttpHandler handler =
            [snapshot_promise](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        snapshot_promise->set_value({
                .method = exchange.method(),
                .scheme = std::string(exchange.scheme()),
                .protocol = std::string(exchange.protocol()),
        });
        (void) co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 204,
                .end_stream = true,
        });
        co_return;
    };

    fiber::http::Http2Connection::Options options;
    options.enable_connect_protocol = true;
    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)}, std::move(handler), options);

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_EQ(snapshot_future.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    ExtendedConnectSnapshot snapshot = snapshot_future.get();
    EXPECT_EQ(snapshot.method, fiber::http::HttpMethod::Connect);
    EXPECT_EQ(snapshot.scheme, "https");
    EXPECT_EQ(snapshot.protocol, "websocket");

    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    auto settings = std::find_if(frames.begin(), frames.end(),
                                 [](const EncodedFrame &frame) { return frame.type == 0x4 && frame.flags == 0; });
    ASSERT_NE(settings, frames.end()) << describe_frames(frames);
    ASSERT_TRUE(parse_settings_parameter(*settings, 0x8).has_value());
    EXPECT_EQ(*parse_settings_parameter(*settings, 0x8), 1U);
}

TEST(Http2ConnectionTest, ServerRejectsExtendedConnectProtocolWhenDisabled) {
    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "CONNECT"},
                                                 {":scheme", "https"},
                                                 {":path", "/chat"},
                                                 {":authority", "example.com"},
                                                 {":protocol", "websocket"},
                                         },
                                         true);

    auto handler_called = std::make_shared<std::atomic<bool>>(false);
    fiber::http::HttpHandler handler = [handler_called](fiber::http::HttpExchange &) -> fiber::async::Task<void> {
        handler_called->store(true, std::memory_order_release);
        co_return;
    };
    fiber::http::Http2Connection::Options options;
    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)}, std::move(handler), options, false);

    EXPECT_TRUE(outcome.result.has_value());
    EXPECT_FALSE(handler_called->load(std::memory_order_acquire));
}

TEST(Http2ConnectionTest, ServerBorrowsHpackStaticStorage) {
    struct StorageSnapshot {
        bool method = false;
        bool header_name = false;
        bool header_value = false;
    };

    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "GET"},
                                                 {":scheme", "https"},
                                                 {":path", "/"},
                                                 {":authority", "example.com"},
                                                 {"accept-encoding", "gzip, deflate"},
                                         },
                                         true);

    auto snapshot_promise = std::make_shared<std::promise<StorageSnapshot>>();
    auto snapshot_future = snapshot_promise->get_future();
    fiber::http::HttpHandler handler =
            [snapshot_promise](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        fiber::http::Http2HpackStaticTable::TableEntryView method_entry;
        fiber::http::Http2HpackStaticTable::TableEntryView encoding_entry;
        const bool have_method = fiber::http::Http2HpackStaticTable::get_by_index(2, method_entry);
        const bool have_encoding = fiber::http::Http2HpackStaticTable::get_by_index(16, encoding_entry);

        StorageSnapshot snapshot;
        snapshot.method = have_method && exchange.method_view().data() == method_entry.value.data();
        for (const auto &field: exchange.request_headers()) {
            if (field.name_view() == "accept-encoding") {
                snapshot.header_name = have_encoding && field.name == encoding_entry.name.data();
                snapshot.header_value = have_encoding && field.value == encoding_entry.value.data();
                break;
            }
        }
        snapshot_promise->set_value(snapshot);
        co_return;
    };

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)}, std::move(handler));

    ASSERT_TRUE(outcome.result.has_value());
    StorageSnapshot snapshot = snapshot_future.get();
    EXPECT_TRUE(snapshot.method);
    EXPECT_TRUE(snapshot.header_name);
    EXPECT_TRUE(snapshot.header_value);
}

TEST(Http2ConnectionTest, ServerCopiesHpackDynamicStorageBeforeTableMutation) {
    auto append_integer = [](std::string &out, std::uint32_t value, std::uint8_t prefix_mask, std::uint8_t first_bits) {
        if (value < prefix_mask) {
            out.push_back(static_cast<char>(first_bits | value));
            return;
        }
        out.push_back(static_cast<char>(first_bits | prefix_mask));
        value -= prefix_mask;
        while (value >= 128U) {
            out.push_back(static_cast<char>((value & 0x7fU) | 0x80U));
            value >>= 7U;
        }
        out.push_back(static_cast<char>(value));
    };
    auto append_string = [&append_integer](std::string &out, std::string_view value) {
        append_integer(out, static_cast<std::uint32_t>(value.size()), 0x7fU, 0);
        out.append(value);
    };
    auto append_literal = [&append_integer, &append_string](std::string &out, std::string_view name,
                                                            std::string_view value) {
        append_integer(out, 0, 0x3fU, 0x40U);
        append_string(out, name);
        append_string(out, value);
    };
    auto append_authority = [&append_integer, &append_string](std::string &out) {
        append_integer(out, 1, 0x0fU, 0);
        append_string(out, "example.com");
    };

    std::string first_block;
    first_block.push_back(static_cast<char>(0x82U)); // :method GET
    first_block.push_back(static_cast<char>(0x87U)); // :scheme https
    first_block.push_back(static_cast<char>(0x84U)); // :path /
    append_authority(first_block);
    append_literal(first_block, "x-test", "first");

    std::string second_block;
    second_block.push_back(static_cast<char>(0x82U));
    second_block.push_back(static_cast<char>(0x87U));
    append_integer(second_block, 4, 0x0fU, 0); // literal :path
    append_string(second_block, "/check");
    append_authority(second_block);
    append_integer(second_block, 62, 0x7fU, 0x80U); // dynamic x-test: first
    append_literal(second_block, "x-large", std::string(4060, 'a')); // clears the dynamic table
    append_literal(second_block, "x-new", "overwritten"); // overwrites the old table bytes

    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += make_frame(static_cast<std::uint32_t>(first_block.size()), 0x1, 0x5, 1, first_block);
    request += make_frame(static_cast<std::uint32_t>(second_block.size()), 0x1, 0x5, 3, second_block);

    auto value_promise = std::make_shared<std::promise<std::string>>();
    auto value_future = value_promise->get_future();
    fiber::http::HttpHandler handler =
            [value_promise](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        if (exchange.uri().path == "/check") {
            value_promise->set_value(std::string(exchange.request_headers().get("x-test")));
        }
        co_return;
    };

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)}, std::move(handler));

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_EQ(value_future.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    EXPECT_EQ(value_future.get(), "first");
}

TEST(Http2ConnectionTest, ServerRejectsPseudoPathWithoutLeadingSlash) {
    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "GET"},
                                                 {":scheme", "https"},
                                                 {":path", "not-a-path"},
                                                 {":authority", "example.com"},
                                         },
                                         true);

    fiber::http::HttpHandler handler = [](fiber::http::HttpExchange &) -> fiber::async::Task<void> { co_return; };
    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)}, std::move(handler), {}, false);

    ASSERT_TRUE(outcome.result.has_value());
    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    auto it = std::find_if(frames.begin(), frames.end(),
                           [](const EncodedFrame &frame) { return frame.type == 0x3U && frame.stream_id == 1U; });
    ASSERT_NE(it, frames.end()) << describe_frames(frames);
    ASSERT_EQ(it->payload.size(), 4U);
    EXPECT_EQ(static_cast<unsigned char>((*it).payload[3]), 0x1U);
}

TEST(Http2ConnectionTest, ServerExchangeAbortSendsCancelRstStream) {
    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "POST"},
                                                 {":scheme", "https"},
                                                 {":path", "/upload"},
                                                 {":authority", "example.com"},
                                         },
                                         false);

    auto abort_result = std::make_shared<fiber::common::IoResult<void>>();
    fiber::http::HttpHandler handler = [abort_result](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        *abort_result = exchange.abort(fiber::common::IoErr::ConnReset);
        co_return;
    };
    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)}, std::move(handler));

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_TRUE(abort_result->has_value());
    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    auto it = std::find_if(frames.begin(), frames.end(),
                           [](const EncodedFrame &frame) { return frame.type == 0x3U && frame.stream_id == 1U; });
    ASSERT_NE(it, frames.end()) << describe_frames(frames);
    ASSERT_EQ(it->payload.size(), 4U);
    EXPECT_EQ(static_cast<unsigned char>(it->payload[3]), 0x8U);
}

TEST(Http2ConnectionTest, LocalStreamCreationRequiresStart) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    fiber::http::Http2Connection connection(options, &test_http2_stream_factory(), TestHttp2StreamFactory::ops());
    auto *owner = TestHttp2StreamOwner::create_owner();
    ASSERT_NE(owner, nullptr);
    auto lease = connection.try_attach_local_stream(owner->stream);
    EXPECT_FALSE(lease.has_value());
    delete owner;
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise, options]() mutable -> fiber::async::DetachedTask {
        auto transport = std::make_unique<FakeHttpTransport>(std::vector<std::string>{});
        auto *fake_transport = transport.get();
        ControlHttp2Connection started(std::move(transport), fake_transport, options);
        bool opened = started.open_stream() != nullptr;
        started.request_stop();
        co_await started.stop_and_join();
        promise->set_value(opened);
        fiber::event::EventLoop::current().stop();
        co_return;
    });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_TRUE(future.get());
    group.join();
}

TEST(Http2ConnectionTest, ClientExchangeSendRequestHeaderEncodesRequestHeaders) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ClientRequestHeaderRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0),
                        [promise]() mutable { return run_client_request_header_send(std::move(promise)); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ClientRequestHeaderRunOutcome outcome = future.get();
    group.join();

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_EQ(outcome.stream_id, 1U);
    ASSERT_GE(outcome.written.size(), kClientConnectionPreface.size());
    EXPECT_EQ(outcome.written.substr(0, kClientConnectionPreface.size()), kClientConnectionPreface);

    ParsedHeadersFrames parsed =
            collect_stream_headers_frames(std::string_view(outcome.written).substr(kClientConnectionPreface.size()), 1);
    ASSERT_FALSE(parsed.header_block.empty());
    EXPECT_EQ(parsed.first_flags & 0x1U, 0x1U);
    EXPECT_EQ(parsed.first_flags & 0x4U, 0x4U);

    const auto fields = decode_header_block(parsed.header_block);
    ASSERT_GE(fields.size(), 5U);
    EXPECT_EQ(fields[0].first, ":method");
    EXPECT_EQ(fields[0].second, "POST");
    EXPECT_EQ(fields[1].first, ":scheme");
    EXPECT_EQ(fields[1].second, "https");
    EXPECT_EQ(fields[2].first, ":authority");
    EXPECT_EQ(fields[2].second, "example.com");
    EXPECT_EQ(fields[3].first, ":path");
    EXPECT_EQ(fields[3].second, "/submit");

    bool found_user_agent = false;
    for (const auto &field: fields) {
        if (field.first == "user-agent" && field.second == "fiber-test") {
            found_user_agent = true;
            break;
        }
    }
    EXPECT_TRUE(found_user_agent);
}

TEST(Http2ConnectionTest, ClientExchangeAbortSendsCancelRstStream) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ClientAbortRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise]() mutable { return run_client_exchange_abort(std::move(promise)); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ClientAbortRunOutcome outcome = future.get();
    group.join();

    ASSERT_TRUE(outcome.header_result.has_value());
    ASSERT_TRUE(outcome.abort_result.has_value());
    EXPECT_EQ(outcome.stream_id, 1U);
    EXPECT_TRUE(outcome.local_rst);

    std::vector<EncodedFrame> frames = parse_frames(strip_client_initial_flight(outcome.written));
    auto it = std::find_if(frames.begin(), frames.end(),
                           [](const EncodedFrame &frame) { return frame.type == 0x3U && frame.stream_id == 1U; });
    ASSERT_NE(it, frames.end()) << describe_frames(frames);
    ASSERT_EQ(it->payload.size(), 4U);
    EXPECT_EQ(static_cast<unsigned char>(it->payload[3]), 0x8U);
}

TEST(Http2ConnectionTest, ClientExchangeEncodesExtendedConnectProtocolBeforeRegularHeaders) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ClientExtendedConnectRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise]() mutable {
        return run_client_extended_connect_header_send(std::move(promise), true);
    });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ClientExtendedConnectRunOutcome outcome = future.get();
    group.join();

    ASSERT_TRUE(outcome.header_result.has_value());
    ASSERT_TRUE(outcome.run_result.has_value());
    EXPECT_EQ(outcome.stream_id, 1U);
    EXPECT_EQ(outcome.support_before, fiber::http::Http2ExtendedConnectSupport::Unknown);
    EXPECT_EQ(outcome.support_after, fiber::http::Http2ExtendedConnectSupport::Enabled);

    std::string payload = strip_client_initial_flight(outcome.written);
    ParsedHeadersFrames parsed = collect_stream_headers_frames(payload, 1);
    ASSERT_FALSE(parsed.header_block.empty());
    EXPECT_EQ(parsed.first_flags & 0x1U, 0x0U);

    const auto fields = decode_header_block(parsed.header_block);
    ASSERT_EQ(fields.size(), 6U);
    EXPECT_EQ(fields[0], std::make_pair(std::string(":method"), std::string("CONNECT")));
    EXPECT_EQ(fields[1], std::make_pair(std::string(":scheme"), std::string("https")));
    EXPECT_EQ(fields[2], std::make_pair(std::string(":authority"), std::string("example.com")));
    EXPECT_EQ(fields[3], std::make_pair(std::string(":path"), std::string("/chat")));
    EXPECT_EQ(fields[4], std::make_pair(std::string(":protocol"), std::string("websocket")));
    EXPECT_EQ(fields[5], std::make_pair(std::string("sec-websocket-version"), std::string("13")));
}

TEST(Http2ConnectionTest, ClientExchangeReportsDisabledExtendedConnectAfterPeerSettings) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ClientExtendedConnectRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise]() mutable {
        return run_client_extended_connect_header_send(std::move(promise), false);
    });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ClientExtendedConnectRunOutcome outcome = future.get();
    group.join();

    ASSERT_TRUE(outcome.header_result.has_value());
    ASSERT_TRUE(outcome.run_result.has_value());
    EXPECT_EQ(outcome.support_before, fiber::http::Http2ExtendedConnectSupport::Unknown);
    EXPECT_EQ(outcome.support_after, fiber::http::Http2ExtendedConnectSupport::Disabled);
}

TEST(Http2ConnectionTest, ClientExchangeWriteBodyEncodesDataFrames) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ClientRequestBodyRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise]() mutable { return run_client_request_body_send(std::move(promise)); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ClientRequestBodyRunOutcome outcome = future.get();
    group.join();

    ASSERT_TRUE(outcome.header_result.has_value());
    ASSERT_TRUE(outcome.body_result.has_value());
    EXPECT_EQ(outcome.stream_id, 1U);
    EXPECT_EQ(outcome.body_result.value(), 5U);
    EXPECT_EQ(outcome.conn_send_window, 65530);

    std::string payload = strip_client_initial_flight(outcome.written);
    ParsedHeadersFrames parsed = collect_stream_headers_frames(payload, 1);
    ASSERT_FALSE(parsed.header_block.empty());
    EXPECT_EQ(parsed.first_flags & 0x1U, 0x0U);

    std::vector<EncodedFrame> frames = parse_frames(payload);
    std::size_t data_frame_count = 0;
    for (const auto &frame: frames) {
        if (frame.type != 0x0 || frame.stream_id != 1U) {
            continue;
        }
        ++data_frame_count;
        EXPECT_EQ(frame.flags & 0x1U, 0x1U);
        EXPECT_EQ(frame.payload, "hello");
    }
    EXPECT_EQ(data_frame_count, 1U) << describe_frames(frames);
}

TEST(Http2ConnectionTest, TimedOutQueuedClientBodyResetsStreamAndRejectsRetry) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ClientBodyCancelRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0),
                        [promise]() mutable { return run_client_body_cancel_before_write(std::move(promise)); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ClientBodyCancelRunOutcome outcome = future.get();
    group.join();

    ASSERT_TRUE(outcome.header_result.has_value());
    ASSERT_FALSE(outcome.canceled_body_result.has_value());
    EXPECT_EQ(outcome.canceled_body_result.error(), fiber::common::IoErr::TimedOut);
    EXPECT_EQ(outcome.conn_window_after_cancel, 65535);
    EXPECT_EQ(outcome.stream_window_after_cancel, 65535);
    ASSERT_FALSE(outcome.retried_body_result.has_value());
    EXPECT_EQ(outcome.retried_body_result.error(), fiber::common::IoErr::TimedOut);

    const std::vector<EncodedFrame> frames = parse_frames(strip_client_initial_flight(outcome.written));
    std::size_t data_frame_count = 0;
    std::size_t rst_stream_count = 0;
    for (const EncodedFrame &frame: frames) {
        if (frame.type == 0x0 && frame.stream_id == 1U) {
            ++data_frame_count;
        } else if (frame.type == 0x3 && frame.stream_id == 1U) {
            ++rst_stream_count;
        }
    }
    EXPECT_EQ(data_frame_count, 0U) << describe_frames(frames);
    EXPECT_EQ(rst_stream_count, 1U) << describe_frames(frames);
}

TEST(Http2ConnectionTest, ClientWriteReturnsAfterFirstFlowControlledDataBatch) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ClientPartialBodyRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0),
                        [promise]() mutable { return run_client_partial_request_body_send(std::move(promise)); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ClientPartialBodyRunOutcome outcome = future.get();
    group.join();

    ASSERT_TRUE(outcome.header_result.has_value());
    ASSERT_TRUE(outcome.first_body_result.has_value());
    EXPECT_EQ(*outcome.first_body_result, 2U);
    ASSERT_TRUE(outcome.second_body_result.has_value());
    EXPECT_EQ(*outcome.second_body_result, 3U);

    const std::vector<EncodedFrame> frames = parse_frames(strip_client_initial_flight(outcome.written));
    std::vector<std::string> payloads;
    std::vector<std::uint8_t> flags;
    for (const EncodedFrame &frame: frames) {
        if (frame.type == 0x0 && frame.stream_id == 1U) {
            payloads.push_back(frame.payload);
            flags.push_back(frame.flags);
        }
    }
    ASSERT_EQ(payloads.size(), 2U) << describe_frames(frames);
    EXPECT_EQ(payloads[0], "he");
    EXPECT_EQ(flags[0] & 0x1U, 0U);
    EXPECT_EQ(payloads[1], "llo");
    EXPECT_EQ(flags[1] & 0x1U, 0x1U);
}

TEST(Http2ConnectionTest, ConnectionWindowUpdateWakesQueuedBodySend) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ClientConnWindowWaitRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise]() mutable {
        return run_client_body_waiting_for_connection_window(std::move(promise));
    });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ClientConnWindowWaitRunOutcome outcome = future.get();
    group.join();

    ASSERT_TRUE(outcome.header_result.has_value());
    ASSERT_TRUE(outcome.body_result.has_value());
    EXPECT_EQ(outcome.body_result.value(), 5U);
    EXPECT_EQ(outcome.conn_send_window, 0);
    EXPECT_EQ(outcome.stream_send_window, 65530);

    const std::vector<EncodedFrame> frames = parse_frames(strip_client_initial_flight(outcome.written));
    bool saw_data = false;
    for (const EncodedFrame &frame: frames) {
        if (frame.type == 0x0 && frame.stream_id == 1U) {
            saw_data = true;
            EXPECT_EQ(frame.payload, "hello");
            EXPECT_EQ(frame.flags & 0x1U, 0x1U);
        }
    }
    EXPECT_TRUE(saw_data) << describe_frames(frames);
}

TEST(Http2ConnectionTest, ClientExchangeWriteTrailerEncodesTrailerHeaders) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ClientRequestTrailerRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0),
                        [promise]() mutable { return run_client_request_trailer_send(std::move(promise)); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ClientRequestTrailerRunOutcome outcome = future.get();
    group.join();

    ASSERT_TRUE(outcome.header_result.has_value());
    ASSERT_TRUE(outcome.body_result.has_value());
    ASSERT_TRUE(outcome.trailer_result.has_value());
    EXPECT_EQ(outcome.stream_id, 1U);
    EXPECT_EQ(outcome.body_result.value(), 5U);

    std::string payload = strip_client_initial_flight(outcome.written);
    const auto blocks = collect_stream_header_blocks(payload, 1);
    ASSERT_EQ(blocks.size(), 2U);
    EXPECT_EQ(blocks[0].first_flags & 0x1U, 0x0U);
    EXPECT_EQ(blocks[1].first_flags & 0x1U, 0x1U);

    const auto trailer_fields = decode_header_block(blocks[1].header_block);
    ASSERT_EQ(trailer_fields.size(), 1U);
    EXPECT_EQ(trailer_fields[0].first, "digest");
    EXPECT_EQ(trailer_fields[0].second, "sha-256=xyz");

    std::vector<EncodedFrame> frames = parse_frames(payload);
    std::size_t data_frame_count = 0;
    for (const auto &frame: frames) {
        if (frame.type != 0x0 || frame.stream_id != 1U) {
            continue;
        }
        ++data_frame_count;
        EXPECT_EQ(frame.flags & 0x1U, 0x0U);
        EXPECT_EQ(frame.payload, "hello");
    }
    EXPECT_EQ(data_frame_count, 1U) << describe_frames(frames);
}

TEST(Http2ConnectionTest, ClientExchangeCanReadBufferedResponseBody) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ClientResponseBodyRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise]() mutable { return run_client_response_body_read(std::move(promise)); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ClientResponseBodyRunOutcome outcome = future.get();
    group.join();

    ASSERT_TRUE(outcome.header_result.has_value());
    ASSERT_TRUE(outcome.run_result.has_value());
    ASSERT_TRUE(outcome.body_result.has_value());
    EXPECT_EQ(outcome.stream_id, 1U);

    EXPECT_TRUE(outcome.body_result->complete());
    const auto bytes = chain_to_bytes(std::move(outcome.body_result.value()));
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size()), "hello");
}

TEST(Http2ConnectionTest, ClientExchangeCanReadInformationalFinalAndTrailerHeaders) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ClientResponseHeaderRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise]() mutable {
        return run_client_response_headers_and_trailers_read(std::move(promise));
    });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ClientResponseHeaderRunOutcome outcome = future.get();
    group.join();

    ASSERT_TRUE(outcome.header_result.has_value());
    ASSERT_TRUE(outcome.run_result.has_value());
    ASSERT_TRUE(outcome.informational_result.has_value());
    ASSERT_TRUE(outcome.informational.present);
    EXPECT_EQ(outcome.informational.kind, fiber::http::OutgoingHeaderKind::Informational);
    EXPECT_EQ(outcome.informational.status_code, 103);
    EXPECT_FALSE(outcome.informational.end_stream);
    ASSERT_EQ(outcome.informational.headers.size(), 1U);
    EXPECT_EQ(outcome.informational.headers[0].first, "link");
    EXPECT_EQ(outcome.informational.headers[0].second, "</style.css>; rel=preload");

    ASSERT_TRUE(outcome.final_result.has_value());
    ASSERT_TRUE(outcome.final.present);
    EXPECT_EQ(outcome.final.kind, fiber::http::OutgoingHeaderKind::Final);
    EXPECT_EQ(outcome.final.status_code, 200);
    EXPECT_FALSE(outcome.final.end_stream);
    ASSERT_EQ(outcome.final.headers.size(), 1U);
    EXPECT_EQ(outcome.final.headers[0].first, "content-type");
    EXPECT_EQ(outcome.final.headers[0].second, "text/plain");

    ASSERT_TRUE(outcome.body_result.has_value());
    EXPECT_TRUE(outcome.body_result->complete());
    const auto body_bytes = chain_to_bytes(std::move(outcome.body_result.value()));
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(body_bytes.data()), body_bytes.size()), "hello");

    ASSERT_TRUE(outcome.trailer_result.has_value());
    ASSERT_TRUE(outcome.trailer.present);
    EXPECT_EQ(outcome.trailer.kind, fiber::http::OutgoingHeaderKind::Trailer);
    EXPECT_EQ(outcome.trailer.status_code, 0);
    EXPECT_TRUE(outcome.trailer.end_stream);
    ASSERT_EQ(outcome.trailer.headers.size(), 1U);
    EXPECT_EQ(outcome.trailer.headers[0].first, "digest");
    EXPECT_EQ(outcome.trailer.headers[0].second, "sha-256=xyz");

    ASSERT_TRUE(outcome.end_result.has_value());
    EXPECT_EQ(*outcome.end_result, nullptr);
}

TEST(Http2ConnectionTest, ClientExchangeHeaderEndStreamClosesBodyAndHeaderInput) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ClientResponseHeaderRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0),
                        [promise]() mutable { return run_client_response_header_end_stream_read(std::move(promise)); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ClientResponseHeaderRunOutcome outcome = future.get();
    group.join();

    ASSERT_TRUE(outcome.header_result.has_value());
    ASSERT_TRUE(outcome.run_result.has_value());
    ASSERT_TRUE(outcome.final_result.has_value());
    ASSERT_TRUE(outcome.final.present);
    EXPECT_EQ(outcome.final.kind, fiber::http::OutgoingHeaderKind::Final);
    EXPECT_EQ(outcome.final.status_code, 204);
    EXPECT_TRUE(outcome.final.end_stream);
    ASSERT_EQ(outcome.final.headers.size(), 1U);
    EXPECT_EQ(outcome.final.headers[0].first, "content-type");
    EXPECT_EQ(outcome.final.headers[0].second, "text/plain");

    ASSERT_TRUE(outcome.body_result.has_value());
    EXPECT_EQ(outcome.body_result->readable_bytes(), 0U);
    EXPECT_TRUE(outcome.body_result->complete());

    ASSERT_TRUE(outcome.end_result.has_value());
    EXPECT_EQ(*outcome.end_result, nullptr);
}

TEST(Http2ConnectionTest, ClientExchangeReadHeaderAndBodyReturnCanceledAfterRstStream) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ClientResponseAbortRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0),
                        [promise]() mutable { return run_client_response_read_after_rst_stream(std::move(promise)); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ClientResponseAbortRunOutcome outcome = future.get();
    group.join();

    ASSERT_TRUE(outcome.header_result.has_value());
    ASSERT_TRUE(outcome.run_result.has_value());
    ASSERT_FALSE(outcome.read_header_result.has_value());
    EXPECT_EQ(outcome.read_header_result.error(), fiber::common::IoErr::Canceled);
    ASSERT_FALSE(outcome.read_body_result.has_value());
    EXPECT_EQ(outcome.read_body_result.error(), fiber::common::IoErr::Canceled);
}

TEST(Http2ConnectionTest, ClientExchangeSendRequestHeaderReturnsCanceledAfterPeerGoaway) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ClientGoawayRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0),
                        [promise]() mutable { return run_client_exchange_open_after_goaway(std::move(promise)); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ClientGoawayRunOutcome outcome = future.get();
    group.join();

    EXPECT_EQ(outcome.state, fiber::http::Http2Connection::State::Draining);
    ASSERT_FALSE(outcome.send_result.has_value());
    EXPECT_EQ(outcome.send_result.error(), fiber::common::IoErr::Canceled);
}

TEST(Http2ConnectionTest, GracefulShutdownSendsGoawayAndClosesTransportAfterQueueDrains) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.initial_connection_recv_window = 65535;

    ControlRunOutcome outcome = execute_control_connection(
            {},
            [](ControlHttp2Connection &connection, fiber::http::Http2Stream *, fiber::http::Http2Stream *) {
                connection.request_graceful_close();
                EXPECT_EQ(connection.open_stream(1), nullptr);
            },
            options, true, false);

    ASSERT_TRUE(outcome.result.has_value());
    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    ASSERT_EQ(frames.size(), 1U) << describe_frames(frames);
    EXPECT_EQ(frames[0].type, 0x7);
    EXPECT_EQ(frames[0].stream_id, 0U);
    EXPECT_EQ(outcome.state, fiber::http::Http2Connection::State::Closed);
    EXPECT_GE(outcome.transport_close_count, 1U);
}

TEST(Http2ConnectionTest, RetainedClosedStreamDetachesFromConnectionBeforeLeaseRelease) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    RetainedStreamOutcome outcome = execute_retained_stream_connection(options);

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_TRUE(outcome.stream_opened);
    EXPECT_FALSE(outcome.stream_registered_after_run);
    EXPECT_TRUE(outcome.lease_valid_after_run);
    EXPECT_FALSE(outcome.attached_after_run);
    EXPECT_EQ(outcome.close_reason, fiber::common::IoErr::Canceled);
}

TEST(Http2ConnectionTest, SendsStableSpanAcrossPartialWrites) {
    SendOutcome outcome = execute_send_connection(
            [](SendingHttp2Connection &connection) { return connection.submit_bytes("hello world"); }, 11, {3, 4, 4});

    ASSERT_EQ(outcome.submit_error, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.written, "hello world");
}

TEST(Http2ConnectionTest, SendsIoBufChainUsingWritev) {
    SendOutcome outcome = execute_send_connection(
            [](SendingHttp2Connection &connection) {
                fiber::mem::IoBuf first = fiber::mem::IoBuf::allocate(4);
                if (!first) {
                    return fiber::common::IoErr::NoMem;
                }
                std::memcpy(first.writable_data(), "ab", 2);
                first.commit(2);

                fiber::mem::IoBuf second = fiber::mem::IoBuf::allocate(8);
                if (!second) {
                    return fiber::common::IoErr::NoMem;
                }
                std::memcpy(second.writable_data(), "cdef", 4);
                second.commit(4);

                fiber::mem::IoBufNodePool pool;
                fiber::mem::IoBufChain chain(pool);
                if (!chain.append(std::move(first))) {
                    return fiber::common::IoErr::NoMem;
                }
                if (!chain.append(std::move(second))) {
                    return fiber::common::IoErr::NoMem;
                }
                std::array<iovec, 4> iov{};
                int count = chain.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
                std::string flattened;
                for (int i = 0; i < count; ++i) {
                    flattened.append(static_cast<const char *>(iov[i].iov_base), iov[i].iov_len);
                }
                return connection.submit_bytes(flattened);
            },
            6, {4, 2});

    ASSERT_EQ(outcome.submit_error, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.written, "abcdef");
}

TEST(Http2ConnectionTest, ServerHandlerCanSendHeaderOnlyResponseHeaders) {
    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "GET"},
                                                 {":scheme", "https"},
                                                 {":path", "/"},
                                                 {":authority", "example.com"},
                                         },
                                         true);

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)});

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_TRUE(outcome.header_result.has_value());

    ParsedHeadersFrames parsed = collect_stream_headers_frames(outcome.written, 1);
    ASSERT_FALSE(parsed.header_block.empty());
    EXPECT_EQ(parsed.first_flags & 0x1U, 0x1U);
    EXPECT_EQ(parsed.first_flags & 0x4U, 0x4U);

    const auto fields = decode_header_block(parsed.header_block);
    ASSERT_FALSE(fields.empty());
    EXPECT_EQ(fields[0].first, ":status");
    EXPECT_EQ(fields[0].second, "204");

    bool found_server = false;
    for (const auto &field: fields) {
        if (field.first == "server" && field.second == "fiber") {
            found_server = true;
            break;
        }
    }
    EXPECT_TRUE(found_server);
}

TEST(Http2ConnectionTest, ServerHandlerCanSendInformationalHeadersBeforeFinalResponse) {
    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "GET"},
                                                 {":scheme", "https"},
                                                 {":path", "/"},
                                                 {":authority", "example.com"},
                                         },
                                         true);

    auto continue_result = std::make_shared<fiber::common::IoResult<void>>();
    auto final_result = std::make_shared<fiber::common::IoResult<void>>();
    fiber::http::HttpHandler handler = [continue_result,
                                        final_result](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        *continue_result = co_await exchange.send_continue_header();
        if (!*continue_result) {
            co_return;
        }
        fiber::http::HttpHeaders headers(exchange.pool());
        headers.set("server", "fiber");
        *final_result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 204,
                .headers = &headers,
                .end_stream = true,
        });
        co_return;
    };

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)}, std::move(handler));

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_TRUE(continue_result->has_value());
    ASSERT_TRUE(final_result->has_value());

    const auto blocks = collect_stream_header_blocks(outcome.written, 1);
    ASSERT_EQ(blocks.size(), 2U);

    EXPECT_EQ(blocks[0].first_flags & 0x1U, 0x0U);
    EXPECT_EQ(blocks[0].first_flags & 0x4U, 0x4U);
    const auto informational_fields = decode_header_block(blocks[0].header_block);
    ASSERT_EQ(informational_fields.size(), 1U);
    EXPECT_EQ(informational_fields[0].first, ":status");
    EXPECT_EQ(informational_fields[0].second, "100");

    EXPECT_EQ(blocks[1].first_flags & 0x1U, 0x1U);
    EXPECT_EQ(blocks[1].first_flags & 0x4U, 0x4U);
    const auto final_fields = decode_header_block(blocks[1].header_block);
    ASSERT_FALSE(final_fields.empty());
    EXPECT_EQ(final_fields[0].first, ":status");
    EXPECT_EQ(final_fields[0].second, "204");

    bool found_server = false;
    for (const auto &field: final_fields) {
        if (field.first == "server" && field.second == "fiber") {
            found_server = true;
            break;
        }
    }
    EXPECT_TRUE(found_server);
}

TEST(Http2ConnectionTest, ServerFinalResponseDiscardsUnreadRequestBodyWithoutReset) {
    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "POST"},
                                                 {":scheme", "https"},
                                                 {":path", "/upload"},
                                                 {":authority", "example.com"},
                                                 {"content-length", "6"},
                                         },
                                         false);

    auto header_result = std::make_shared<fiber::common::IoResult<void>>();
    fiber::http::HttpHandler handler =
            [header_result](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        *header_result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 413,
                .headers = nullptr,
                .body = fiber::http::HttpBodySpec::ContentLength(0),
                .end_stream = true,
        });
        co_return;
    };

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)}, std::move(handler));

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_TRUE(header_result->has_value());

    const auto blocks = collect_stream_header_blocks(outcome.written, 1);
    ASSERT_EQ(blocks.size(), 1U);
    const auto fields = decode_header_block(blocks[0].header_block);
    ASSERT_FALSE(fields.empty());
    EXPECT_EQ(fields[0].first, ":status");
    EXPECT_EQ(fields[0].second, "413");

    const std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    auto rst = std::find_if(frames.begin(), frames.end(),
                            [](const EncodedFrame &frame) { return frame.type == 0x3U && frame.stream_id == 1U; });
    EXPECT_EQ(rst, frames.end()) << describe_frames(frames);
}

TEST(Http2ConnectionTest, ServerHandlerCanSendResponseBodyDataFrames) {
    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "GET"},
                                                 {":scheme", "https"},
                                                 {":path", "/body"},
                                                 {":authority", "example.com"},
                                         },
                                         true);

    auto header_result = std::make_shared<fiber::common::IoResult<void>>();
    auto body_result = std::make_shared<fiber::common::IoResult<size_t>>();
    fiber::http::HttpHandler handler = [header_result,
                                        body_result](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        fiber::http::HttpHeaders headers(exchange.pool());
        headers.set("server", "fiber");
        *header_result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 200,
                .headers = &headers,
                .end_stream = false,
        });
        if (!*header_result) {
            co_return;
        }
        *body_result = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>("hello"), 5, true);
        co_return;
    };

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)}, std::move(handler));

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_TRUE(header_result->has_value());
    ASSERT_TRUE(body_result->has_value());
    EXPECT_EQ(body_result->value(), 5U);

    ParsedHeadersFrames parsed = collect_stream_headers_frames(outcome.written, 1);
    ASSERT_FALSE(parsed.header_block.empty());
    EXPECT_EQ(parsed.first_flags & 0x1U, 0x0U);
    EXPECT_EQ(parsed.first_flags & 0x4U, 0x4U);

    const auto fields = decode_header_block(parsed.header_block);
    ASSERT_FALSE(fields.empty());
    EXPECT_EQ(fields[0].first, ":status");
    EXPECT_EQ(fields[0].second, "200");

    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    std::size_t data_frame_count = 0;
    for (const auto &frame: frames) {
        if (frame.type != 0x0 || frame.stream_id != 1U) {
            continue;
        }
        ++data_frame_count;
        EXPECT_EQ(frame.flags & 0x1U, 0x1U);
        EXPECT_EQ(frame.payload, "hello");
    }
    EXPECT_EQ(data_frame_count, 1U) << describe_frames(frames);
}

TEST(Http2ConnectionTest, ServerHandlerResumesBodySendAfterStreamWindowUpdate) {
    fiber::http::Http2Connection::Options options;
    options.initial_stream_send_window = 0;

    std::string first = std::string(kClientConnectionPreface);
    first += make_frame(0, 0x4, 0x0, 0, {});
    first += build_headers_frame_bytes(1,
                                       {
                                               {":method", "GET"},
                                               {":scheme", "https"},
                                               {":path", "/body-window"},
                                               {":authority", "example.com"},
                                       },
                                       true);
    std::string stream_update_payload(4, '\0');
    stream_update_payload[3] = 5;
    std::string second = make_frame(4, 0x8, 0x0, 1, stream_update_payload);

    auto header_result = std::make_shared<fiber::common::IoResult<void>>();
    auto body_result = std::make_shared<fiber::common::IoResult<size_t>>();
    fiber::http::HttpHandler handler = [header_result,
                                        body_result](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        *header_result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 200,
                .end_stream = false,
        });
        if (!*header_result) {
            co_return;
        }
        *body_result = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>("hello"), 5, true);
        co_return;
    };

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(first), std::move(second)}, std::move(handler),
                                                            fiber::http::HttpServerOptions{}, options);

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_TRUE(header_result->has_value());
    ASSERT_TRUE(body_result->has_value());
    EXPECT_EQ(body_result->value(), 5U);

    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    bool saw_headers = false;
    bool saw_data = false;
    for (const auto &frame: frames) {
        if (frame.stream_id != 1U) {
            continue;
        }
        if (frame.type == 0x1) {
            saw_headers = true;
            continue;
        }
        if (frame.type == 0x0) {
            saw_data = true;
            EXPECT_EQ(frame.payload, "hello");
            EXPECT_EQ(frame.flags & 0x1U, 0x1U);
        }
    }
    EXPECT_TRUE(saw_headers) << describe_frames(frames);
    EXPECT_TRUE(saw_data) << describe_frames(frames);
}

TEST(Http2ConnectionTest, ServerWriteReturnsAfterFirstFlowControlledDataBatch) {
    fiber::http::Http2Connection::Options options;
    options.initial_stream_send_window = 2;

    std::string first = std::string(kClientConnectionPreface);
    first += make_frame(0, 0x4, 0x0, 0, {});
    first += build_headers_frame_bytes(1,
                                       {
                                               {":method", "GET"},
                                               {":scheme", "https"},
                                               {":path", "/partial-body"},
                                               {":authority", "example.com"},
                                       },
                                       true);
    auto first_result = std::make_shared<fiber::common::IoResult<std::size_t>>();
    fiber::http::HttpHandler handler = [first_result](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        auto header = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 200,
                .body = fiber::http::HttpBodySpec::ContentLength(5),
                .end_stream = false,
        });
        if (!header) {
            co_return;
        }
        constexpr std::string_view kBody = "hello";
        *first_result =
                co_await exchange.write(reinterpret_cast<const std::uint8_t *>(kBody.data()), kBody.size(), true);
        co_return;
    };

    ServerHeaderRunOutcome outcome =
            execute_server_request({std::move(first)}, std::move(handler), fiber::http::HttpServerOptions{}, options);

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_TRUE(first_result->has_value());
    EXPECT_EQ(**first_result, 2U);

    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    std::vector<std::string> payloads;
    std::vector<std::uint8_t> flags;
    for (const auto &frame: frames) {
        if (frame.type == 0x0 && frame.stream_id == 1U) {
            payloads.push_back(frame.payload);
            flags.push_back(frame.flags);
        }
    }
    ASSERT_EQ(payloads.size(), 1U) << describe_frames(frames);
    EXPECT_EQ(payloads[0], "he");
    EXPECT_EQ(flags[0] & 0x1U, 0U);
}

TEST(Http2ConnectionTest, ServerWriteTimeoutResetsStreamAndRejectsRetry) {
    fiber::http::Http2Connection::Options options;
    options.initial_stream_send_window = 0;

    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "GET"},
                                                 {":scheme", "https"},
                                                 {":path", "/write-timeout"},
                                                 {":authority", "example.com"},
                                         },
                                         true);

    auto first_result =
            std::make_shared<fiber::common::IoResult<std::size_t>>(std::unexpected(fiber::common::IoErr::Invalid));
    auto retry_result =
            std::make_shared<fiber::common::IoResult<std::size_t>>(std::unexpected(fiber::common::IoErr::Invalid));
    auto terminal_error = std::make_shared<fiber::common::IoErr>(fiber::common::IoErr::None);
    fiber::http::HttpHandler handler = [first_result, retry_result, terminal_error](
                                               fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        auto header = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 200,
                .body = fiber::http::HttpBodySpec::ContentLength(5),
                .end_stream = false,
        });
        if (!header) {
            co_return;
        }

        constexpr std::string_view kBody = "hello";
        *first_result = co_await exchange.write(reinterpret_cast<const std::uint8_t *>(kBody.data()), kBody.size(),
                                                true, std::chrono::milliseconds::zero());
        *retry_result = co_await exchange.write(reinterpret_cast<const std::uint8_t *>(kBody.data()), kBody.size(),
                                                true, std::chrono::milliseconds::zero());
        *terminal_error = exchange.response_stats().terminal_error;
        co_return;
    };

    ServerHeaderRunOutcome outcome =
            execute_server_request({std::move(request)}, std::move(handler), fiber::http::HttpServerOptions{}, options);

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_FALSE(first_result->has_value());
    EXPECT_EQ(first_result->error(), fiber::common::IoErr::TimedOut);
    ASSERT_FALSE(retry_result->has_value());
    EXPECT_EQ(retry_result->error(), fiber::common::IoErr::TimedOut);
    EXPECT_EQ(*terminal_error, fiber::common::IoErr::TimedOut);

    std::size_t reset_count = 0;
    for (const auto &frame: parse_frames(outcome.written)) {
        if (frame.type == 0x3 && frame.stream_id == 1U) {
            ++reset_count;
        }
    }
    EXPECT_EQ(reset_count, 1U);
}

TEST(Http2ConnectionTest, ServerHandlerCanSendResponseTrailers) {
    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "GET"},
                                                 {":scheme", "https"},
                                                 {":path", "/trailers"},
                                                 {":authority", "example.com"},
                                         },
                                         true);

    auto header_result = std::make_shared<fiber::common::IoResult<void>>();
    auto body_result = std::make_shared<fiber::common::IoResult<size_t>>();
    auto trailer_result = std::make_shared<fiber::common::IoResult<void>>();
    fiber::http::HttpHandler handler = [header_result, body_result, trailer_result](
                                               fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        fiber::http::HttpHeaders headers(exchange.pool());
        headers.set("server", "fiber");
        *header_result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 200,
                .headers = &headers,
                .end_stream = false,
        });
        if (!*header_result) {
            co_return;
        }

        *body_result = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>("hello"), 5, false);
        if (!*body_result) {
            co_return;
        }

        fiber::http::HttpHeaders trailers(exchange.pool());
        trailers.set("digest", "sha-256=xyz");
        *trailer_result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Trailer,
                .headers = &trailers,
                .end_stream = true,
        });
        co_return;
    };

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)}, std::move(handler));

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_TRUE(header_result->has_value());
    ASSERT_TRUE(body_result->has_value());
    ASSERT_TRUE(trailer_result->has_value());
    EXPECT_EQ(body_result->value(), 5U);

    const auto blocks = collect_stream_header_blocks(outcome.written, 1);
    ASSERT_EQ(blocks.size(), 2U);

    EXPECT_EQ(blocks[0].first_flags & 0x1U, 0x0U);
    const auto final_fields = decode_header_block(blocks[0].header_block);
    ASSERT_FALSE(final_fields.empty());
    EXPECT_EQ(final_fields[0].first, ":status");
    EXPECT_EQ(final_fields[0].second, "200");

    EXPECT_EQ(blocks[1].first_flags & 0x1U, 0x1U);
    const auto trailer_fields = decode_header_block(blocks[1].header_block);
    ASSERT_EQ(trailer_fields.size(), 1U);
    EXPECT_EQ(trailer_fields[0].first, "digest");
    EXPECT_EQ(trailer_fields[0].second, "sha-256=xyz");

    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    std::size_t data_frame_count = 0;
    for (const auto &frame: frames) {
        if (frame.type != 0x0 || frame.stream_id != 1U) {
            continue;
        }
        ++data_frame_count;
        EXPECT_EQ(frame.flags & 0x1U, 0x0U);
        EXPECT_EQ(frame.payload, "hello");
    }
    EXPECT_EQ(data_frame_count, 1U) << describe_frames(frames);
}

TEST(Http2ConnectionTest, ServerIgnoresPriorityUpdateFrame) {
    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += make_frame(8, 0x10, 0x0, 0, std::string("\0\0\0\1u=1", 8));
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "GET"},
                                                 {":scheme", "https"},
                                                 {":path", "/"},
                                                 {":authority", "example.com"},
                                         },
                                         true);

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)});

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_TRUE(outcome.header_result.has_value());

    ParsedHeadersFrames parsed = collect_stream_headers_frames(outcome.written, 1);
    ASSERT_FALSE(parsed.header_block.empty());
    EXPECT_EQ(parsed.first_flags & 0x1U, 0x1U);
    EXPECT_EQ(parsed.first_flags & 0x4U, 0x4U);

    const auto fields = decode_header_block(parsed.header_block);
    ASSERT_FALSE(fields.empty());
    EXPECT_EQ(fields[0].first, ":status");
    EXPECT_EQ(fields[0].second, "204");
}

TEST(Http2ConnectionTest, ServerHandlerCanReadBufferedRequestBodyAcrossMultipleDataFrames) {
    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "POST"},
                                                 {":scheme", "https"},
                                                 {":path", "/upload"},
                                                 {":authority", "example.com"},
                                                 {"content-length", "11"},
                                         },
                                         false);
    request += make_frame(6, 0x0, 0x0, 1, "hello ");
    request += make_frame(5, 0x0, 0x1, 1, "world");

    auto observed_body = std::make_shared<std::string>();
    auto last_flags = std::make_shared<std::vector<bool>>();
    auto read_result = std::make_shared<fiber::common::IoResult<void>>();
    auto header_result = std::make_shared<fiber::common::IoResult<void>>();
    auto framing_ok = std::make_shared<bool>(false);
    fiber::http::HttpHandler handler = [observed_body, last_flags, read_result, header_result,
                                        framing_ok](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        const auto body_spec = exchange.request_body_spec();
        *framing_ok = body_spec.is_content_length() && body_spec.content_length() == 11;
        co_await fiber::async::sleep(std::chrono::milliseconds(2));

        for (;;) {
            auto chunk = co_await exchange.read_body(4);
            if (!chunk) {
                *read_result = std::unexpected(chunk.error());
                co_return;
            }

            const bool last = chunk->complete();
            const auto bytes = chain_to_bytes(std::move(*chunk));
            observed_body->append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            last_flags->push_back(last);
            if (last) {
                break;
            }
        }

        *read_result = fiber::common::IoResult<void>{};
        fiber::http::HttpHeaders headers(exchange.pool());
        headers.set("server", "fiber");
        *header_result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 204,
                .headers = &headers,
                .end_stream = true,
        });
        co_return;
    };

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)}, std::move(handler));

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_TRUE(read_result->has_value());
    ASSERT_TRUE(header_result->has_value());
    EXPECT_TRUE(*framing_ok);
    EXPECT_EQ(*observed_body, "hello world");
    ASSERT_EQ(last_flags->size(), 3U);
    EXPECT_FALSE((*last_flags)[0]);
    EXPECT_FALSE((*last_flags)[1]);
    EXPECT_TRUE((*last_flags)[2]);

    ParsedHeadersFrames parsed = collect_stream_headers_frames(outcome.written, 1);
    ASSERT_FALSE(parsed.header_block.empty());
    EXPECT_EQ(parsed.first_flags & 0x1U, 0x1U);
    EXPECT_EQ(parsed.first_flags & 0x4U, 0x4U);
}

TEST(Http2ConnectionTest, ServerHandlerCanAwaitLaterRequestBodyFrame) {
    std::string first = std::string(kClientConnectionPreface);
    first += make_frame(0, 0x4, 0x0, 0, {});
    first += build_headers_frame_bytes(1,
                                       {
                                               {":method", "POST"},
                                               {":scheme", "https"},
                                               {":path", "/delayed"},
                                               {":authority", "example.com"},
                                       },
                                       false);
    std::string second = make_frame(12, 0x0, 0x1, 1, "delayed body");

    auto observed_body = std::make_shared<std::string>();
    auto observed_last = std::make_shared<bool>(false);
    auto read_result = std::make_shared<fiber::common::IoResult<fiber::mem::IoBufChain>>();
    auto header_result = std::make_shared<fiber::common::IoResult<void>>();
    fiber::http::HttpHandler handler = [observed_body, observed_last, read_result, header_result](
                                               fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        *read_result = co_await exchange.read_body(64);
        if (!*read_result) {
            co_return;
        }

        *observed_last = read_result->value().complete();
        const auto bytes = chain_to_bytes(std::move(read_result->value()));
        observed_body->assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());

        fiber::http::HttpHeaders headers(exchange.pool());
        headers.set("server", "fiber");
        *header_result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 204,
                .headers = &headers,
                .end_stream = true,
        });
        co_return;
    };

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(first), std::move(second)}, std::move(handler));

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_TRUE(read_result->has_value());
    ASSERT_TRUE(header_result->has_value());
    EXPECT_EQ(*observed_body, "delayed body");
    EXPECT_TRUE(*observed_last);
}

TEST(Http2ConnectionTest, ServerHandlerSendAfterConnectionCloseReturnsCanceled) {
    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "GET"},
                                                 {":scheme", "https"},
                                                 {":path", "/late-send"},
                                                 {":authority", "example.com"},
                                         },
                                         true);

    ServerDelayedSendAfterCloseOutcome outcome = execute_server_delayed_send_after_close({std::move(request)});

    ASSERT_TRUE(outcome.run_result.has_value());
    ASSERT_TRUE(outcome.delayed_send_completed);
    ASSERT_FALSE(outcome.delayed_send_result.has_value());
    EXPECT_EQ(outcome.delayed_send_result.error(), fiber::common::IoErr::Canceled);
}

TEST(Http2ConnectionTest, ServerReadBodyReturnsLastWhenRequestEndsAtHeaders) {
    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "POST"},
                                                 {":scheme", "https"},
                                                 {":path", "/empty"},
                                                 {":authority", "example.com"},
                                         },
                                         true);

    auto read_result = std::make_shared<fiber::common::IoResult<fiber::mem::IoBufChain>>();
    auto framing_is_none = std::make_shared<bool>(false);
    fiber::http::HttpHandler handler =
            [read_result, framing_is_none](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        *framing_is_none = exchange.request_body_spec().is_none();
        *read_result = co_await exchange.read_body(64);
        co_return;
    };

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)}, std::move(handler));

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_TRUE(read_result->has_value());
    EXPECT_TRUE(*framing_is_none);
    EXPECT_EQ(read_result->value().readable_bytes(), 0u);
    EXPECT_TRUE(read_result->value().complete());
}

TEST(Http2ConnectionTest, ServerReadBodyTimesOutWhileWaitingForMoreBody) {
    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "POST"},
                                                 {":scheme", "https"},
                                                 {":path", "/timeout"},
                                                 {":authority", "example.com"},
                                         },
                                         false);

    fiber::http::HttpServerOptions http_options;

    auto read_result = std::make_shared<fiber::common::IoResult<fiber::mem::IoBufChain>>();
    fiber::http::HttpHandler handler = [read_result](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        *read_result = co_await exchange.read_body(64, std::chrono::seconds(0));
        co_return;
    };

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)}, std::move(handler), http_options);

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_FALSE(read_result->has_value());
    EXPECT_EQ(read_result->error(), fiber::common::IoErr::TimedOut);
}

TEST(Http2ConnectionTest, ServerReadBodyReturnsCanceledWhenPeerResetsStreamWhileWaiting) {
    std::string first = std::string(kClientConnectionPreface);
    first += make_frame(0, 0x4, 0x0, 0, {});
    first += build_headers_frame_bytes(1,
                                       {
                                               {":method", "POST"},
                                               {":scheme", "https"},
                                               {":path", "/rst"},
                                               {":authority", "example.com"},
                                       },
                                       false);

    std::string rst_payload(4, '\0');
    std::string second = make_frame(4, 0x3, 0x0, 1, rst_payload);

    auto read_result = std::make_shared<fiber::common::IoResult<fiber::mem::IoBufChain>>();
    fiber::http::HttpHandler handler = [read_result](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        *read_result = co_await exchange.read_body(64);
        co_return;
    };

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(first), std::move(second)}, std::move(handler));

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_FALSE(read_result->has_value());
    EXPECT_EQ(read_result->error(), fiber::common::IoErr::Canceled);
}

TEST(Http2ConnectionTest, ServerResponseChannelWaitObservesPeerResetBeforeHandlerDispatch) {
    std::string first = std::string(kClientConnectionPreface);
    first += make_frame(0, 0x4, 0x0, 0, {});
    first += build_headers_frame_bytes(1,
                                       {
                                               {":method", "GET"},
                                               {":scheme", "https"},
                                               {":path", "/response-channel-reset"},
                                               {":authority", "example.com"},
                                       },
                                       false);

    std::string rst_payload(4, '\0');
    std::string second = make_frame(4, 0x3, 0x0, 1, rst_payload);

    auto initial_closed = std::make_shared<bool>(true);
    auto final_closed = std::make_shared<bool>(false);
    auto wait_result = std::make_shared<fiber::common::IoResult<void>>(std::unexpected(fiber::common::IoErr::Invalid));
    fiber::http::HttpHandler handler = [initial_closed, final_closed,
                                        wait_result](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        *initial_closed = exchange.response_channel_closed();
        *wait_result = co_await exchange.wait_response_channel_closed();
        *final_closed = exchange.response_channel_closed();
        co_return;
    };

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(first), std::move(second)}, std::move(handler));

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_TRUE(*initial_closed);
    EXPECT_TRUE(wait_result->has_value());
    EXPECT_TRUE(*final_closed);
}

TEST(Http2ConnectionTest, PeerEndStreamDoesNotCloseResponseChannel) {
    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "GET"},
                                                 {":scheme", "https"},
                                                 {":path", "/request-finished"},
                                                 {":authority", "example.com"},
                                         },
                                         true);

    auto closed_before = std::make_shared<bool>(true);
    auto closed_after = std::make_shared<bool>(true);
    auto wait_result = std::make_shared<fiber::common::IoResult<void>>(std::unexpected(fiber::common::IoErr::Invalid));
    fiber::http::HttpHandler handler = [closed_before, closed_after,
                                        wait_result](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        *closed_before = exchange.response_channel_closed();
        *wait_result = co_await fiber::async::timeout_for(
                [&exchange]() { return exchange.wait_response_channel_closed(); }, std::chrono::milliseconds(1));
        *closed_after = exchange.response_channel_closed();
        co_return;
    };

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)}, std::move(handler));

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_FALSE(*closed_before);
    ASSERT_FALSE(wait_result->has_value());
    EXPECT_EQ(wait_result->error(), fiber::common::IoErr::TimedOut);
    EXPECT_FALSE(*closed_after);
}

TEST(Http2ConnectionTest, ServerReadBodyReplenishesStreamWindowAfterCrossingLowWatermark) {
    fiber::http::Http2Connection::Options options;
    options.initial_connection_recv_window = 65535;
    options.initial_stream_recv_window = 64;
    options.stream_recv_window_low_watermark = 16;

    std::string body(49, 'x');
    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "POST"},
                                                 {":scheme", "https"},
                                                 {":path", "/stream-window"},
                                                 {":authority", "example.com"},
                                         },
                                         false);
    request += make_frame(static_cast<std::uint32_t>(body.size()), 0x0, 0x1, 1, body);

    auto read_result = std::make_shared<fiber::common::IoResult<void>>();
    auto header_result = std::make_shared<fiber::common::IoResult<void>>();
    fiber::http::HttpHandler handler =
            [read_result, header_result](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        auto body_result = co_await exchange.read_body(34);
        if (!body_result) {
            *read_result = std::unexpected(body_result.error());
            co_return;
        }
        *read_result = {};

        fiber::http::HttpHeaders headers(exchange.pool());
        headers.set("server", "fiber");
        *header_result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 204,
                .headers = &headers,
                .end_stream = true,
        });
        co_return;
    };

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)}, std::move(handler), options);

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_TRUE(read_result->has_value());
    ASSERT_TRUE(header_result->has_value());

    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    bool saw_stream_window_update = false;
    for (const auto &frame: frames) {
        if (frame.type == static_cast<std::uint8_t>(fiber::http::Http2FrameType::WindowUpdate) &&
            frame.stream_id == 1U) {
            EXPECT_EQ(parse_window_update_increment(frame), 34U);
            saw_stream_window_update = true;
        }
    }
    EXPECT_TRUE(saw_stream_window_update) << describe_frames(frames);
}

TEST(Http2ConnectionTest, ServerReceiveDataReplenishesConnectionWindowAfterCrossingLowWatermark) {
    fiber::http::Http2Connection::Options options;
    options.initial_connection_recv_window = 65535;
    options.connection_recv_window_low_watermark = 65520;

    std::string body(32, 'z');
    std::string request = std::string(kClientConnectionPreface);
    request += make_frame(0, 0x4, 0x0, 0, {});
    request += build_headers_frame_bytes(1,
                                         {
                                                 {":method", "POST"},
                                                 {":scheme", "https"},
                                                 {":path", "/conn-window"},
                                                 {":authority", "example.com"},
                                         },
                                         false);
    request += make_frame(static_cast<std::uint32_t>(body.size()), 0x0, 0x1, 1, body);

    ServerHeaderRunOutcome outcome = execute_server_request({std::move(request)}, options);

    ASSERT_TRUE(outcome.result.has_value());

    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    bool saw_conn_window_update = false;
    for (const auto &frame: frames) {
        if (frame.type == static_cast<std::uint8_t>(fiber::http::Http2FrameType::WindowUpdate) &&
            frame.stream_id == 0U) {
            EXPECT_EQ(parse_window_update_increment(frame), 32U);
            saw_conn_window_update = true;
        }
    }
    EXPECT_TRUE(saw_conn_window_update) << describe_frames(frames);
}

TEST(Http2ConnectionTest, ClosingSendingNotifiesQueuedEntries) {
    SendOutcome outcome = execute_send_connection(
            [](SendingHttp2Connection &connection) {
                fiber::common::IoErr err = connection.submit_bytes("first");
                if (err != fiber::common::IoErr::None) {
                    return err;
                }
                err = connection.submit_bytes("second");
                if (err != fiber::common::IoErr::None) {
                    return err;
                }
                connection.request_stop(fiber::common::IoErr::Canceled);
                return fiber::common::IoErr::None;
            },
            0);

    ASSERT_EQ(outcome.submit_error, fiber::common::IoErr::None);
    EXPECT_TRUE(outcome.written.empty());
}
