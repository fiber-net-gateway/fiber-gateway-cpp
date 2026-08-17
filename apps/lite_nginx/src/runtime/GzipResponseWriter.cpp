#include "GzipResponseWriter.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

#include <zlib.h>

#include <fiber/async/Yield.h>
#include <fiber/common/Assert.h>
#include <fiber/http/HttpExchange.h>

namespace fiber::lite_nginx::runtime {

using namespace fiber::http;

namespace {

constexpr std::size_t kOutputBufferSize = 16 * 1024;
constexpr std::size_t kCompressionInputBudget = 256 * 1024;
constexpr int kWindowBits = 15;
constexpr int kMemoryLevel = 8;
constexpr std::size_t kZlibWorkspaceSize =
        8192 + 16 + (std::size_t{1} << (kWindowBits + 2)) + (std::size_t{1} << (kMemoryLevel + 9));

std::string_view trim_ows(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

bool ascii_equal_ci(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        unsigned char left = static_cast<unsigned char>(lhs[i]);
        unsigned char right = static_cast<unsigned char>(rhs[i]);
        if (left >= 'A' && left <= 'Z') {
            left = static_cast<unsigned char>(left - 'A' + 'a');
        }
        if (right >= 'A' && right <= 'Z') {
            right = static_cast<unsigned char>(right - 'A' + 'a');
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

template<typename Callback>
void for_each_comma_token(std::string_view value, Callback &&callback) noexcept {
    while (!value.empty()) {
        const std::size_t comma = value.find(',');
        callback(trim_ows(value.substr(0, comma)));
        if (comma == std::string_view::npos) {
            return;
        }
        value.remove_prefix(comma + 1);
    }
}

int parse_quality(std::string_view value) noexcept {
    value = trim_ows(value);
    if (value.empty() || (value.front() != '0' && value.front() != '1')) {
        return -1;
    }
    const int whole = value.front() - '0';
    value.remove_prefix(1);
    if (value.empty()) {
        return whole * 1000;
    }
    if (value.front() != '.') {
        return -1;
    }
    value.remove_prefix(1);
    if (value.size() > 3) {
        return -1;
    }
    int fraction = 0;
    int scale = 100;
    for (char ch: value) {
        if (ch < '0' || ch > '9') {
            return -1;
        }
        if (whole == 1 && ch != '0') {
            return -1;
        }
        fraction += (ch - '0') * scale;
        scale /= 10;
    }
    return whole * 1000 + fraction;
}

int coding_quality(std::string_view item) noexcept {
    const std::size_t semicolon = item.find(';');
    if (semicolon == std::string_view::npos) {
        return 1000;
    }
    item.remove_prefix(semicolon + 1);
    while (!item.empty()) {
        const std::size_t next = item.find(';');
        std::string_view parameter = trim_ows(item.substr(0, next));
        const std::size_t equal = parameter.find('=');
        if (equal != std::string_view::npos && ascii_equal_ci(trim_ows(parameter.substr(0, equal)), "q")) {
            const int quality = parse_quality(parameter.substr(equal + 1));
            return quality < 0 ? 0 : quality;
        }
        if (next == std::string_view::npos) {
            break;
        }
        item.remove_prefix(next + 1);
    }
    return 1000;
}

bool accepts_gzip(const HttpHeaders &headers) noexcept {
    int gzip_quality = -1;
    int wildcard_quality = -1;
    for (const auto &field: headers.get_all("accept-encoding")) {
        for_each_comma_token(field.value_view(), [&](std::string_view item) noexcept {
            const std::size_t semicolon = item.find(';');
            const std::string_view coding = trim_ows(item.substr(0, semicolon));
            const int quality = coding_quality(item);
            if (ascii_equal_ci(coding, "gzip")) {
                gzip_quality = std::max(gzip_quality, quality);
            } else if (coding == "*") {
                wildcard_quality = std::max(wildcard_quality, quality);
            }
        });
    }
    return gzip_quality >= 0 ? gzip_quality > 0 : wildcard_quality > 0;
}

bool contains_directive(const HttpHeaders &headers, std::string_view name, std::string_view directive) noexcept {
    for (const auto &field: headers.get_all(name)) {
        bool found = false;
        for_each_comma_token(field.value_view(), [&](std::string_view item) noexcept {
            const std::size_t equal = item.find('=');
            if (ascii_equal_ci(trim_ows(item.substr(0, equal)), directive)) {
                found = true;
            }
        });
        if (found) {
            return true;
        }
    }
    return false;
}

bool has_nonempty_header(const HttpHeaders &headers, std::string_view name) noexcept {
    for (const auto &field: headers.get_all(name)) {
        if (!trim_ows(field.value_view()).empty()) {
            return true;
        }
    }
    return false;
}

bool vary_contains_accept_encoding(const HttpHeaders &headers) noexcept {
    for (const auto &field: headers.get_all("vary")) {
        bool found = false;
        for_each_comma_token(field.value_view(), [&](std::string_view item) noexcept {
            if (item == "*" || ascii_equal_ci(item, "accept-encoding")) {
                found = true;
            }
        });
        if (found) {
            return true;
        }
    }
    return false;
}

bool matches_content_type(const HttpHeaders &headers, const GzipResponseWriterOptions &options) noexcept {
    if (options.any_type) {
        return true;
    }
    std::string_view content_type = headers.get("content-type");
    const std::size_t semicolon = content_type.find(';');
    content_type = trim_ows(content_type.substr(0, semicolon));
    if (content_type.empty()) {
        return false;
    }
    if (options.types.empty()) {
        return ascii_equal_ci(content_type, "text/html");
    }
    return std::ranges::any_of(options.types,
                               [&](const std::string &type) noexcept { return ascii_equal_ci(content_type, type); });
}

bool response_status_is_compressible(int status_code) noexcept {
    return status_code == 200 || status_code == 403 || status_code == 404;
}

} // namespace

struct GzipResponseWriter::CompressionState {
    z_stream stream{};
    std::uint8_t *workspace = nullptr;
    std::size_t workspace_offset = 0;
    bool zlib_state_allocated = false;
    bool initialized = false;
};

void *GzipResponseWriter::workspace_alloc(void *opaque, unsigned int items, unsigned int size) noexcept {
    auto *compression = static_cast<GzipResponseWriter::CompressionState *>(opaque);
    if (compression == nullptr || (size != 0 && items > std::numeric_limits<std::size_t>::max() / size)) {
        return nullptr;
    }
    std::size_t bytes = static_cast<std::size_t>(items) * size;
    if (items == 1 && bytes < 8192 && bytes % 512 != 0 && !compression->zlib_state_allocated) {
        bytes = 8192;
        compression->zlib_state_allocated = true;
    }
    constexpr std::size_t kAlignment = alignof(std::max_align_t);
    constexpr std::size_t kAlignmentMask = kAlignment - 1;
    const std::size_t offset = (compression->workspace_offset + kAlignmentMask) & ~kAlignmentMask;
    if (offset > kZlibWorkspaceSize || bytes > kZlibWorkspaceSize - offset) {
        return nullptr;
    }
    compression->workspace_offset = offset + bytes;
    return compression->workspace + offset;
}

void GzipResponseWriter::workspace_free(void *, void *) noexcept {}

GzipResponseWriter::GzipResponseWriter(HttpExchange &exchange, HttpResponseWriter next,
                                       const GzipResponseWriterOptions &options) noexcept :
    exchange_(&exchange), options_(options), next_(next), writer_(this, writer_ops()),
    filtered_headers_(exchange.pool()) {
    FIBER_ASSERT(next_.valid());
}

GzipResponseWriter::~GzipResponseWriter() noexcept {
    if (compression_ != nullptr && compression_->initialized) {
        const int result = deflateEnd(&compression_->stream);
        FIBER_ASSERT(result == Z_OK);
    }
}

const HttpResponseWriter::Ops &GzipResponseWriter::writer_ops() noexcept {
    static const HttpResponseWriter::Ops kOps{
            &GzipResponseWriter::on_send_header,     &GzipResponseWriter::on_write_all_chain,
            &GzipResponseWriter::on_write_all_bytes, &GzipResponseWriter::on_write_chain,
            &GzipResponseWriter::on_write_bytes,     &GzipResponseWriter::on_flush,
            &GzipResponseWriter::on_abort,
    };
    return kOps;
}

async::Task<common::IoResult<void>> GzipResponseWriter::on_send_header(void *ctx, const OutgoingHeaderBlockView &header,
                                                                       std::chrono::milliseconds timeout) {
    co_return co_await static_cast<GzipResponseWriter *>(ctx)->send_header(header, timeout);
}

async::Task<common::IoResult<std::size_t>>
GzipResponseWriter::on_write_all_chain(void *ctx, mem::IoBufChain chunk, std::chrono::milliseconds timeout) noexcept {
    co_return co_await static_cast<GzipResponseWriter *>(ctx)->write_all_chain(std::move(chunk), timeout);
}

async::Task<common::IoResult<std::size_t>>
GzipResponseWriter::on_write_all_bytes(void *ctx, const std::uint8_t *buf, std::size_t len, bool end,
                                       std::chrono::milliseconds timeout) noexcept {
    co_return co_await static_cast<GzipResponseWriter *>(ctx)->write_all_bytes(buf, len, end, timeout);
}

async::Task<common::IoResult<std::size_t>>
GzipResponseWriter::on_write_chain(void *ctx, mem::IoBufChain &chunk, std::chrono::milliseconds timeout) noexcept {
    co_return co_await static_cast<GzipResponseWriter *>(ctx)->write_chain(chunk, timeout);
}

async::Task<common::IoResult<std::size_t>>
GzipResponseWriter::on_write_bytes(void *ctx, const std::uint8_t *buf, std::size_t len, bool end,
                                   std::chrono::milliseconds timeout) noexcept {
    co_return co_await static_cast<GzipResponseWriter *>(ctx)->write_bytes(buf, len, end, timeout);
}

async::Task<common::IoResult<void>> GzipResponseWriter::on_flush(void *ctx,
                                                                 std::chrono::milliseconds timeout) noexcept {
    co_return co_await static_cast<GzipResponseWriter *>(ctx)->flush(timeout);
}

common::IoResult<void> GzipResponseWriter::on_abort(void *ctx, common::IoErr reason) noexcept {
    return static_cast<GzipResponseWriter *>(ctx)->abort(reason);
}

async::Task<common::IoResult<void>> GzipResponseWriter::send_header(const OutgoingHeaderBlockView &header,
                                                                    std::chrono::milliseconds timeout) {
    if (header.kind == OutgoingHeaderKind::Informational) {
        co_return co_await next_.send_header(header, timeout);
    }
    if (header.kind == OutgoingHeaderKind::Trailer) {
        if (state_ == State::Active) {
            auto result = co_await finish(false, timeout);
            if (!result) {
                co_return result;
            }
        }
        co_return co_await next_.send_header(header, timeout);
    }
    if (state_ != State::AwaitingFinalHeader) {
        co_return std::unexpected(common::IoErr::Already);
    }

    const HttpHeaders *headers = header.headers;
    const bool intrinsic_candidate =
            options_.enabled && response_status_is_compressible(header.status_code) &&
            exchange_->method() != HttpMethod::Head && !header.end_stream && !header.body.is_none() &&
            !header.body.is_stream() &&
            (!header.body.is_content_length() || header.body.content_length() >= options_.min_length) &&
            headers != nullptr && !has_nonempty_header(*headers, "content-encoding") &&
            !contains_directive(*headers, "cache-control", "no-transform") && matches_content_type(*headers, options_);
    const bool add_vary = intrinsic_candidate && !vary_contains_accept_encoding(*headers);
    const bool active = intrinsic_candidate &&
                        !contains_directive(exchange_->request_headers(), "cache-control", "no-transform") &&
                        accepts_gzip(exchange_->request_headers());

    if (!active && !add_vary) {
        state_ = State::Bypass;
        stats_.decision = GzipResponseDecision::Bypassed;
        co_return co_await next_.send_header(header, timeout);
    }

    auto prepare_result = prepare_filtered_headers(header, active, add_vary);
    if (!prepare_result) {
        fail(prepare_result.error());
        co_return std::unexpected(prepare_result.error());
    }
    OutgoingHeaderBlockView filtered = header;
    filtered.headers = &filtered_headers_;
    if (active) {
        filtered.body = HttpBodySpec::Auto();
        filtered.end_stream = false;
        state_ = State::Active;
        stats_.decision = GzipResponseDecision::Active;
        if (header.body.is_content_length()) {
            expected_input_bytes_ = header.body.content_length();
            has_expected_input_bytes_ = true;
        }
    } else {
        state_ = State::Bypass;
        stats_.decision = GzipResponseDecision::Bypassed;
    }
    auto result = co_await next_.send_header(filtered, timeout);
    if (!result) {
        state_ = State::Failed;
        stats_.decision = GzipResponseDecision::Failed;
    }
    co_return result;
}

common::IoResult<void> GzipResponseWriter::prepare_filtered_headers(const OutgoingHeaderBlockView &header,
                                                                    bool transform, bool add_vary) noexcept {
    filtered_headers_.clear();
    if (header.headers != nullptr) {
        for (const auto &field: *header.headers) {
            const std::string_view name = field.lowcase_view();
            if (transform && (name == "content-length" || name == "transfer-encoding" || name == "accept-ranges" ||
                              name == "content-encoding")) {
                continue;
            }
            if (transform && name == "etag") {
                const std::string_view value = trim_ows(field.value_view());
                if (value.size() >= 4 && value.starts_with("W/\"") && value.ends_with('"')) {
                    if (filtered_headers_.add_view(field.name_view(), field.value_view(), field.lowcase_name,
                                                   field.name_hash) == nullptr) {
                        return std::unexpected(common::IoErr::NoMem);
                    }
                } else if (value.size() >= 2 && value.starts_with('"') && value.ends_with('"')) {
                    auto *weak = static_cast<char *>(filtered_headers_.pool().alloc(value.size() + 2, alignof(char)));
                    if (weak == nullptr) {
                        return std::unexpected(common::IoErr::NoMem);
                    }
                    weak[0] = 'W';
                    weak[1] = '/';
                    std::memcpy(weak + 2, value.data(), value.size());
                    if (filtered_headers_.add_view(field.name_view(), {weak, value.size() + 2}, field.lowcase_name,
                                                   field.name_hash) == nullptr) {
                        return std::unexpected(common::IoErr::NoMem);
                    }
                }
                continue;
            }
            if (filtered_headers_.add_view(field.name_view(), field.value_view(), field.lowcase_name,
                                           field.name_hash) == nullptr) {
                return std::unexpected(common::IoErr::NoMem);
            }
        }
    }
    if (transform && filtered_headers_.add("Content-Encoding", "gzip") == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (add_vary && filtered_headers_.add("Vary", "Accept-Encoding") == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return {};
}

common::IoResult<void> GzipResponseWriter::initialize_compressor() noexcept {
    if (compression_ != nullptr && compression_->initialized) {
        return {};
    }
    if (options_.compression_level < 1 || options_.compression_level > 9) {
        return std::unexpected(common::IoErr::Invalid);
    }
    void *state_mem = exchange_->pool().alloc(sizeof(CompressionState), alignof(CompressionState));
    if (state_mem == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    compression_ = new (state_mem) CompressionState{};
    compression_->workspace =
            static_cast<std::uint8_t *>(exchange_->pool().alloc(kZlibWorkspaceSize, alignof(std::max_align_t)));
    if (compression_->workspace == nullptr) {
        return std::unexpected(common::IoErr::NoMem);
    }
    output_ = mem::IoBuf::allocate(kOutputBufferSize);
    if (!output_) {
        return std::unexpected(common::IoErr::NoMem);
    }
    compression_->stream.zalloc = &GzipResponseWriter::workspace_alloc;
    compression_->stream.zfree = &GzipResponseWriter::workspace_free;
    compression_->stream.opaque = compression_;
    const int result = deflateInit2(&compression_->stream, options_.compression_level, Z_DEFLATED, kWindowBits + 16,
                                    kMemoryLevel, Z_DEFAULT_STRATEGY);
    if (result != Z_OK) {
        return std::unexpected(result == Z_MEM_ERROR ? common::IoErr::NoMem : common::IoErr::Invalid);
    }
    compression_->initialized = true;
    return {};
}

async::Task<common::IoResult<void>> GzipResponseWriter::drain_output(bool end_stream,
                                                                     std::chrono::milliseconds timeout) noexcept {
    const std::size_t readable = output_.readable();
    auto result = co_await next_.write_all(output_.readable_data(), readable, end_stream, timeout);
    if (!result) {
        fail(result.error());
        co_return std::unexpected(result.error());
    }
    if (*result != readable) {
        fail(common::IoErr::Invalid);
        co_return std::unexpected(common::IoErr::Invalid);
    }
    stats_.output_bytes = readable > std::numeric_limits<std::size_t>::max() - stats_.output_bytes
                                  ? std::numeric_limits<std::size_t>::max()
                                  : stats_.output_bytes + readable;
    output_.clear();
    co_return common::IoResult<void>{};
}

async::Task<common::IoResult<void>> GzipResponseWriter::compress_input(const std::uint8_t *buf, std::size_t len,
                                                                       std::chrono::milliseconds timeout) noexcept {
    auto init_result = initialize_compressor();
    if (!init_result) {
        fail(init_result.error());
        co_return std::unexpected(init_result.error());
    }
    if (has_expected_input_bytes_ &&
        (stats_.input_bytes > expected_input_bytes_ || len > expected_input_bytes_ - stats_.input_bytes)) {
        fail(common::IoErr::Invalid);
        co_return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t offset = 0;
    while (offset < len) {
        if (input_since_yield_ == kCompressionInputBudget) {
            co_await async::yield();
            input_since_yield_ = 0;
        }
        const std::size_t budget = kCompressionInputBudget - input_since_yield_;
        const std::size_t slice =
                std::min({len - offset, budget, static_cast<std::size_t>(std::numeric_limits<uInt>::max())});
        compression_->stream.next_in = const_cast<Bytef *>(buf + offset);
        compression_->stream.avail_in = static_cast<uInt>(slice);
        while (compression_->stream.avail_in > 0) {
            if (output_.writable() == 0) {
                auto drain_result = co_await drain_output(false, timeout);
                if (!drain_result) {
                    co_return drain_result;
                }
            }
            compression_->stream.next_out = output_.writable_data();
            compression_->stream.avail_out = static_cast<uInt>(output_.writable());
            const uInt before = compression_->stream.avail_out;
            const int result = deflate(&compression_->stream, Z_NO_FLUSH);
            output_.commit(before - compression_->stream.avail_out);
            if (result != Z_OK) {
                fail(common::IoErr::Invalid);
                co_return std::unexpected(common::IoErr::Invalid);
            }
        }
        offset += slice;
        stats_.input_bytes = slice > std::numeric_limits<std::size_t>::max() - stats_.input_bytes
                                     ? std::numeric_limits<std::size_t>::max()
                                     : stats_.input_bytes + slice;
        input_since_yield_ += slice;
    }
    flush_pending_ = flush_pending_ || len != 0;
    co_return common::IoResult<void>{};
}

async::Task<common::IoResult<void>> GzipResponseWriter::sync_flush(std::chrono::milliseconds timeout) noexcept {
    if (!flush_pending_) {
        co_return common::IoResult<void>{};
    }
    auto init_result = initialize_compressor();
    if (!init_result) {
        fail(init_result.error());
        co_return std::unexpected(init_result.error());
    }
    for (;;) {
        if (output_.writable() == 0) {
            auto drain_result = co_await drain_output(false, timeout);
            if (!drain_result) {
                co_return drain_result;
            }
        }
        compression_->stream.next_in = nullptr;
        compression_->stream.avail_in = 0;
        compression_->stream.next_out = output_.writable_data();
        compression_->stream.avail_out = static_cast<uInt>(output_.writable());
        const uInt before = compression_->stream.avail_out;
        const int result = deflate(&compression_->stream, Z_SYNC_FLUSH);
        output_.commit(before - compression_->stream.avail_out);
        if (result != Z_OK) {
            fail(common::IoErr::Invalid);
            co_return std::unexpected(common::IoErr::Invalid);
        }
        if (compression_->stream.avail_out != 0) {
            break;
        }
    }
    if (output_.readable() != 0) {
        auto drain_result = co_await drain_output(false, timeout);
        if (!drain_result) {
            co_return drain_result;
        }
    }
    flush_pending_ = false;
    co_return common::IoResult<void>{};
}

async::Task<common::IoResult<void>> GzipResponseWriter::finish(bool end_stream,
                                                               std::chrono::milliseconds timeout) noexcept {
    if (has_expected_input_bytes_ && stats_.input_bytes != expected_input_bytes_) {
        fail(common::IoErr::Invalid);
        co_return std::unexpected(common::IoErr::Invalid);
    }
    auto init_result = initialize_compressor();
    if (!init_result) {
        fail(init_result.error());
        co_return std::unexpected(init_result.error());
    }
    for (;;) {
        if (output_.writable() == 0) {
            auto drain_result = co_await drain_output(false, timeout);
            if (!drain_result) {
                co_return drain_result;
            }
        }
        compression_->stream.next_in = nullptr;
        compression_->stream.avail_in = 0;
        compression_->stream.next_out = output_.writable_data();
        compression_->stream.avail_out = static_cast<uInt>(output_.writable());
        const uInt before = compression_->stream.avail_out;
        const int result = deflate(&compression_->stream, Z_FINISH);
        output_.commit(before - compression_->stream.avail_out);
        if (result == Z_STREAM_END) {
            break;
        }
        if (result != Z_OK || compression_->stream.avail_out != 0) {
            fail(common::IoErr::Invalid);
            co_return std::unexpected(common::IoErr::Invalid);
        }
    }
    auto drain_result = co_await drain_output(end_stream, timeout);
    if (!drain_result) {
        co_return drain_result;
    }
    state_ = State::Finished;
    flush_pending_ = false;
    stats_.decision = GzipResponseDecision::Completed;
    co_return common::IoResult<void>{};
}

async::Task<common::IoResult<std::size_t>>
GzipResponseWriter::write_all_chain(mem::IoBufChain chunk, std::chrono::milliseconds timeout) noexcept {
    if (state_ == State::Bypass) {
        co_return co_await next_.write_all(std::move(chunk), timeout);
    }
    co_return co_await write_chain(chunk, timeout);
}

async::Task<common::IoResult<std::size_t>>
GzipResponseWriter::write_all_bytes(const std::uint8_t *buf, std::size_t len, bool end,
                                    std::chrono::milliseconds timeout) noexcept {
    if (state_ == State::Bypass) {
        co_return co_await next_.write_all(buf, len, end, timeout);
    }
    co_return co_await write_bytes(buf, len, end, timeout);
}

async::Task<common::IoResult<std::size_t>> GzipResponseWriter::write_chain(mem::IoBufChain &chunk,
                                                                           std::chrono::milliseconds timeout) noexcept {
    if (state_ == State::Bypass) {
        co_return co_await next_.write(chunk, timeout);
    }
    if (state_ != State::Active) {
        co_return std::unexpected(state_ == State::Finished ? common::IoErr::Already : common::IoErr::Invalid);
    }
    const std::size_t intended = chunk.readable_bytes();
    const bool end = chunk.complete();
    while (const mem::IoBuf *buf = chunk.first_readable()) {
        const std::size_t readable = buf->readable();
        auto result = co_await compress_input(buf->readable_data(), readable, timeout);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        chunk.consume(readable);
    }
    if (end) {
        auto result = co_await finish(true, timeout);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        chunk.clear_complete();
    }
    co_return intended;
}

async::Task<common::IoResult<std::size_t>> GzipResponseWriter::write_bytes(const std::uint8_t *buf, std::size_t len,
                                                                           bool end,
                                                                           std::chrono::milliseconds timeout) noexcept {
    if (state_ == State::Bypass) {
        co_return co_await next_.write(buf, len, end, timeout);
    }
    if (state_ != State::Active) {
        co_return std::unexpected(state_ == State::Finished ? common::IoErr::Already : common::IoErr::Invalid);
    }
    auto result = co_await compress_input(buf, len, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    if (end) {
        result = co_await finish(true, timeout);
        if (!result) {
            co_return std::unexpected(result.error());
        }
    }
    co_return len;
}

async::Task<common::IoResult<void>> GzipResponseWriter::flush(std::chrono::milliseconds timeout) noexcept {
    if (state_ == State::Bypass || state_ == State::AwaitingFinalHeader) {
        co_return co_await next_.flush(timeout);
    }
    if (state_ != State::Active) {
        co_return std::unexpected(state_ == State::Finished ? common::IoErr::Already : common::IoErr::Invalid);
    }
    auto result = co_await sync_flush(timeout);
    if (!result) {
        co_return result;
    }
    result = co_await next_.flush(timeout);
    if (!result) {
        fail(result.error());
    }
    co_return result;
}

common::IoResult<void> GzipResponseWriter::abort(common::IoErr reason) noexcept {
    if (state_ != State::Finished) {
        state_ = State::Failed;
        stats_.decision = GzipResponseDecision::Failed;
    }
    return next_.abort(reason);
}

void GzipResponseWriter::fail(common::IoErr error) noexcept {
    if (state_ == State::Failed) {
        return;
    }
    state_ = State::Failed;
    stats_.decision = GzipResponseDecision::Failed;
    (void) next_.abort(error);
}

} // namespace fiber::lite_nginx::runtime
