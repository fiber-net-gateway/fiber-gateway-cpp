#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoopGroup.h"
#include "http/Http2Connection.h"
#include "http/Http2Stream.h"

namespace {

using fiber::async::DetachedTask;

constexpr std::string_view kClientConnectionPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

class FakeHttpTransport final : public fiber::http::HttpTransport {
public:
    explicit FakeHttpTransport(std::vector<std::string> chunks, std::vector<size_t> write_steps = {},
                               bool block_reads = false, bool hold_eof = false) :
        chunks_(std::move(chunks)), write_steps_(std::move(write_steps)), reads_blocked_(block_reads),
        hold_eof_(hold_eof) {}

    fiber::async::Task<fiber::common::IoResult<void>> handshake(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> shutdown(std::chrono::milliseconds) override {
        ++shutdown_count_;
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> read(void *buf, size_t len, std::chrono::milliseconds) override {
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

    fiber::async::Task<fiber::common::IoResult<size_t>> write(fiber::mem::IoBuf &buf, std::chrono::milliseconds timeout) override {
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
    void release_reads() noexcept { reads_blocked_ = false; }

    [[nodiscard]] bool valid() const noexcept override { return !closed_; }
    [[nodiscard]] int fd() const noexcept override { return -1; }
    [[nodiscard]] std::string negotiated_alpn() const noexcept override { return "h2"; }
    [[nodiscard]] const fiber::net::SocketAddress &remote_addr() const noexcept override { return remote_addr_; }
    [[nodiscard]] const std::string &written() const noexcept { return written_; }
    [[nodiscard]] std::size_t close_count() const noexcept { return close_count_; }
    [[nodiscard]] std::size_t shutdown_count() const noexcept { return shutdown_count_; }

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
    std::size_t close_count_ = 0;
    std::size_t shutdown_count_ = 0;
    std::string written_;
    fiber::net::SocketAddress remote_addr_{};
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
        fiber::http::Http2Connection(std::move(transport), options) {}

    const std::vector<ObservedChunk> &chunks() const noexcept { return chunks_; }
    fiber::async::Task<void> stop_and_join() noexcept { co_await stop_and_join_send_loop(); }

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
};

struct CompletedSend {
    std::size_t total_bytes = 0;
    std::size_t written_bytes = 0;
    fiber::common::IoErr result = fiber::common::IoErr::None;
};

struct SendOutcome {
    fiber::common::IoErr submit_error = fiber::common::IoErr::None;
    std::vector<CompletedSend> completions;
    std::string written;
};

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
    bool stream1_registered = false;
    bool stream1_remote_end_headers = false;
    bool stream1_remote_end_stream = false;
    bool stream1_remote_rst = false;
    bool stream2_remote_end_headers = false;
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

std::string iobuf_to_string(const fiber::mem::IoBuf &buf) {
    return std::string(reinterpret_cast<const char *>(buf.readable_data()), buf.readable());
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
    std::memcpy(out.data() + 9, payload.data(), payload.size());
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
    RecordingHttp2Connection connection(std::move(transport), options);
    connection.set_payload_error(payload_error);

    RunOutcome outcome;
    outcome.result = co_await connection.run();
    outcome.chunks = connection.chunks();
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
    fiber::async::spawn(group.at(0), [promise = std::move(promise), chunks = std::move(chunks), options,
                                      payload_error]() mutable {
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

class SendingHttp2Connection final : public fiber::http::Http2Connection {
public:
    SendingHttp2Connection(std::unique_ptr<fiber::http::HttpTransport> transport, FakeHttpTransport *fake_transport,
                           std::size_t expected_done, Options options = {}) :
        fiber::http::Http2Connection(std::move(transport), options), fake_transport_(fake_transport),
        expected_done_(expected_done) {}

    fiber::common::IoErr submit_stable_span(std::string_view data) noexcept {
        SendEntry *entry = acquire_send_entry();
        if (!entry) {
            return fiber::common::IoErr::NoMem;
        }
        entry->payload_ptr()->set_stable_span(reinterpret_cast<const std::uint8_t *>(data.data()), data.size());
        entry->total_bytes = data.size();
        entry->on_done = &SendingHttp2Connection::handle_done;
        entry->user_data = this;
        fiber::common::IoErr err = enqueue_send_entry(entry);
        if (err != fiber::common::IoErr::None) {
            release_send_entry(entry);
        }
        return err;
    }

    fiber::common::IoErr submit_buf(fiber::mem::IoBuf &&buf) noexcept {
        SendEntry *entry = acquire_send_entry();
        if (!entry) {
            return fiber::common::IoErr::NoMem;
        }
        entry->payload_ptr()->set_buf(std::move(buf));
        entry->total_bytes = entry->payload_ptr()->readable_bytes();
        entry->on_done = &SendingHttp2Connection::handle_done;
        entry->user_data = this;
        fiber::common::IoErr err = enqueue_send_entry(entry);
        if (err != fiber::common::IoErr::None) {
            release_send_entry(entry);
        }
        return err;
    }

    fiber::common::IoErr submit_chain(fiber::mem::IoBufChain &&bufs) noexcept {
        SendEntry *entry = acquire_send_entry();
        if (!entry) {
            return fiber::common::IoErr::NoMem;
        }
        entry->payload_ptr()->set_chain(std::move(bufs));
        entry->total_bytes = entry->payload_ptr()->readable_bytes();
        entry->on_done = &SendingHttp2Connection::handle_done;
        entry->user_data = this;
        fiber::common::IoErr err = enqueue_send_entry(entry);
        if (err != fiber::common::IoErr::None) {
            release_send_entry(entry);
        }
        return err;
    }

    void request_stop(fiber::common::IoErr reason = fiber::common::IoErr::Canceled) noexcept { shutdown(reason); }

    [[nodiscard]] bool done() const noexcept { return done_; }
    [[nodiscard]] bool send_loop_stopped() const noexcept { return send_loop_exited(); }
    fiber::async::Task<void> stop_and_join() noexcept { co_await stop_and_join_send_loop(); }

    SendOutcome snapshot() const {
        SendOutcome outcome;
        outcome.completions = completions_;
        if (fake_transport_) {
            outcome.written = fake_transport_->written();
        }
        return outcome;
    }

private:
    static void handle_done(void *user_data, std::size_t total_bytes, std::size_t written_bytes, std::size_t,
                            std::size_t, fiber::common::IoErr result) noexcept {
        auto *self = static_cast<SendingHttp2Connection *>(user_data);
        self->record_done(total_bytes, written_bytes, result);
    }

    void record_done(std::size_t total_bytes, std::size_t written_bytes, fiber::common::IoErr result) noexcept {
        completions_.push_back({total_bytes, written_bytes, result});
        if (completions_.size() >= expected_done_) {
            done_ = true;
        }
    }

    FakeHttpTransport *fake_transport_ = nullptr;
    std::size_t expected_done_ = 0;
    bool done_ = false;
    std::vector<CompletedSend> completions_;
};

class ControlHttp2Connection final : public fiber::http::Http2Connection {
public:
    ControlHttp2Connection(std::unique_ptr<fiber::http::HttpTransport> transport, FakeHttpTransport *fake_transport,
                           Options options = {}) :
        fiber::http::Http2Connection(std::move(transport), options), fake_transport_(fake_transport) {}

    fiber::http::Http2Stream *open_stream(std::uint32_t stream_id) noexcept { return create_local_stream(stream_id); }
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
    [[nodiscard]] bool current_stream_remote_end_headers(std::uint32_t stream_id) const noexcept {
        const fiber::http::Http2Stream *stream = find_stream(stream_id);
        return stream ? stream->remote_end_headers() : false;
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
    [[nodiscard]] bool send_loop_stopped() const noexcept { return send_loop_exited(); }
    fiber::async::Task<void> stop_and_join() noexcept { co_await stop_and_join_send_loop(); }
    [[nodiscard]] const std::string &written() const noexcept { return fake_transport_->written(); }

private:
    FakeHttpTransport *fake_transport_ = nullptr;
};

using SendScript = std::function<fiber::common::IoErr(SendingHttp2Connection &)>;
using ControlScript = std::function<void(ControlHttp2Connection &, fiber::http::Http2Stream &, fiber::http::Http2Stream &)>;

DetachedTask run_http2_connection_task(fiber::http::Http2Connection *connection,
                                       std::shared_ptr<std::atomic_bool> done = nullptr) {
    if (!connection) {
        co_return;
    }
    (void)co_await connection->run();
    if (done) {
        done->store(true, std::memory_order_release);
    }
}

struct ControlSetupContext {
    ControlHttp2Connection *connection = nullptr;
    FakeHttpTransport *fake_transport = nullptr;
    ControlScript setup;
    bool open_streams_for_setup = true;
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
    const bool strip_initial_flight = ctx.connection->current_role() == fiber::http::Http2Connection::ConnectionRole::Client &&
                                      ctx.strip_initial_flight;
    outcome.written = strip_initial_flight ? strip_client_initial_flight(ctx.connection->written())
                                           : ctx.connection->written();
    outcome.state = ctx.connection->current_state();
    outcome.transport_close_count = ctx.fake_transport->close_count();
    outcome.conn_send_window = ctx.connection->current_connection_send_window();
    outcome.peer_max_frame_size = ctx.connection->current_peer_max_frame_size();
    outcome.peer_max_concurrent_streams = ctx.connection->current_peer_max_concurrent_streams();
    outcome.peer_enable_push = ctx.connection->current_peer_enable_push();
    if (*ctx.stream1_id != 0) {
        outcome.stream1_send_window = ctx.connection->current_stream_send_window(*ctx.stream1_id);
        outcome.stream1_registered = ctx.connection->current_has_stream(*ctx.stream1_id);
        outcome.stream1_remote_end_headers = ctx.connection->current_stream_remote_end_headers(*ctx.stream1_id);
        outcome.stream1_remote_end_stream = ctx.connection->current_stream_remote_end_stream(*ctx.stream1_id);
        outcome.stream1_remote_rst = ctx.connection->current_stream_remote_rst(*ctx.stream1_id);
    }
    outcome.stream2_registered = ctx.connection->current_has_stream(2);
    outcome.stream2_remote_end_headers = ctx.connection->current_stream_remote_end_headers(2);
    outcome.stream2_remote_end_stream = ctx.connection->current_stream_remote_end_stream(2);
    if (*ctx.stream3_id != 0) {
        outcome.stream3_registered = ctx.connection->current_has_stream(*ctx.stream3_id);
    }
}

DetachedTask run_control_setup_task(ControlSetupContext ctx) {
    if (!ctx.connection) {
        co_return;
    }

    if (ctx.block_reads_for_setup) {
        if (ctx.open_streams_for_setup) {
            for (int i = 0; i < 20 && (!*ctx.stream1 || !*ctx.stream3); ++i) {
                if (!*ctx.stream1) {
                    set_control_stream_slot(ctx.stream1, ctx.stream1_id, ctx.connection->open_stream(1));
                }
                if (!*ctx.stream3) {
                    set_control_stream_slot(ctx.stream3, ctx.stream3_id, ctx.connection->open_stream(3));
                }
                if (!*ctx.stream1 || !*ctx.stream3) {
                    co_await fiber::async::sleep(std::chrono::milliseconds(1));
                }
            }
            if (!*ctx.stream1 || !*ctx.stream3) {
                ctx.connection->request_stop(fiber::common::IoErr::Invalid);
                ctx.fake_transport->release_reads();
                co_return;
            }
        } else {
            set_control_stream_slot(ctx.stream1, ctx.stream1_id, ctx.local_stream1);
            set_control_stream_slot(ctx.stream3, ctx.stream3_id, ctx.local_stream3);
        }
        ctx.setup(*ctx.connection, **ctx.stream1, **ctx.stream3);
        ctx.fake_transport->release_reads();
    } else if (ctx.setup) {
        ctx.setup(*ctx.connection, *ctx.local_stream1, *ctx.local_stream3);
        set_control_stream_slot(ctx.stream1, ctx.stream1_id, ctx.local_stream1);
        set_control_stream_slot(ctx.stream3, ctx.stream3_id, ctx.local_stream3);
    }

    if (ctx.snapshot_before_shutdown) {
        co_await fiber::async::sleep(std::chrono::milliseconds(5));
        capture_control_outcome(ctx);
        ctx.connection->request_stop();
    }
}

DetachedTask run_send_connection(std::shared_ptr<std::promise<SendOutcome>> promise, std::vector<size_t> write_steps,
                                 std::size_t expected_done, SendScript submit,
                                 fiber::http::Http2Connection::Options options = {}) {
    options.role = fiber::http::Http2Connection::ConnectionRole::Server;
    auto transport = std::make_unique<FakeHttpTransport>(std::vector<std::string>{}, std::move(write_steps), true);
    auto *fake_transport = transport.get();
    SendingHttp2Connection connection(std::move(transport), fake_transport, expected_done, options);
    auto run_done = std::make_shared<std::atomic_bool>(false);
    fiber::async::spawn([connection = &connection, run_done]() { return run_http2_connection_task(connection, run_done); });
    co_await fiber::async::sleep(std::chrono::milliseconds(1));

    SendOutcome outcome;
    outcome.submit_error = submit(connection);
    if (outcome.submit_error != fiber::common::IoErr::None) {
        promise->set_value(std::move(outcome));
        fiber::event::EventLoop::current().stop();
        co_return;
    }

    while (!connection.done()) {
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

SendOutcome execute_send_connection(SendScript submit, std::size_t expected_done, std::vector<size_t> write_steps = {},
                                    fiber::http::Http2Connection::Options options = {}) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<SendOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise = std::move(promise), write_steps = std::move(write_steps), expected_done,
                                      submit = std::move(submit), options]() mutable {
        return run_send_connection(std::move(promise), std::move(write_steps), expected_done, std::move(submit), options);
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

DetachedTask run_control_connection(std::shared_ptr<std::promise<ControlRunOutcome>> promise, std::vector<std::string> chunks,
                                    ControlScript setup, fiber::http::Http2Connection::Options options = {},
                                    bool preserve_initial_flight = false, bool open_streams_for_setup = true,
                                    bool snapshot_before_shutdown = false) {
    const bool block_reads_for_setup = static_cast<bool>(setup) &&
                                       options.role == fiber::http::Http2Connection::ConnectionRole::Client;
    auto transport = std::make_unique<FakeHttpTransport>(std::move(chunks), std::vector<size_t>{}, block_reads_for_setup,
                                                         snapshot_before_shutdown);
    auto *fake_transport = transport.get();
    ControlHttp2Connection connection(std::move(transport), fake_transport, options);
    fiber::http::Http2Stream *stream1 = nullptr;
    fiber::http::Http2Stream *stream3 = nullptr;
    std::uint32_t stream1_id = 0;
    std::uint32_t stream3_id = 0;
    ControlRunOutcome outcome;
    fiber::http::Http2Stream::Lease local_stream1 = fiber::http::Http2Stream::alloc(1);
    fiber::http::Http2Stream::Lease local_stream3 = fiber::http::Http2Stream::alloc(3);

    if (!local_stream1 || !local_stream3) {
        outcome.result = std::unexpected(fiber::common::IoErr::NoMem);
        promise->set_value(std::move(outcome));
        fiber::event::EventLoop::current().stop();
        co_return;
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
                                      open_streams_for_setup,
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

        outcome.result = co_await connection.run();
        promise->set_value(std::move(outcome));
        fiber::event::EventLoop::current().stop();
        co_return;
    }

    if (block_reads_for_setup) {
        ControlSetupContext setup_ctx{&connection,
                                      fake_transport,
                                      setup,
                                      open_streams_for_setup,
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
        outcome.result = co_await connection.run();
    } else {
        if (setup) {
            setup(connection, *local_stream1, *local_stream3);
            stream1 = local_stream1.get();
            stream3 = local_stream3.get();
            stream1_id = local_stream1->stream_id();
            stream3_id = local_stream3->stream_id();
        }
        outcome.result = co_await connection.run();
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
                                             bool preserve_initial_flight = false,
                                             bool open_streams_for_setup = true,
                                             bool snapshot_before_shutdown = false) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ControlRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0),
                        [promise = std::move(promise), chunks = std::move(chunks), setup = std::move(setup), options,
                         preserve_initial_flight, open_streams_for_setup, snapshot_before_shutdown]() mutable {
                            return run_control_connection(std::move(promise), std::move(chunks), std::move(setup), options,
                                                          preserve_initial_flight, open_streams_for_setup,
                                                          snapshot_before_shutdown);
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

DetachedTask run_retained_stream_connection(std::shared_ptr<std::promise<RetainedStreamOutcome>> promise,
                                            fiber::http::Http2Connection::Options options = {}) {
    auto transport = std::make_unique<FakeHttpTransport>(std::vector<std::string>{}, std::vector<size_t>{}, true, false);
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
    outcome.result = co_await connection.run();
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
            {settings},
            [](ControlHttp2Connection &, fiber::http::Http2Stream &, fiber::http::Http2Stream &) {},
            options, true, true, true);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_EQ(outcome.peer_max_frame_size, 32768U);
    EXPECT_EQ(outcome.stream1_send_window, 70000);
    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    ASSERT_EQ(frames.size(), 1U) << describe_frames(frames);
    EXPECT_EQ(frames[0].type, 0x4);
    EXPECT_EQ(frames[0].flags, 0x1);
    EXPECT_EQ(frames[0].stream_id, 0U);
    EXPECT_TRUE(frames[0].payload.empty());
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
            [](ControlHttp2Connection &, fiber::http::Http2Stream &, fiber::http::Http2Stream &) {},
            options, true, true, true);

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
            [](ControlHttp2Connection &, fiber::http::Http2Stream &, fiber::http::Http2Stream &) {},
            options, true);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_FALSE(outcome.stream1_registered);
}

TEST(Http2ConnectionTest, HeadersCreatePeerStreamAndOpenIt) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    ControlRunOutcome outcome = execute_control_connection({make_frame(3, 0x1, 0x4, 2, "abc")}, {}, options, false, true, true);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_TRUE(outcome.stream2_registered);
    EXPECT_TRUE(outcome.stream2_remote_end_headers);
    EXPECT_FALSE(outcome.stream2_remote_end_stream);
}

TEST(Http2ConnectionTest, HeadersWithContinuationCompleteExistingLocalStreamHeaders) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    ControlRunOutcome outcome = execute_control_connection(
            {make_frame(3, 0x1, 0x0, 1, "abc"), make_frame(2, 0x9, 0x4, 1, "de")},
            [](ControlHttp2Connection &, fiber::http::Http2Stream &, fiber::http::Http2Stream &) {}, options, false,
            true, true);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_TRUE(outcome.stream1_registered);
    EXPECT_TRUE(outcome.stream1_remote_end_headers);
    EXPECT_FALSE(outcome.stream1_remote_end_stream);
}

TEST(Http2ConnectionTest, HeadersWithEndStreamCreateHalfClosedRemoteStream) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    ControlRunOutcome outcome = execute_control_connection({make_frame(0, 0x1, 0x5, 2, "")}, {}, options, false, true, true);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_TRUE(outcome.stream2_registered);
    EXPECT_TRUE(outcome.stream2_remote_end_headers);
    EXPECT_TRUE(outcome.stream2_remote_end_stream);
}

TEST(Http2ConnectionTest, HeadersWithEndStreamContinuationCloseRemoteStreamAfterBlockComplete) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    ControlRunOutcome outcome = execute_control_connection(
            {make_frame(3, 0x1, 0x1, 2, "abc"), make_frame(2, 0x9, 0x4, 2, "de")}, {}, options, false, true, true);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_TRUE(outcome.stream2_registered);
    EXPECT_TRUE(outcome.stream2_remote_end_headers);
    EXPECT_TRUE(outcome.stream2_remote_end_stream);
}

TEST(Http2ConnectionTest, TrailerHeadersRequireEndStream) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    ControlRunOutcome outcome = execute_control_connection(
            {make_frame(3, 0x1, 0x4, 2, "abc"), make_frame(2, 0x1, 0x4, 2, "tr")}, {}, options);

    ASSERT_FALSE(outcome.result.has_value());
    EXPECT_EQ(outcome.result.error(), fiber::common::IoErr::Invalid);
}

TEST(Http2ConnectionTest, TrailerHeadersWithContinuationCloseRemoteStream) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    ControlRunOutcome outcome = execute_control_connection(
            {make_frame(3, 0x1, 0x4, 2, "abc"), make_frame(2, 0x1, 0x1, 2, "tr"), make_frame(2, 0x9, 0x4, 2, "ai")},
            {}, options, false, true, true);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_TRUE(outcome.stream2_registered);
    EXPECT_TRUE(outcome.stream2_remote_end_headers);
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
            [](ControlHttp2Connection &, fiber::http::Http2Stream &, fiber::http::Http2Stream &) {},
            options, true, true, true);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_TRUE(outcome.stream1_registered);
    EXPECT_FALSE(outcome.stream1_remote_end_headers);
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
    EXPECT_EQ(frames[1].type, 0x8);
    EXPECT_EQ(frames[1].stream_id, 0U);
    EXPECT_EQ(frames[1].length, 4U);

    ASSERT_EQ(frames[1].payload.size(), 4U);
    std::uint32_t increment = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(frames[1].payload[0]) & 0x7fU) << 24) |
                              (static_cast<std::uint32_t>(static_cast<std::uint8_t>(frames[1].payload[1])) << 16) |
                              (static_cast<std::uint32_t>(static_cast<std::uint8_t>(frames[1].payload[2])) << 8) |
                              static_cast<std::uint32_t>(static_cast<std::uint8_t>(frames[1].payload[3]));
    EXPECT_EQ(increment, 0x7fffffffU - 65535U);
}

TEST(Http2ConnectionTest, ServerConnectionPrefaceSendsSettingsAndWindowUpdateAfterPeerPreface) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Server;
    options.max_frame_size = 0x00ffffffU;
    options.local_max_concurrent_streams = 128;
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
    EXPECT_EQ(frames[1].type, 0x8);
    EXPECT_EQ(frames[1].stream_id, 0U);
    EXPECT_EQ(frames[1].length, 4U);
}

TEST(Http2ConnectionTest, LocalStreamCreationRequiresRun) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;

    auto transport = std::make_unique<FakeHttpTransport>(std::vector<std::string>{});
    auto *fake_transport = transport.get();
    ControlHttp2Connection connection(std::move(transport), fake_transport, options);

    EXPECT_EQ(connection.open_stream(1), nullptr);
}

TEST(Http2ConnectionTest, GracefulShutdownSendsGoawayAndClosesTransportAfterQueueDrains) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.initial_connection_recv_window = 65535;

    ControlRunOutcome outcome = execute_control_connection(
            {},
            [](ControlHttp2Connection &connection, fiber::http::Http2Stream &, fiber::http::Http2Stream &) {
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
            [](SendingHttp2Connection &connection) {
                return connection.submit_stable_span("hello world");
            },
            1, {3, 4, 4});

    ASSERT_EQ(outcome.submit_error, fiber::common::IoErr::None);
    ASSERT_EQ(outcome.completions.size(), 1U);
    EXPECT_EQ(outcome.completions[0].total_bytes, 11U);
    EXPECT_EQ(outcome.completions[0].written_bytes, 11U);
    EXPECT_EQ(outcome.completions[0].result, fiber::common::IoErr::None);
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

                fiber::mem::IoBufChain chain;
                if (!chain.append(std::move(first))) {
                    return fiber::common::IoErr::NoMem;
                }
                if (!chain.append(std::move(second))) {
                    return fiber::common::IoErr::NoMem;
                }
                return connection.submit_chain(std::move(chain));
            },
            1, {4, 2});

    ASSERT_EQ(outcome.submit_error, fiber::common::IoErr::None);
    ASSERT_EQ(outcome.completions.size(), 1U);
    EXPECT_EQ(outcome.completions[0].total_bytes, 6U);
    EXPECT_EQ(outcome.completions[0].written_bytes, 6U);
    EXPECT_EQ(outcome.completions[0].result, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.written, "abcdef");
}

TEST(Http2ConnectionTest, ClosingSendingNotifiesQueuedEntries) {
    SendOutcome outcome = execute_send_connection(
            [](SendingHttp2Connection &connection) {
                fiber::common::IoErr err = connection.submit_stable_span("first");
                if (err != fiber::common::IoErr::None) {
                    return err;
                }
                err = connection.submit_stable_span("second");
                if (err != fiber::common::IoErr::None) {
                    return err;
                }
                connection.request_stop(fiber::common::IoErr::Canceled);
                return fiber::common::IoErr::None;
            },
            2);

    ASSERT_EQ(outcome.submit_error, fiber::common::IoErr::None);
    ASSERT_EQ(outcome.completions.size(), 2U);
    EXPECT_EQ(outcome.completions[0].result, fiber::common::IoErr::Canceled);
    EXPECT_EQ(outcome.completions[0].written_bytes, 0U);
    EXPECT_EQ(outcome.completions[1].result, fiber::common::IoErr::Canceled);
    EXPECT_EQ(outcome.completions[1].written_bytes, 0U);
    EXPECT_TRUE(outcome.written.empty());
}
