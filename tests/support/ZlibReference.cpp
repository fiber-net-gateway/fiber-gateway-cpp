#include "support/ZlibReference.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace fiber::test {

namespace {

constexpr std::size_t kMaxUint = std::numeric_limits<unsigned int>::max();

void append_output(ZlibReferenceResult &result, const unsigned char *data, std::size_t len) {
    result.output.append(reinterpret_cast<const char *>(data), len);
}

} // namespace

ZlibReferenceResult zlib_reference_gunzip(std::string_view input, bool allow_trailing, std::size_t out_chunk) {
    ZlibReferenceInflate inflate(15 + 16);
    ZlibReferenceResult result;
    result.ok = true;

    std::size_t offset = 0;
    while (!inflate.stream_end()) {
        ZlibReferenceResult step = inflate.step(input.substr(offset, input.size() - offset), out_chunk);
        offset += step.consumed;
        result.output += std::move(step.output);
        if (!step.ok) {
            result.ok = false;
            result.z_status = step.z_status;
            result.stream_end = false;
            result.consumed = offset;
            return result;
        }
        if (step.consumed == 0 && step.output.empty() && !step.stream_end) {
            // No forward progress without an error: truncated or invalid input.
            result.ok = false;
            result.z_status = step.z_status;
            result.stream_end = false;
            result.consumed = offset;
            return result;
        }
    }
    if (!allow_trailing && offset != input.size()) {
        result.ok = false;
        result.z_status = 0;
        result.stream_end = false;
        result.consumed = offset;
        return result;
    }
    result.stream_end = true;
    result.z_status = Z_STREAM_END;
    result.consumed = offset;
    return result;
}

ZlibReferenceResult zlib_reference_gzip(std::string_view input, int level) {
    return zlib_reference_deflate(input, level, 15 + 16);
}

ZlibReferenceResult zlib_reference_deflate(std::string_view input, int level, int window_bits) {
    ZlibReferenceResult result;
    z_stream stream{};
    int status = deflateInit2(&stream, level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY);
    if (status != Z_OK) {
        result.z_status = status;
        return result;
    }
    result.output.resize(deflateBound(&stream, static_cast<uLong>(input.size())));
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
    stream.next_out = reinterpret_cast<Bytef *>(result.output.data());
    if (!input.empty()) {
        stream.avail_in = static_cast<uInt>(std::min(input.size(), kMaxUint));
    }
    stream.avail_out = static_cast<uInt>(std::min(result.output.size(), kMaxUint));
    status = deflate(&stream, Z_FINISH);
    const bool ok = status == Z_STREAM_END;
    result.ok = ok;
    result.z_status = status;
    result.consumed = ok ? input.size() : 0;
    result.output.resize(ok ? result.output.size() - stream.avail_out : 0);
    result.stream_end = ok;
    deflateEnd(&stream);
    return result;
}

std::uint32_t zlib_reference_crc32(std::uint32_t crc, std::string_view data) {
    return static_cast<std::uint32_t>(
            crc32(crc, reinterpret_cast<const unsigned char *>(data.data()), static_cast<uInt>(data.size())));
}

struct ZlibReferenceInflate::Stream {
    z_stream stream{};
};

ZlibReferenceInflate::ZlibReferenceInflate(int window_bits) : stream_(new Stream{}) {
    const int status = inflateInit2(&stream_->stream, window_bits);
    last_status_ = status;
    failed_ = status != Z_OK;
}

ZlibReferenceInflate::~ZlibReferenceInflate() {
    if (stream_ != nullptr) {
        inflateEnd(&stream_->stream);
        delete stream_;
    }
}

ZlibReferenceResult ZlibReferenceInflate::step(std::string_view input, std::size_t out_capacity) {
    ZlibReferenceResult result;
    if (failed_ || stream_end_) {
        result.z_status = last_status_;
        return result;
    }
    result.output.resize(out_capacity);
    stream_->stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
    stream_->stream.avail_in = input.empty() ? 0 : static_cast<uInt>(std::min(input.size(), kMaxUint));
    stream_->stream.next_out = reinterpret_cast<Bytef *>(result.output.data());
    stream_->stream.avail_out = static_cast<uInt>(std::min(out_capacity, kMaxUint));
    const int status = inflate(&stream_->stream, Z_NO_FLUSH);
    const std::size_t consumed = input.size() - stream_->stream.avail_in;
    const std::size_t written = out_capacity - stream_->stream.avail_out;
    result.consumed = consumed;
    result.output.resize(written);
    result.z_status = status;
    last_status_ = status;
    if (status == Z_STREAM_END) {
        stream_end_ = true;
        result.stream_end = true;
        result.ok = true;
        return result;
    }
    if (status != Z_OK && status != Z_BUF_ERROR) {
        failed_ = true;
        result.ok = false;
        return result;
    }
    result.ok = true;
    return result;
}

std::uint64_t ZlibReferenceInflate::total_in() const noexcept { return stream_->stream.total_in; }

std::uint64_t ZlibReferenceInflate::total_out() const noexcept { return stream_->stream.total_out; }

bool ZlibReferenceInflate::stream_end() const noexcept { return stream_end_; }

bool ZlibReferenceInflate::failed() const noexcept { return failed_; }

int ZlibReferenceInflate::last_status() const noexcept { return last_status_; }

struct ZlibReferenceDeflate::Stream {
    z_stream stream{};
};

ZlibReferenceDeflate::ZlibReferenceDeflate(int level, int window_bits) : stream_(new Stream{}) {
    const int status = deflateInit2(&stream_->stream, level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY);
    last_status_ = status;
    failed_ = status != Z_OK;
}

ZlibReferenceDeflate::~ZlibReferenceDeflate() {
    if (stream_ != nullptr) {
        deflateEnd(&stream_->stream);
        delete stream_;
    }
}

ZlibReferenceResult ZlibReferenceDeflate::step(std::string_view input, std::size_t out_capacity,
                                               ZlibReferenceFlush flush) {
    ZlibReferenceResult result;
    if (failed_ || stream_end_) {
        result.z_status = last_status_;
        return result;
    }
    result.output.resize(out_capacity);
    stream_->stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
    stream_->stream.avail_in = input.empty() ? 0 : static_cast<uInt>(std::min(input.size(), kMaxUint));
    stream_->stream.next_out = reinterpret_cast<Bytef *>(result.output.data());
    stream_->stream.avail_out = static_cast<uInt>(std::min(out_capacity, kMaxUint));
    int flush_flag = Z_NO_FLUSH;
    if (flush == ZlibReferenceFlush::Sync) {
        flush_flag = Z_SYNC_FLUSH;
    } else if (flush == ZlibReferenceFlush::Finish) {
        flush_flag = Z_FINISH;
    }
    const int status = deflate(&stream_->stream, flush_flag);
    const std::size_t consumed = input.size() - stream_->stream.avail_in;
    const std::size_t written = out_capacity - stream_->stream.avail_out;
    result.consumed = consumed;
    result.output.resize(written);
    result.z_status = status;
    last_status_ = status;
    if (status == Z_STREAM_END) {
        stream_end_ = true;
        result.stream_end = true;
        result.ok = true;
        return result;
    }
    if (status != Z_OK && status != Z_BUF_ERROR) {
        failed_ = true;
        result.ok = false;
        return result;
    }
    result.ok = true;
    return result;
}

std::uint64_t ZlibReferenceDeflate::total_in() const noexcept { return stream_->stream.total_in; }

std::uint64_t ZlibReferenceDeflate::total_out() const noexcept { return stream_->stream.total_out; }

bool ZlibReferenceDeflate::stream_end() const noexcept { return stream_end_; }

bool ZlibReferenceDeflate::failed() const noexcept { return failed_; }

int ZlibReferenceDeflate::last_status() const noexcept { return last_status_; }

} // namespace fiber::test
