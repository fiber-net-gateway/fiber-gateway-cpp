#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <limits>
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/HttpBodyPipe.h>

namespace {

using fiber::async::DetachedTask;
using fiber::common::IoErr;
using fiber::common::IoResult;
using fiber::http::HttpBodyPipeOptions;
using fiber::http::HttpBodyPipePhase;
using fiber::http::HttpBodyPipeResult;
using fiber::http::kUnbufferedBodyPipeLowWater;
using fiber::mem::IoBuf;
using fiber::mem::IoBufChain;

struct ReadAction {
    std::size_t bytes = 0;
    bool complete = false;
    IoErr error = IoErr::None;
};

class FakeBodyReader {
public:
    explicit FakeBodyReader(std::vector<ReadAction> actions, std::vector<char> *operations = nullptr) :
        actions_(std::move(actions)), operations_(operations) {}

    fiber::async::Task<IoResult<IoBufChain>> read_body(std::size_t max_bytes,
                                                       std::chrono::milliseconds timeout) noexcept {
        if (operations_) {
            operations_->push_back('R');
        }
        max_bytes_.push_back(max_bytes);
        timeouts_.push_back(timeout);
        if (next_action_ >= actions_.size()) {
            co_return std::unexpected(IoErr::Invalid);
        }

        const ReadAction action = actions_[next_action_++];
        if (action.error != IoErr::None) {
            co_return std::unexpected(action.error);
        }

        IoBufChain chunk(fiber::event::EventLoop::current().io_buf_node_pool());
        if (action.bytes > 0) {
            IoBuf data = IoBuf::allocate(action.bytes);
            if (!data) {
                co_return std::unexpected(IoErr::NoMem);
            }
            std::memset(data.writable_data(), 'x', action.bytes);
            data.commit(action.bytes);
            if (!chunk.append(std::move(data))) {
                co_return std::unexpected(IoErr::NoMem);
            }
        }
        if (action.complete) {
            chunk.mark_complete();
        }
        co_return chunk;
    }

    IoResult<void> abort(IoErr reason) noexcept {
        ++abort_calls_;
        abort_reason_ = reason;
        return {};
    }

    [[nodiscard]] const std::vector<std::size_t> &max_bytes() const noexcept { return max_bytes_; }
    [[nodiscard]] std::size_t abort_calls() const noexcept { return abort_calls_; }
    [[nodiscard]] IoErr abort_reason() const noexcept { return abort_reason_; }

private:
    std::vector<ReadAction> actions_;
    std::vector<char> *operations_ = nullptr;
    std::vector<std::size_t> max_bytes_;
    std::vector<std::chrono::milliseconds> timeouts_;
    std::size_t next_action_ = 0;
    std::size_t abort_calls_ = 0;
    IoErr abort_reason_ = IoErr::None;
};

struct WriteAction {
    std::size_t max_bytes = std::numeric_limits<std::size_t>::max();
    IoErr error = IoErr::None;
    bool stall = false;
};

class FakeBodyWriter {
public:
    explicit FakeBodyWriter(std::vector<WriteAction> actions = {}, std::vector<char> *operations = nullptr,
                            IoErr flush_error = IoErr::None) :
        actions_(std::move(actions)), operations_(operations), flush_error_(flush_error) {}

    fiber::async::Task<IoResult<std::size_t>> write(IoBufChain &buffer, std::chrono::milliseconds timeout) noexcept {
        if (operations_) {
            operations_->push_back('W');
        }
        buffered_before_write_.push_back(buffer.readable_bytes());
        timeouts_.push_back(timeout);

        WriteAction action;
        if (next_action_ < actions_.size()) {
            action = actions_[next_action_++];
        }
        if (action.error != IoErr::None) {
            co_return std::unexpected(action.error);
        }
        if (action.stall) {
            co_return 0;
        }

        const std::size_t written = std::min(buffer.readable_bytes(), action.max_bytes);
        buffer.consume_and_compact(written);
        if (buffer.readable_bytes() == 0 && buffer.complete()) {
            buffer.clear_complete();
        }
        co_return written;
    }

    fiber::async::Task<IoResult<void>> flush(std::chrono::milliseconds timeout) noexcept {
        if (operations_) {
            operations_->push_back('F');
        }
        flush_timeouts_.push_back(timeout);
        if (flush_error_ != IoErr::None) {
            co_return std::unexpected(flush_error_);
        }
        co_return IoResult<void>{};
    }

    IoResult<void> abort(IoErr reason) noexcept {
        ++abort_calls_;
        abort_reason_ = reason;
        return {};
    }

    [[nodiscard]] const std::vector<std::size_t> &buffered_before_write() const noexcept {
        return buffered_before_write_;
    }
    [[nodiscard]] std::size_t abort_calls() const noexcept { return abort_calls_; }
    [[nodiscard]] IoErr abort_reason() const noexcept { return abort_reason_; }

private:
    std::vector<WriteAction> actions_;
    std::vector<char> *operations_ = nullptr;
    std::vector<std::size_t> buffered_before_write_;
    std::vector<std::chrono::milliseconds> timeouts_;
    std::vector<std::chrono::milliseconds> flush_timeouts_;
    std::size_t next_action_ = 0;
    std::size_t abort_calls_ = 0;
    IoErr abort_reason_ = IoErr::None;
    IoErr flush_error_ = IoErr::None;
};

DetachedTask run_pipe(FakeBodyReader *reader, FakeBodyWriter *writer, HttpBodyPipeOptions options,
                      std::promise<HttpBodyPipeResult> *promise) {
    auto result = co_await fiber::http::pipe_http_body(fiber::http::make_http_body_pipe_reader(*reader),
                                                       fiber::http::make_http_body_pipe_writer(*writer),
                                                       fiber::event::EventLoop::current().io_buf_node_pool(), options);
    promise->set_value(std::move(result));
    fiber::event::EventLoop::current().stop();
    co_return;
}

HttpBodyPipeResult execute_pipe(FakeBodyReader &reader, FakeBodyWriter &writer, HttpBodyPipeOptions options = {}) {
    fiber::event::EventLoopGroup group(1);
    std::promise<HttpBodyPipeResult> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_pipe(&reader, &writer, options, &promise); });
    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        return std::unexpected(fiber::http::HttpBodyPipeError{
                .code = IoErr::TimedOut,
                .phase = HttpBodyPipePhase::Validate,
        });
    }

    auto result = future.get();
    group.join();
    return result;
}

TEST(HttpBodyPipeTest, WritesDownToLowWaterThenRefillsToCapacity) {
    FakeBodyReader reader({
            {.bytes = 64 * 1024},
            {.bytes = 24 * 1024, .complete = true},
    });
    FakeBodyWriter writer({
            {.max_bytes = 8 * 1024},
            {.max_bytes = 16 * 1024},
    });

    auto result = execute_pipe(reader, writer);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(reader.max_bytes(), (std::vector<std::size_t>{64 * 1024, 24 * 1024}));
    EXPECT_EQ(writer.buffered_before_write(), (std::vector<std::size_t>{64 * 1024, 56 * 1024, 40 * 1024, 24 * 1024}));
    EXPECT_EQ(result->bytes_read, 88u * 1024u);
    EXPECT_EQ(result->bytes_written, 88u * 1024u);
    EXPECT_EQ(result->peak_buffered_bytes, 64u * 1024u);
    EXPECT_EQ(result->read_calls, 2u);
    EXPECT_EQ(result->write_calls, 4u);
}

TEST(HttpBodyPipeTest, AccumulatesUntilExactLowWaterAndForwardsDelayedCompletion) {
    FakeBodyReader reader({
            {.bytes = 16 * 1024},
            {.bytes = 16 * 1024},
            {.bytes = 16 * 1024},
            {.complete = true},
    });
    FakeBodyWriter writer;

    auto result = execute_pipe(reader, writer);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(reader.max_bytes(), (std::vector<std::size_t>{64 * 1024, 48 * 1024, 32 * 1024, 64 * 1024}));
    EXPECT_EQ(writer.buffered_before_write(), (std::vector<std::size_t>{48 * 1024, 0}));
    EXPECT_EQ(result->bytes_written, 48u * 1024u);
}

TEST(HttpBodyPipeTest, EndOfInputFlushesBelowLowWater) {
    FakeBodyReader reader({
            {.bytes = 10 * 1024, .complete = true},
    });
    FakeBodyWriter writer;

    auto result = execute_pipe(reader, writer);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(writer.buffered_before_write(), (std::vector<std::size_t>{10 * 1024}));
    EXPECT_EQ(result->bytes_written, 10u * 1024u);
}

TEST(HttpBodyPipeTest, ReadErrorAbortsBothEndpointsAndReportsPhase) {
    FakeBodyReader reader({
            {.error = IoErr::TimedOut},
    });
    FakeBodyWriter writer;

    auto result = execute_pipe(reader, writer);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IoErr::TimedOut);
    EXPECT_EQ(result.error().phase, HttpBodyPipePhase::Read);
    EXPECT_EQ(reader.abort_calls(), 1u);
    EXPECT_EQ(writer.abort_calls(), 1u);
    EXPECT_EQ(writer.abort_reason(), IoErr::TimedOut);
}

TEST(HttpBodyPipeTest, DrainsBufferedBytesBeforeReportingReadError) {
    std::vector<char> operations;
    FakeBodyReader reader(
            {
                    {.bytes = 10 * 1024},
                    {.error = IoErr::ConnReset},
            },
            &operations);
    FakeBodyWriter writer({}, &operations);

    auto result = execute_pipe(reader, writer);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IoErr::ConnReset);
    EXPECT_EQ(result.error().phase, HttpBodyPipePhase::Read);
    EXPECT_EQ(operations, (std::vector<char>{'R', 'R', 'W'}));
    EXPECT_EQ(writer.buffered_before_write(), (std::vector<std::size_t>{10 * 1024}));
    EXPECT_EQ(reader.abort_calls(), 1u);
    EXPECT_EQ(writer.abort_calls(), 1u);
    EXPECT_EQ(writer.abort_reason(), IoErr::ConnReset);
}

TEST(HttpBodyPipeTest, WriteErrorAbortsBothEndpointsAndReportsPhase) {
    FakeBodyReader reader({
            {.bytes = 48 * 1024},
    });
    FakeBodyWriter writer({
            {.error = IoErr::BrokenPipe},
    });

    auto result = execute_pipe(reader, writer);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IoErr::BrokenPipe);
    EXPECT_EQ(result.error().phase, HttpBodyPipePhase::Write);
    EXPECT_EQ(reader.abort_calls(), 1u);
    EXPECT_EQ(reader.abort_reason(), IoErr::BrokenPipe);
    EXPECT_EQ(writer.abort_calls(), 1u);
}

TEST(HttpBodyPipeTest, ZeroLowWaterDrainsEachReadBeforeReadingAgain) {
    std::vector<char> operations;
    FakeBodyReader reader(
            {
                    {.bytes = 8 * 1024},
                    {.bytes = 6 * 1024, .complete = true},
            },
            &operations);
    FakeBodyWriter writer(
            {
                    {.max_bytes = 3 * 1024},
                    {.max_bytes = 5 * 1024},
            },
            &operations);
    HttpBodyPipeOptions options;
    options.low_water = kUnbufferedBodyPipeLowWater;

    auto result = execute_pipe(reader, writer, options);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(operations, (std::vector<char>{'R', 'W', 'W', 'F', 'R', 'W'}));
    EXPECT_EQ(reader.max_bytes(), (std::vector<std::size_t>{64 * 1024, 64 * 1024}));
    EXPECT_EQ(writer.buffered_before_write(), (std::vector<std::size_t>{8 * 1024, 5 * 1024, 6 * 1024}));
    EXPECT_EQ(result->bytes_read, 14u * 1024u);
    EXPECT_EQ(result->bytes_written, 14u * 1024u);
    EXPECT_EQ(result->peak_buffered_bytes, 8u * 1024u);
    EXPECT_EQ(result->read_calls, 2u);
    EXPECT_EQ(result->write_calls, 3u);
}

TEST(HttpBodyPipeTest, ZeroLowWaterForwardsCompletionOnlyRead) {
    std::vector<char> operations;
    FakeBodyReader reader({{.complete = true}}, &operations);
    FakeBodyWriter writer({}, &operations);
    HttpBodyPipeOptions options;
    options.low_water = kUnbufferedBodyPipeLowWater;

    auto result = execute_pipe(reader, writer, options);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(operations, (std::vector<char>{'R', 'W'}));
    EXPECT_EQ(writer.buffered_before_write(), (std::vector<std::size_t>{0}));
    EXPECT_EQ(result->bytes_read, 0u);
    EXPECT_EQ(result->bytes_written, 0u);
    EXPECT_EQ(result->read_calls, 1u);
    EXPECT_EQ(result->write_calls, 1u);
}

TEST(HttpBodyPipeTest, ZeroLowWaterReportsFlushFailureAsWriteError) {
    std::vector<char> operations;
    FakeBodyReader reader({{.bytes = 1024}}, &operations);
    FakeBodyWriter writer({}, &operations, IoErr::BrokenPipe);
    HttpBodyPipeOptions options;
    options.low_water = kUnbufferedBodyPipeLowWater;

    auto result = execute_pipe(reader, writer, options);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().phase, HttpBodyPipePhase::Write);
    EXPECT_EQ(result.error().code, IoErr::BrokenPipe);
    EXPECT_EQ(operations, (std::vector<char>{'R', 'W', 'F'}));
    EXPECT_EQ(reader.abort_calls(), 1u);
    EXPECT_EQ(reader.abort_reason(), IoErr::BrokenPipe);
    EXPECT_EQ(writer.abort_calls(), 1u);
    EXPECT_EQ(writer.abort_reason(), IoErr::BrokenPipe);
}

TEST(HttpBodyPipeTest, RejectsInvalidBufferBoundsBeforeStartingIo) {
    for (const HttpBodyPipeOptions options: {
                 HttpBodyPipeOptions{.buffer_size = 0, .low_water = kUnbufferedBodyPipeLowWater},
                 HttpBodyPipeOptions{.buffer_size = 1024, .low_water = 1025},
         }) {
        FakeBodyReader reader({});
        FakeBodyWriter writer;

        auto result = execute_pipe(reader, writer, options);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, IoErr::Invalid);
        EXPECT_EQ(result.error().phase, HttpBodyPipePhase::Validate);
        EXPECT_EQ(reader.abort_calls(), 0u);
        EXPECT_EQ(writer.abort_calls(), 0u);
    }
}

TEST(HttpBodyPipeTest, RejectsEmptyIncompleteReadAndZeroProgressWrite) {
    {
        FakeBodyReader reader({
                {},
        });
        FakeBodyWriter writer;

        auto result = execute_pipe(reader, writer);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().phase, HttpBodyPipePhase::Validate);
        EXPECT_EQ(reader.abort_calls(), 1u);
        EXPECT_EQ(writer.abort_calls(), 1u);
    }

    {
        FakeBodyReader reader({
                {.bytes = 48 * 1024},
        });
        FakeBodyWriter writer({
                {.stall = true},
        });

        auto result = execute_pipe(reader, writer);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().phase, HttpBodyPipePhase::Write);
        EXPECT_EQ(result.error().code, IoErr::Invalid);
    }
}

} // namespace
