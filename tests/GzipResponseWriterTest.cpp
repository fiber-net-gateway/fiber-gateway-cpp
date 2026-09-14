#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/common/IoError.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/GzipResponseWriter.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpResponseWriter.h>
#include <fiber/net/SocketAddress.h>

#include "support/ZlibReference.h"

namespace {

using namespace std::chrono_literals;

using fiber::async::DetachedTask;
using fiber::async::Task;
using fiber::common::IoErr;
using fiber::common::IoResult;
using fiber::http::GzipResponseDecision;
using fiber::http::GzipResponseWriter;
using fiber::http::GzipResponseWriterOptions;
using fiber::http::GzipResponseWriterStats;
using fiber::http::HttpBodySpec;
using fiber::http::HttpExchange;
using fiber::http::HttpHeaders;
using fiber::http::HttpResponseWriter;
using fiber::http::OutgoingHeaderBlockView;
using fiber::http::OutgoingHeaderKind;
using fiber::mem::IoBuf;
using fiber::mem::IoBufChain;
using fiber::mem::IoBufNodePool;
using fiber::net::SocketAddress;

// ---- payloads and reference streams ----

std::string repeat_pattern(std::size_t total, std::string_view pattern) {
    std::string out;
    out.reserve(total);
    while (out.size() < total) {
        out.append(pattern.substr(0, std::min(pattern.size(), total - out.size())));
    }
    return out;
}

std::string html_payload(std::size_t target) {
    return repeat_pattern(target, "<div class=\"row\"><span>value</span><p>lorem ipsum dolor</p></div>\n");
}

std::string random_bytes(std::size_t n, std::uint32_t seed) {
    std::mt19937 gen(seed);
    std::uniform_int_distribution<int> dist(0, 255);
    std::string out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(static_cast<char>(dist(gen)));
    }
    return out;
}

std::string reference_gzip(std::string_view input, int level) {
    const fiber::test::ZlibReferenceResult r = fiber::test::zlib_reference_gzip(input, level);
    EXPECT_TRUE(r.ok) << "reference gzip status " << r.z_status;
    return r.output;
}

// Reference member for a write bursts-with-optional-flush/finish operation
// sequence, mirroring what the writer feeds its encoder. Each step is an input
// burst plus whether a sync flush follows it.
std::string reference_gzip_sequence(int level, const std::vector<std::pair<std::string_view, bool>> &steps) {
    fiber::test::ZlibReferenceDeflate deflater(level, 15 + 16);
    std::string out;
    for (const auto &[burst, flush_after]: steps) {
        std::size_t offset = 0;
        while (offset < burst.size()) {
            const fiber::test::ZlibReferenceResult r =
                    deflater.step(burst.substr(offset), 16384, fiber::test::ZlibReferenceFlush::None);
            EXPECT_TRUE(r.ok);
            out += r.output;
            offset += r.consumed;
            if (r.consumed == 0 && r.output.empty()) {
                ADD_FAILURE() << "reference deflate stalled with input left";
                return out;
            }
        }
        if (flush_after) {
            for (;;) {
                const fiber::test::ZlibReferenceResult r =
                        deflater.step({}, 16384, fiber::test::ZlibReferenceFlush::Sync);
                EXPECT_TRUE(r.ok);
                out += r.output;
                if (r.output.empty()) {
                    break;
                }
            }
        }
    }
    for (;;) {
        const fiber::test::ZlibReferenceResult r = deflater.step({}, 16384, fiber::test::ZlibReferenceFlush::Finish);
        EXPECT_TRUE(r.ok);
        out += r.output;
        if (r.stream_end) {
            return out;
        }
    }
}

// Drain an inflater over the given bytes, tolerating a mid-stream stop.
std::string inflate_available(fiber::test::ZlibReferenceInflate &inflate, std::string_view input) {
    std::string out;
    std::size_t offset = 0;
    while (offset < input.size() && !inflate.stream_end()) {
        const fiber::test::ZlibReferenceResult r = inflate.step(input.substr(offset), 4096);
        EXPECT_TRUE(r.ok) << "reference inflate status " << r.z_status;
        out += r.output;
        offset += r.consumed;
        if (r.consumed == 0 && r.output.empty()) {
            break;
        }
    }
    return out;
}

// ---- capture sink ----

struct CapturedHeader {
    OutgoingHeaderKind kind = OutgoingHeaderKind::Final;
    int status_code = 0;
    bool end_stream = false;
    HttpBodySpec body{};
    std::vector<std::pair<std::string, std::string>> fields;
};

std::optional<std::string> field_value(const CapturedHeader &header, std::string_view name) {
    for (const auto &field: header.fields) {
        if (field.first == name) {
            return field.second;
        }
    }
    return std::nullopt;
}

std::size_t field_count(const CapturedHeader &header, std::string_view name) {
    std::size_t count = 0;
    for (const auto &field: header.fields) {
        if (field.first == name) {
            ++count;
        }
    }
    return count;
}

// Terminal sink: records every header block, body byte and control op the
// decorated writer chain emits, with optional write-failure injection.
class CaptureSink {
public:
    struct WriteCall {
        std::size_t len;
        bool end;
    };

    std::vector<CapturedHeader> headers;
    std::string body;
    std::vector<WriteCall> writes;
    int flush_count = 0;
    int abort_count = 0;
    std::optional<IoErr> fail_writes_with;
    std::optional<IoErr> abort_reason;

    HttpResponseWriter writer() noexcept {
        static const HttpResponseWriter::Ops kOps{
                &CaptureSink::on_send_header, &CaptureSink::on_write_all_chain, &CaptureSink::on_write_all_bytes,
                &CaptureSink::on_write_chain, &CaptureSink::on_write_bytes,     &CaptureSink::on_flush,
                &CaptureSink::on_abort,
        };
        return {this, kOps};
    }

    bool ended() const noexcept {
        for (const WriteCall &call: writes) {
            if (call.end) {
                return true;
            }
        }
        return false;
    }

private:
    bool note_write(const std::uint8_t *buf, std::size_t len, bool end) {
        if (fail_writes_with.has_value()) {
            return false;
        }
        body.append(reinterpret_cast<const char *>(buf), len);
        writes.push_back(WriteCall{len, end});
        return true;
    }

    void note_chain(IoBufChain &chunk, bool end) {
        while (const IoBuf *buf = chunk.first_readable()) {
            const std::size_t readable = buf->readable();
            if (!note_write(buf->readable_data(), readable, end)) {
                return;
            }
            chunk.consume(readable);
        }
    }

    static Task<IoResult<void>> on_send_header(void *ctx, const OutgoingHeaderBlockView &header,
                                               std::chrono::milliseconds) {
        auto *self = static_cast<CaptureSink *>(ctx);
        CapturedHeader captured;
        captured.kind = header.kind;
        captured.status_code = header.status_code;
        captured.end_stream = header.end_stream;
        captured.body = header.body;
        if (header.headers != nullptr) {
            for (const auto &field: *header.headers) {
                captured.fields.emplace_back(std::string(field.name_view()), std::string(field.value_view()));
            }
        }
        self->headers.push_back(std::move(captured));
        co_return IoResult<void>{};
    }

    static Task<IoResult<std::size_t>> on_write_all_chain(void *ctx, IoBufChain chunk,
                                                          std::chrono::milliseconds) noexcept {
        auto *self = static_cast<CaptureSink *>(ctx);
        if (self->fail_writes_with.has_value()) {
            co_return std::unexpected(*self->fail_writes_with);
        }
        const std::size_t intended = chunk.readable_bytes();
        self->note_chain(chunk, chunk.complete());
        co_return intended;
    }

    static Task<IoResult<std::size_t>> on_write_all_bytes(void *ctx, const std::uint8_t *buf, std::size_t len, bool end,
                                                          std::chrono::milliseconds) noexcept {
        auto *self = static_cast<CaptureSink *>(ctx);
        if (!self->note_write(buf, len, end)) {
            co_return std::unexpected(*self->fail_writes_with);
        }
        co_return len;
    }

    static Task<IoResult<std::size_t>> on_write_chain(void *ctx, IoBufChain &chunk,
                                                      std::chrono::milliseconds) noexcept {
        auto *self = static_cast<CaptureSink *>(ctx);
        if (self->fail_writes_with.has_value()) {
            co_return std::unexpected(*self->fail_writes_with);
        }
        const std::size_t intended = chunk.readable_bytes();
        self->note_chain(chunk, chunk.complete());
        co_return intended;
    }

    static Task<IoResult<std::size_t>> on_write_bytes(void *ctx, const std::uint8_t *buf, std::size_t len, bool end,
                                                      std::chrono::milliseconds) noexcept {
        auto *self = static_cast<CaptureSink *>(ctx);
        if (!self->note_write(buf, len, end)) {
            co_return std::unexpected(*self->fail_writes_with);
        }
        co_return len;
    }

    static Task<IoResult<void>> on_flush(void *ctx, std::chrono::milliseconds) noexcept {
        ++static_cast<CaptureSink *>(ctx)->flush_count;
        co_return IoResult<void>{};
    }

    static IoResult<void> on_abort(void *ctx, IoErr reason) noexcept {
        auto *self = static_cast<CaptureSink *>(ctx);
        ++self->abort_count;
        self->abort_reason = reason;
        return {};
    }
};

// ---- scenario harness ----

// Runs a Task<void>-returning scenario lambda on a dedicated event loop and
// waits for its completion.
template<typename F>
DetachedTask scenario_task(F scenario, std::promise<bool> *done) {
    co_await scenario();
    done->set_value(true);
}

template<typename F>
void run_scenario(F &&scenario) {
    fiber::event::EventLoopGroup group(1);
    std::promise<bool> done;
    auto future = done.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return scenario_task(std::forward<F>(scenario), &done); });
    const auto status = future.wait_for(5s);
    group.stop();
    group.join();

    ASSERT_EQ(status, std::future_status::ready);
}

OutgoingHeaderBlockView final_header(const HttpHeaders *headers, int status, HttpBodySpec body,
                                     bool end_stream = false) noexcept {
    OutgoingHeaderBlockView view;
    view.kind = OutgoingHeaderKind::Final;
    view.status_code = status;
    view.headers = headers;
    view.body = body;
    view.end_stream = end_stream;
    return view;
}

GzipResponseWriterOptions active_options() {
    GzipResponseWriterOptions options;
    options.enabled = true;
    options.request_accepts_gzip = true;
    options.compression_level = 1;
    return options;
}

const std::uint8_t *bytes_of(const std::string &s) noexcept { return reinterpret_cast<const std::uint8_t *>(s.data()); }

} // namespace

// ---- active compression path ----

TEST(GzipResponseWriterTest, CompressesBodyAndRewritesHeaders) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;
    const std::string body = html_payload(1000);

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        headers.add("Content-Length", std::to_string(body.size()));
        headers.add("ETag", "\"abc123\"");
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::ContentLength(body.size())));
        EXPECT_TRUE(head.has_value());
        const auto written = co_await w.write_all(bytes_of(body), body.size(), true);
        EXPECT_TRUE(written.has_value());
        EXPECT_EQ(*written, body.size());
        stats = gzip.stats();
    });

    ASSERT_EQ(sink.headers.size(), 1u);
    const CapturedHeader &h = sink.headers[0];
    EXPECT_EQ(h.kind, OutgoingHeaderKind::Final);
    EXPECT_EQ(h.status_code, 200);
    EXPECT_FALSE(h.end_stream);
    EXPECT_TRUE(h.body.is_auto()) << "content-length framing must not survive compression";
    EXPECT_EQ(field_value(h, "Content-Encoding"), std::optional<std::string>("gzip"));
    EXPECT_EQ(field_value(h, "Vary"), std::optional<std::string>("Accept-Encoding"));
    EXPECT_EQ(field_value(h, "Content-Length"), std::nullopt);
    EXPECT_EQ(field_value(h, "ETag"), std::optional<std::string>("W/\"abc123\""));
    EXPECT_EQ(field_value(h, "Content-Type"), std::optional<std::string>("text/html"));

    EXPECT_EQ(sink.body, reference_gzip(body, 1));
    EXPECT_TRUE(sink.ended());
    EXPECT_EQ(sink.abort_count, 0);
    EXPECT_EQ(stats.decision, GzipResponseDecision::Completed);
    EXPECT_EQ(stats.input_bytes, body.size());
    EXPECT_EQ(stats.output_bytes, sink.body.size());
}

TEST(GzipResponseWriterTest, EmptyChunkedBodyProducesEmptyMember) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::Chunked()));
        EXPECT_TRUE(head.has_value());
        const auto written = co_await w.write_all(nullptr, 0, true);
        EXPECT_TRUE(written.has_value());
        EXPECT_EQ(*written, 0u);
        stats = gzip.stats();
    });

    EXPECT_EQ(sink.body, reference_gzip("", 1));
    EXPECT_EQ(sink.body.size(), 20u);
    EXPECT_TRUE(sink.ended());
    EXPECT_EQ(stats.decision, GzipResponseDecision::Completed);
    EXPECT_EQ(stats.input_bytes, 0u);
}

TEST(GzipResponseWriterTest, SyncFlushIsDecodableBeforeEnd) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;
    const std::string first = "part-one-";
    const std::string second = "part-two";
    std::string flushed_prefix;

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::Chunked()));
        EXPECT_TRUE(head.has_value());
        const auto written = co_await w.write_all(bytes_of(first), first.size(), false);
        EXPECT_TRUE(written.has_value());
        const auto flushed = co_await w.flush();
        EXPECT_TRUE(flushed.has_value());
        flushed_prefix = sink.body;
        EXPECT_GT(flushed_prefix.size(), 0u) << "flush must publish the compressed prefix";
        const auto done = co_await w.write_all(bytes_of(second), second.size(), true);
        EXPECT_TRUE(done.has_value());
        stats = gzip.stats();
    });

    // The flushed prefix must be recoverable without the trailer.
    fiber::test::ZlibReferenceInflate inflate(15 + 16);
    EXPECT_EQ(inflate_available(inflate, flushed_prefix), first);
    EXPECT_FALSE(inflate.stream_end());

    // The completed member matches the reference for the identical operation
    // sequence (write, flush, write, finish) and decodes to the full body.
    EXPECT_EQ(sink.body, reference_gzip_sequence(1, {{first, true}, {second, false}}));
    fiber::test::ZlibReferenceInflate whole(15 + 16);
    EXPECT_EQ(inflate_available(whole, sink.body), first + second);
    EXPECT_TRUE(whole.stream_end());

    EXPECT_EQ(sink.flush_count, 1);
    EXPECT_EQ(stats.decision, GzipResponseDecision::Completed);
    EXPECT_EQ(stats.input_bytes, first.size() + second.size());
}

TEST(GzipResponseWriterTest, FlushBeforeAnyInputEmitsNothing) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::Chunked()));
        EXPECT_TRUE(head.has_value());
        const auto flushed = co_await w.flush();
        EXPECT_TRUE(flushed.has_value());
        EXPECT_TRUE(sink.body.empty()) << "virgin flush must not start the member";
        const std::string data = "data";
        const auto written = co_await w.write_all(bytes_of(data), data.size(), true);
        EXPECT_TRUE(written.has_value());
        stats = gzip.stats();
    });

    EXPECT_EQ(sink.body, reference_gzip("data", 1));
    EXPECT_EQ(sink.flush_count, 1);
    EXPECT_EQ(stats.decision, GzipResponseDecision::Completed);
}

TEST(GzipResponseWriterTest, LargeSingleWriteCompressesCorrectly) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;
    const std::string body =
            repeat_pattern(600 * 1024, R"({"id":%d,"name":"fiber-gateway","tags":["http","quic"],"ok":true},)");

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::Chunked()));
        EXPECT_TRUE(head.has_value());
        const auto written = co_await w.write_all(bytes_of(body), body.size(), true);
        EXPECT_TRUE(written.has_value());
        EXPECT_EQ(*written, body.size());
        stats = gzip.stats();
    });

    EXPECT_EQ(sink.body, reference_gzip(body, 1));
    EXPECT_EQ(stats.decision, GzipResponseDecision::Completed);
    EXPECT_EQ(stats.input_bytes, body.size());
    EXPECT_EQ(stats.output_bytes, sink.body.size());
}

TEST(GzipResponseWriterTest, ChainWriteCompletesBody) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;
    const std::string part_a = html_payload(3000);
    const std::string part_b = html_payload(700);

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::Chunked()));
        EXPECT_TRUE(head.has_value());

        IoBufChain chain(node_pool);
        IoBuf a = IoBuf::allocate(4096);
        std::memcpy(a.writable_data(), part_a.data(), part_a.size());
        a.commit(part_a.size());
        EXPECT_TRUE(chain.append(std::move(a)));
        IoBuf b = IoBuf::allocate(4096);
        std::memcpy(b.writable_data(), part_b.data(), part_b.size());
        b.commit(part_b.size());
        EXPECT_TRUE(chain.append(std::move(b)));
        chain.mark_complete();

        const auto written = co_await w.write(chain);
        EXPECT_TRUE(written.has_value());
        EXPECT_EQ(*written, part_a.size() + part_b.size());
        EXPECT_EQ(chain.readable_bytes(), 0u);
        stats = gzip.stats();
    });

    EXPECT_EQ(sink.body, reference_gzip(part_a + part_b, 1));
    EXPECT_EQ(stats.decision, GzipResponseDecision::Completed);
    EXPECT_EQ(stats.input_bytes, part_a.size() + part_b.size());
}

TEST(GzipResponseWriterTest, CompressesConfiguredContentTypeAndKeepsWeakEtag) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    const std::vector<std::string> json_types{"application/json"};
    options.types = json_types;
    GzipResponseWriterStats stats;
    const std::string body = repeat_pattern(2000, R"({"ok":true,"n":42})");

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "application/json");
        headers.add("Content-Length", std::to_string(body.size()));
        headers.add("ETag", "W/\"v1\"");
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::ContentLength(body.size())));
        EXPECT_TRUE(head.has_value());
        const auto written = co_await w.write_all(bytes_of(body), body.size(), true);
        EXPECT_TRUE(written.has_value());
        stats = gzip.stats();
    });

    ASSERT_EQ(sink.headers.size(), 1u);
    const CapturedHeader &h = sink.headers[0];
    EXPECT_EQ(field_value(h, "Content-Encoding"), std::optional<std::string>("gzip"));
    EXPECT_EQ(field_value(h, "ETag"), std::optional<std::string>("W/\"v1\"")) << "weak ETag must stay untouched";
    EXPECT_EQ(sink.body, reference_gzip(body, 1));
    EXPECT_EQ(stats.decision, GzipResponseDecision::Completed);
}

TEST(GzipResponseWriterTest, ExistingVaryIsNotDuplicated) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;
    const std::string body = html_payload(300);

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        headers.add("Vary", "Accept-Encoding");
        headers.add("Content-Length", std::to_string(body.size()));
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::ContentLength(body.size())));
        EXPECT_TRUE(head.has_value());
        const auto written = co_await w.write_all(bytes_of(body), body.size(), true);
        EXPECT_TRUE(written.has_value());
        stats = gzip.stats();
    });

    ASSERT_EQ(sink.headers.size(), 1u);
    const CapturedHeader &h = sink.headers[0];
    EXPECT_EQ(field_count(h, "Vary"), 1u);
    EXPECT_EQ(field_value(h, "Vary"), std::optional<std::string>("Accept-Encoding"));
    EXPECT_EQ(field_value(h, "Content-Encoding"), std::optional<std::string>("gzip"));
    EXPECT_EQ(sink.body, reference_gzip(body, 1));
}

TEST(GzipResponseWriterTest, InformationalHeaderPassesThroughBeforeFinal) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;
    const std::string body = html_payload(200);

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();

        OutgoingHeaderBlockView informational;
        informational.kind = OutgoingHeaderKind::Informational;
        informational.status_code = 100;
        const auto early = co_await w.send_header(informational);
        EXPECT_TRUE(early.has_value());

        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        headers.add("Content-Length", std::to_string(body.size()));
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::ContentLength(body.size())));
        EXPECT_TRUE(head.has_value()) << "final header after informational must still be accepted";
        const auto written = co_await w.write_all(bytes_of(body), body.size(), true);
        EXPECT_TRUE(written.has_value());
        stats = gzip.stats();
    });

    ASSERT_EQ(sink.headers.size(), 2u);
    EXPECT_EQ(sink.headers[0].kind, OutgoingHeaderKind::Informational);
    EXPECT_EQ(sink.headers[0].status_code, 100);
    EXPECT_EQ(sink.headers[1].kind, OutgoingHeaderKind::Final);
    EXPECT_EQ(sink.body, reference_gzip(body, 1));
    EXPECT_EQ(stats.decision, GzipResponseDecision::Completed);
}

TEST(GzipResponseWriterTest, TrailerHeaderFinishesActiveBody) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;
    const std::string body = html_payload(4000);

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::Chunked()));
        EXPECT_TRUE(head.has_value());
        const auto written = co_await w.write_all(bytes_of(body), body.size(), false);
        EXPECT_TRUE(written.has_value());

        HttpHeaders trailers(exchange.pool());
        trailers.add("X-Checksum", "deadbeef");
        OutgoingHeaderBlockView trailer;
        trailer.kind = OutgoingHeaderKind::Trailer;
        trailer.headers = &trailers;
        const auto sent = co_await w.send_header(trailer);
        EXPECT_TRUE(sent.has_value());
        stats = gzip.stats();

        const auto late = co_await w.write_all(bytes_of(body), 1, true);
        EXPECT_FALSE(late.has_value());
        EXPECT_EQ(late.error(), IoErr::Already);
    });

    ASSERT_EQ(sink.headers.size(), 2u);
    EXPECT_EQ(sink.headers[1].kind, OutgoingHeaderKind::Trailer);
    EXPECT_EQ(field_value(sink.headers[1], "X-Checksum"), std::optional<std::string>("deadbeef"));
    EXPECT_EQ(sink.body, reference_gzip(body, 1));
    EXPECT_EQ(stats.decision, GzipResponseDecision::Completed);
    EXPECT_EQ(stats.input_bytes, body.size());
}

// ---- bypass paths ----

TEST(GzipResponseWriterTest, BypassWithoutAcceptAddsVaryOnly) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    options.request_accepts_gzip = false;
    GzipResponseWriterStats stats;
    const std::string body = html_payload(1000);

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        headers.add("Content-Length", std::to_string(body.size()));
        headers.add("ETag", "\"abc123\"");
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::ContentLength(body.size())));
        EXPECT_TRUE(head.has_value());
        const auto written = co_await w.write_all(bytes_of(body), body.size(), true);
        EXPECT_TRUE(written.has_value());
        stats = gzip.stats();
    });

    ASSERT_EQ(sink.headers.size(), 1u);
    const CapturedHeader &h = sink.headers[0];
    EXPECT_EQ(field_value(h, "Vary"), std::optional<std::string>("Accept-Encoding")) << "still a compressible body";
    EXPECT_EQ(field_value(h, "Content-Encoding"), std::nullopt);
    EXPECT_EQ(field_value(h, "Content-Length"), std::optional<std::string>("1000"));
    EXPECT_EQ(field_value(h, "ETag"), std::optional<std::string>("\"abc123\"")) << "no transform, no weakening";
    EXPECT_TRUE(h.body.is_content_length());
    EXPECT_EQ(h.body.content_length(), body.size());

    EXPECT_EQ(sink.body, body) << "bypass must pass the body through verbatim";
    EXPECT_TRUE(sink.ended());
    EXPECT_EQ(stats.decision, GzipResponseDecision::Bypassed);
    EXPECT_EQ(stats.input_bytes, 0u);
    EXPECT_EQ(stats.output_bytes, 0u);
}

TEST(GzipResponseWriterTest, BypassesBodylessStatusWithoutVary) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        const auto head = co_await w.send_header(final_header(&headers, 204, HttpBodySpec::None(), true));
        EXPECT_TRUE(head.has_value());
        stats = gzip.stats();
    });

    ASSERT_EQ(sink.headers.size(), 1u);
    const CapturedHeader &h = sink.headers[0];
    EXPECT_EQ(h.status_code, 204);
    EXPECT_TRUE(h.end_stream);
    EXPECT_TRUE(h.body.is_none());
    EXPECT_EQ(field_value(h, "Vary"), std::nullopt) << "204 is never an intrinsic candidate";
    EXPECT_EQ(field_value(h, "Content-Type"), std::optional<std::string>("text/html"));
    EXPECT_TRUE(sink.body.empty());
    EXPECT_EQ(stats.decision, GzipResponseDecision::Bypassed);
}

TEST(GzipResponseWriterTest, BypassesAlreadyEncodedResponse) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;
    const std::string body = reference_gzip(html_payload(500), 6);

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        headers.add("Content-Encoding", "gzip");
        headers.add("Content-Length", std::to_string(body.size()));
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::ContentLength(body.size())));
        EXPECT_TRUE(head.has_value());
        const auto written = co_await w.write_all(bytes_of(body), body.size(), true);
        EXPECT_TRUE(written.has_value());
        stats = gzip.stats();
    });

    ASSERT_EQ(sink.headers.size(), 1u);
    const CapturedHeader &h = sink.headers[0];
    EXPECT_EQ(field_value(h, "Content-Encoding"), std::optional<std::string>("gzip")) << "left untouched";
    EXPECT_EQ(field_value(h, "Vary"), std::nullopt);
    EXPECT_EQ(field_value(h, "Content-Length"), std::optional<std::string>(std::to_string(body.size())));
    EXPECT_EQ(sink.body, body);
    EXPECT_EQ(stats.decision, GzipResponseDecision::Bypassed);
}

TEST(GzipResponseWriterTest, BypassesNoTransformResponse) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;
    const std::string body = html_payload(600);

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        headers.add("Cache-Control", "no-transform");
        headers.add("Content-Length", std::to_string(body.size()));
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::ContentLength(body.size())));
        EXPECT_TRUE(head.has_value());
        const auto written = co_await w.write_all(bytes_of(body), body.size(), true);
        EXPECT_TRUE(written.has_value());
        stats = gzip.stats();
    });

    ASSERT_EQ(sink.headers.size(), 1u);
    const CapturedHeader &h = sink.headers[0];
    EXPECT_EQ(field_value(h, "Vary"), std::nullopt) << "no-transform is not an intrinsic candidate";
    EXPECT_EQ(field_value(h, "Cache-Control"), std::optional<std::string>("no-transform"));
    EXPECT_EQ(sink.body, body);
    EXPECT_EQ(stats.decision, GzipResponseDecision::Bypassed);
}

TEST(GzipResponseWriterTest, BypassesBelowMinLengthBody) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;
    const std::string body = "tiny body"; // below the default min_length of 20

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        headers.add("Content-Length", std::to_string(body.size()));
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::ContentLength(body.size())));
        EXPECT_TRUE(head.has_value());
        const auto written = co_await w.write_all(bytes_of(body), body.size(), true);
        EXPECT_TRUE(written.has_value());
        stats = gzip.stats();
    });

    ASSERT_EQ(sink.headers.size(), 1u);
    const CapturedHeader &h = sink.headers[0];
    EXPECT_EQ(field_value(h, "Vary"), std::nullopt);
    EXPECT_EQ(field_value(h, "Content-Length"), std::optional<std::string>("9"));
    EXPECT_EQ(sink.body, body);
    EXPECT_EQ(stats.decision, GzipResponseDecision::Bypassed);
}

TEST(GzipResponseWriterTest, BypassesUnconfiguredContentType) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;
    const std::string body = repeat_pattern(2000, R"({"ok":true,"n":42})");

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "application/json");
        headers.add("Content-Length", std::to_string(body.size()));
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::ContentLength(body.size())));
        EXPECT_TRUE(head.has_value());
        const auto written = co_await w.write_all(bytes_of(body), body.size(), true);
        EXPECT_TRUE(written.has_value());
        stats = gzip.stats();
    });

    ASSERT_EQ(sink.headers.size(), 1u);
    EXPECT_EQ(field_value(sink.headers[0], "Vary"), std::nullopt) << "type mismatch is not an intrinsic candidate";
    EXPECT_EQ(sink.body, body);
    EXPECT_EQ(stats.decision, GzipResponseDecision::Bypassed);
}

// ---- failure paths ----

TEST(GzipResponseWriterTest, KnownLengthOverflowFailsImmediately) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;
    const std::string body = html_payload(500);

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        headers.add("Content-Length", "100"); // >= min_length, so compression activates
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::ContentLength(100)));
        EXPECT_TRUE(head.has_value());
        const auto written = co_await w.write_all(bytes_of(body), body.size(), true);
        EXPECT_FALSE(written.has_value());
        EXPECT_EQ(written.error(), IoErr::Invalid);
        stats = gzip.stats();
    });

    EXPECT_TRUE(sink.body.empty());
    EXPECT_EQ(sink.abort_count, 1);
    EXPECT_EQ(stats.decision, GzipResponseDecision::Failed);
}

TEST(GzipResponseWriterTest, KnownLengthUnderflowFailsAtFinish) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;
    const std::string body = html_payload(50);

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        headers.add("Content-Length", "100");
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::ContentLength(100)));
        EXPECT_TRUE(head.has_value());
        const auto written = co_await w.write_all(bytes_of(body), body.size(), true);
        EXPECT_FALSE(written.has_value());
        EXPECT_EQ(written.error(), IoErr::Invalid);
        stats = gzip.stats();
    });

    EXPECT_TRUE(sink.body.empty()) << "the short body must not be emitted as a finished member";
    EXPECT_EQ(sink.abort_count, 1);
    EXPECT_EQ(stats.decision, GzipResponseDecision::Failed);
}

TEST(GzipResponseWriterTest, DownstreamWriteFailureFailsTheStream) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    sink.fail_writes_with = IoErr::BrokenPipe;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;
    const std::string body = random_bytes(100000, 9); // fills the 16 KiB output buffer mid-write

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::Chunked()));
        EXPECT_TRUE(head.has_value());
        const auto written = co_await w.write_all(bytes_of(body), body.size(), true);
        EXPECT_FALSE(written.has_value());
        EXPECT_EQ(written.error(), IoErr::BrokenPipe);
        stats = gzip.stats();
    });

    EXPECT_EQ(sink.abort_count, 1);
    EXPECT_EQ(stats.decision, GzipResponseDecision::Failed);
}

TEST(GzipResponseWriterTest, AbortPropagatesAndPoisonsLaterOps) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::Chunked()));
        EXPECT_TRUE(head.has_value());
        const std::string partial = "partial";
        const auto written = co_await w.write_all(bytes_of(partial), partial.size(), false);
        EXPECT_TRUE(written.has_value());

        EXPECT_TRUE(w.abort(IoErr::Canceled).has_value());
        const auto late_write = co_await w.write_all(bytes_of(partial), 1, true);
        EXPECT_FALSE(late_write.has_value());
        EXPECT_EQ(late_write.error(), IoErr::Invalid);
        const auto late_flush = co_await w.flush();
        EXPECT_FALSE(late_flush.has_value());
        EXPECT_EQ(late_flush.error(), IoErr::Invalid);
        stats = gzip.stats();
    });

    EXPECT_EQ(sink.abort_count, 1);
    EXPECT_EQ(sink.abort_reason, IoErr::Canceled);
    EXPECT_EQ(stats.decision, GzipResponseDecision::Failed);
}

TEST(GzipResponseWriterTest, OperationsAfterFinishAreRejected) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;
    const std::string body = html_payload(300);

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        HttpHeaders headers(exchange.pool());
        headers.add("Content-Type", "text/html");
        headers.add("Content-Length", std::to_string(body.size()));
        const auto head = co_await w.send_header(final_header(&headers, 200, HttpBodySpec::ContentLength(body.size())));
        EXPECT_TRUE(head.has_value());
        const auto written = co_await w.write_all(bytes_of(body), body.size(), true);
        EXPECT_TRUE(written.has_value());

        const auto again = co_await w.write_all(bytes_of(body), 1, true);
        EXPECT_FALSE(again.has_value());
        EXPECT_EQ(again.error(), IoErr::Already);
        HttpHeaders repeat(exchange.pool());
        repeat.add("Content-Type", "text/html");
        const auto second_head = co_await w.send_header(final_header(&repeat, 200, HttpBodySpec::Chunked()));
        EXPECT_FALSE(second_head.has_value());
        EXPECT_EQ(second_head.error(), IoErr::Already);
        stats = gzip.stats();
    });

    EXPECT_EQ(sink.body, reference_gzip(body, 1));
    EXPECT_EQ(sink.abort_count, 0) << "redundant ops after finish must not abort the sink";
    EXPECT_EQ(stats.decision, GzipResponseDecision::Completed);
}

TEST(GzipResponseWriterTest, NeverActivatedWriterDestroysCleanly) {
    IoBufNodePool node_pool;
    HttpExchange exchange(node_pool, SocketAddress{});
    CaptureSink sink;
    GzipResponseWriterOptions options = active_options();
    GzipResponseWriterStats stats;

    run_scenario([&]() -> Task<void> {
        GzipResponseWriter gzip(exchange, sink.writer(), options);
        HttpResponseWriter w = gzip.writer();
        EXPECT_TRUE(w.valid());
        // No header, no writes: the destructor must cope with no encoder.
        stats = gzip.stats();
        co_return;
    });

    EXPECT_TRUE(sink.headers.empty());
    EXPECT_TRUE(sink.body.empty());
    EXPECT_EQ(stats.decision, GzipResponseDecision::Undecided);
}
