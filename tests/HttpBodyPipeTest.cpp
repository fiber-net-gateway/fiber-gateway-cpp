#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
    // Deliver the bytes in a max_bytes-sized buffer, as transports receive
    // them, instead of an exactly sized one.
    bool sparse = false;
};

// Body byte at stream offset `offset`: lets a test check order end to end.
std::uint8_t pattern_byte(std::size_t offset) noexcept { return static_cast<std::uint8_t>(offset % 251); }

std::vector<std::uint8_t> pattern_bytes(std::size_t bytes) {
    std::vector<std::uint8_t> out(bytes);
    for (std::size_t i = 0; i < bytes; ++i) {
        out[i] = pattern_byte(i);
    }
    return out;
}

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

        IoBufChain chunk;
        if (action.bytes > 0) {
            IoBuf data = IoBuf::allocate(action.sparse ? std::max(max_bytes, action.bytes) : action.bytes);
            if (!data) {
                co_return std::unexpected(IoErr::NoMem);
            }
            for (std::size_t i = 0; i < action.bytes; ++i) {
                data.writable_data()[i] = pattern_byte(stream_offset_++);
            }
            data.commit(action.bytes);
            delivered_data_.push_back(data.readable_data());
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
    [[nodiscard]] const std::vector<const std::uint8_t *> &delivered_data() const noexcept { return delivered_data_; }
    [[nodiscard]] std::size_t abort_calls() const noexcept { return abort_calls_; }
    [[nodiscard]] IoErr abort_reason() const noexcept { return abort_reason_; }

private:
    std::vector<ReadAction> actions_;
    std::vector<char> *operations_ = nullptr;
    std::vector<std::size_t> max_bytes_;
    std::vector<std::chrono::milliseconds> timeouts_;
    std::vector<const std::uint8_t *> delivered_data_;
    std::size_t stream_offset_ = 0;
    std::size_t next_action_ = 0;
    std::size_t abort_calls_ = 0;
    IoErr abort_reason_ = IoErr::None;
};

struct WriteAction {
    std::size_t max_bytes = std::numeric_limits<std::size_t>::max();
    IoErr error = IoErr::None;
    bool stall = false;
    bool defer_completion = false;
    // Keep a handle to every consumed buffer, as a zero-copy sink queueing
    // them would, and leave the drained nodes in the chain.
    bool retain = false;
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
        nodes_before_write_.push_back(buffer.size());
        timeouts_.push_back(timeout);
        std::size_t storage = 0;
        const IoBuf *prev = nullptr;
        for (const fiber::mem::IoBufNode *node = buffer.front_node(); node != nullptr; node = node->next) {
            if (prev == nullptr || !prev->same_storage(node->buf)) {
                storage += node->buf.capacity();
            }
            prev = &node->buf;
            if (node->buf.readable() == 0) {
                continue;
            }
            node_data_.push_back(node->buf.readable_data());
            for (const IoBuf &kept: retained_) {
                if (kept.same_storage(node->buf)) {
                    ++shared_with_retained_;
                }
            }
        }
        storage_before_write_.push_back(storage);

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
        std::size_t left = written;
        for (const fiber::mem::IoBufNode *node = buffer.front_node(); node != nullptr && left > 0; node = node->next) {
            const std::size_t take = std::min(node->buf.readable(), left);
            written_.insert(written_.end(), node->buf.readable_data(), node->buf.readable_data() + take);
            left -= take;
            if (action.retain && take > 0) {
                retained_.push_back(node->buf);
            }
        }
        if (action.retain) {
            buffer.consume(written);
        } else {
            buffer.consume_and_compact(written);
        }
        if (buffer.readable_bytes() == 0 && buffer.complete() && !action.defer_completion) {
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
    [[nodiscard]] const std::vector<std::size_t> &nodes_before_write() const noexcept { return nodes_before_write_; }
    // Storage the chain pinned at each write: node capacities, counting a
    // run of nodes sharing one storage once.
    [[nodiscard]] const std::vector<std::size_t> &storage_before_write() const noexcept {
        return storage_before_write_;
    }
    [[nodiscard]] const std::vector<const std::uint8_t *> &node_data() const noexcept { return node_data_; }
    [[nodiscard]] const std::vector<std::uint8_t> &written() const noexcept { return written_; }
    [[nodiscard]] std::size_t shared_with_retained() const noexcept { return shared_with_retained_; }
    [[nodiscard]] std::size_t abort_calls() const noexcept { return abort_calls_; }
    [[nodiscard]] IoErr abort_reason() const noexcept { return abort_reason_; }

private:
    std::vector<WriteAction> actions_;
    std::vector<char> *operations_ = nullptr;
    std::vector<std::size_t> buffered_before_write_;
    std::vector<std::size_t> nodes_before_write_;
    std::vector<std::size_t> storage_before_write_;
    std::vector<const std::uint8_t *> node_data_;
    std::vector<std::uint8_t> written_;
    std::vector<IoBuf> retained_;
    std::size_t shared_with_retained_ = 0;
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
                                                       fiber::http::make_http_body_pipe_writer(*writer), options);
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
            {.bytes = static_cast<std::size_t>(64 * 1024)},
            {.bytes = static_cast<std::size_t>(24 * 1024), .complete = true},
    });
    FakeBodyWriter writer({
            {.max_bytes = static_cast<std::size_t>(8 * 1024)},
            {.max_bytes = static_cast<std::size_t>(16 * 1024)},
    });

    auto result = execute_pipe(reader, writer);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(reader.max_bytes(), (std::vector<std::size_t>{std::size_t{64} * 1024, std::size_t{24} * 1024}));
    EXPECT_EQ(writer.buffered_before_write(),
              (std::vector<std::size_t>{std::size_t{64} * 1024, std::size_t{56} * 1024, std::size_t{40} * 1024,
                                        std::size_t{24} * 1024}));
    EXPECT_EQ(result->bytes_read, 88U * 1024U);
    EXPECT_EQ(result->bytes_written, 88U * 1024U);
    EXPECT_EQ(result->peak_buffered_bytes, 64U * 1024U);
    EXPECT_EQ(result->read_calls, 2U);
    EXPECT_EQ(result->write_calls, 4U);
}

TEST(HttpBodyPipeTest, AccumulatesUntilExactLowWaterAndForwardsDelayedCompletion) {
    FakeBodyReader reader({
            {.bytes = static_cast<std::size_t>(16 * 1024)},
            {.bytes = static_cast<std::size_t>(16 * 1024)},
            {.bytes = static_cast<std::size_t>(16 * 1024)},
            {.complete = true},
    });
    FakeBodyWriter writer;

    auto result = execute_pipe(reader, writer);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(reader.max_bytes(), (std::vector<std::size_t>{std::size_t{64} * 1024, std::size_t{48} * 1024,
                                                            std::size_t{32} * 1024, std::size_t{64} * 1024}));
    EXPECT_EQ(writer.buffered_before_write(), (std::vector<std::size_t>{std::size_t{48} * 1024, 0}));
    EXPECT_EQ(result->bytes_written, 48U * 1024U);
}

TEST(HttpBodyPipeTest, EndOfInputFlushesBelowLowWater) {
    FakeBodyReader reader({
            {.bytes = static_cast<std::size_t>(10 * 1024), .complete = true},
    });
    FakeBodyWriter writer;

    auto result = execute_pipe(reader, writer);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(writer.buffered_before_write(), (std::vector<std::size_t>{std::size_t{10} * 1024}));
    EXPECT_EQ(result->bytes_written, 10U * 1024U);
}

TEST(HttpBodyPipeTest, EndOfInputDrainsPartialWritesAndDelayedCompletionBeforeReturning) {
    std::vector<char> operations;
    FakeBodyReader reader({{.bytes = 10}, {.bytes = 5, .complete = true}}, &operations);
    FakeBodyWriter writer({{.max_bytes = 4},
                           {.max_bytes = 2},
                           {.max_bytes = 4},
                           {.max_bytes = 2},
                           {.max_bytes = 2},
                           {.max_bytes = 1, .defer_completion = true}},
                          &operations);
    const auto result = execute_pipe(reader, writer, {.buffer_size = 16, .low_water = 8});

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->bytes_read, 15U);
    EXPECT_EQ(result->bytes_written, 15U);
    EXPECT_EQ(result->read_calls, 2U);
    EXPECT_EQ(result->write_calls, 7U);
    EXPECT_EQ(writer.buffered_before_write(), (std::vector<std::size_t>{10, 6, 4, 5, 3, 1, 0}));
    EXPECT_EQ(operations, (std::vector<char>{'R', 'W', 'R', 'W', 'W', 'W', 'W', 'W', 'W'}));
    EXPECT_EQ(reader.abort_calls(), 0U);
    EXPECT_EQ(writer.abort_calls(), 0U);
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
    EXPECT_EQ(reader.abort_calls(), 1U);
    EXPECT_EQ(writer.abort_calls(), 1U);
    EXPECT_EQ(writer.abort_reason(), IoErr::TimedOut);
}

TEST(HttpBodyPipeTest, DrainsBufferedBytesBeforeReportingReadError) {
    std::vector<char> operations;
    FakeBodyReader reader(
            {
                    {.bytes = static_cast<std::size_t>(10 * 1024)},
                    {.error = IoErr::ConnReset},
            },
            &operations);
    FakeBodyWriter writer({}, &operations);

    auto result = execute_pipe(reader, writer);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IoErr::ConnReset);
    EXPECT_EQ(result.error().phase, HttpBodyPipePhase::Read);
    EXPECT_EQ(operations, (std::vector<char>{'R', 'R', 'W'}));
    EXPECT_EQ(writer.buffered_before_write(), (std::vector<std::size_t>{std::size_t{10} * 1024}));
    EXPECT_EQ(reader.abort_calls(), 1U);
    EXPECT_EQ(writer.abort_calls(), 1U);
    EXPECT_EQ(writer.abort_reason(), IoErr::ConnReset);
}

TEST(HttpBodyPipeTest, WriteErrorAbortsBothEndpointsAndReportsPhase) {
    FakeBodyReader reader({
            {.bytes = static_cast<std::size_t>(48 * 1024)},
    });
    FakeBodyWriter writer({
            {.error = IoErr::BrokenPipe},
    });

    auto result = execute_pipe(reader, writer);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IoErr::BrokenPipe);
    EXPECT_EQ(result.error().phase, HttpBodyPipePhase::Write);
    EXPECT_EQ(reader.abort_calls(), 1U);
    EXPECT_EQ(reader.abort_reason(), IoErr::BrokenPipe);
    EXPECT_EQ(writer.abort_calls(), 1U);
}

TEST(HttpBodyPipeTest, ZeroLowWaterDrainsEachReadBeforeReadingAgain) {
    std::vector<char> operations;
    FakeBodyReader reader(
            {
                    {.bytes = static_cast<std::size_t>(8 * 1024)},
                    {.bytes = static_cast<std::size_t>(6 * 1024), .complete = true},
            },
            &operations);
    FakeBodyWriter writer(
            {
                    {.max_bytes = static_cast<std::size_t>(3 * 1024)},
                    {.max_bytes = static_cast<std::size_t>(5 * 1024)},
            },
            &operations);
    HttpBodyPipeOptions options;
    options.low_water = kUnbufferedBodyPipeLowWater;

    auto result = execute_pipe(reader, writer, options);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(operations, (std::vector<char>{'R', 'W', 'W', 'F', 'R', 'W'}));
    EXPECT_EQ(reader.max_bytes(), (std::vector<std::size_t>{std::size_t{64} * 1024, std::size_t{64} * 1024}));
    EXPECT_EQ(writer.buffered_before_write(),
              (std::vector<std::size_t>{std::size_t{8} * 1024, std::size_t{5} * 1024, std::size_t{6} * 1024}));
    EXPECT_EQ(result->bytes_read, 14U * 1024U);
    EXPECT_EQ(result->bytes_written, 14U * 1024U);
    EXPECT_EQ(result->peak_buffered_bytes, 8U * 1024U);
    EXPECT_EQ(result->read_calls, 2U);
    EXPECT_EQ(result->write_calls, 3U);
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
    EXPECT_EQ(result->bytes_read, 0U);
    EXPECT_EQ(result->bytes_written, 0U);
    EXPECT_EQ(result->read_calls, 1U);
    EXPECT_EQ(result->write_calls, 1U);
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
    EXPECT_EQ(reader.abort_calls(), 1U);
    EXPECT_EQ(reader.abort_reason(), IoErr::BrokenPipe);
    EXPECT_EQ(writer.abort_calls(), 1U);
    EXPECT_EQ(writer.abort_reason(), IoErr::BrokenPipe);
}

TEST(HttpBodyPipeTest, CoalescesTrickledReadsInsteadOfPinningEachReadBuffer) {
    std::vector<ReadAction> actions(300, ReadAction{.bytes = 8, .sparse = true});
    actions.back().complete = true;
    FakeBodyReader reader(std::move(actions));
    FakeBodyWriter writer;

    auto result = execute_pipe(reader, writer, {.buffer_size = 4096, .low_water = 2048});

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(writer.buffered_before_write(), (std::vector<std::size_t>{2048, 352}));
    // 256 reads, each received into its own buffer of up to 4 KiB, share one
    // coalesce buffer instead of pinning ~770 KiB.
    EXPECT_EQ(writer.nodes_before_write(), (std::vector<std::size_t>{1, 1}));
    EXPECT_EQ(writer.storage_before_write(), (std::vector<std::size_t>{4096, 4096}));
    EXPECT_EQ(writer.written(), pattern_bytes(2400));
    EXPECT_EQ(result->bytes_read, 2400U);
    EXPECT_EQ(result->bytes_written, 2400U);
    EXPECT_EQ(result->read_calls, 300U);
}

TEST(HttpBodyPipeTest, KeepsDenseReadsZeroCopyBetweenCoalescedOnes) {
    FakeBodyReader reader({
            {.bytes = 8, .sparse = true},
            {.bytes = 1024},
            {.bytes = 8, .complete = true, .sparse = true},
    });
    FakeBodyWriter writer;

    auto result = execute_pipe(reader, writer, {.buffer_size = 4096, .low_water = 2048});

    ASSERT_TRUE(result.has_value());
    // The second sparse read must not join the first one's coalesce buffer
    // ahead of the dense read: it opens its own.
    EXPECT_EQ(writer.nodes_before_write(), (std::vector<std::size_t>{3}));
    ASSERT_EQ(writer.node_data().size(), 3U);
    ASSERT_EQ(reader.delivered_data().size(), 3U);
    EXPECT_NE(writer.node_data()[0], reader.delivered_data()[0]);
    EXPECT_EQ(writer.node_data()[1], reader.delivered_data()[1]);
    EXPECT_NE(writer.node_data()[2], reader.delivered_data()[2]);
    EXPECT_EQ(writer.written(), pattern_bytes(1040));
}

TEST(HttpBodyPipeTest, CoalescesSmallExactlySizedReadsBetweenSparseOnes) {
    std::vector<ReadAction> actions(16, ReadAction{.bytes = 8});
    for (std::size_t i = 0; i < actions.size(); i += 2) {
        actions[i].sparse = true;
    }
    actions.back().complete = true;
    FakeBodyReader reader(std::move(actions));
    FakeBodyWriter writer;

    auto result = execute_pipe(reader, writer, {.buffer_size = 4096, .low_water = 128});

    ASSERT_TRUE(result.has_value());
    // A kept 8-byte node would end each run, opening a 4 KiB coalesce buffer
    // per sparse read.
    EXPECT_EQ(writer.nodes_before_write(), (std::vector<std::size_t>{1}));
    EXPECT_EQ(writer.storage_before_write(), (std::vector<std::size_t>{4096}));
    EXPECT_EQ(writer.written(), pattern_bytes(128));
}

TEST(HttpBodyPipeTest, NeverCoalescesIntoStorageTheSinkStillHolds) {
    FakeBodyReader reader({
            {.bytes = 8, .sparse = true},
            {.bytes = 8, .sparse = true},
            {.bytes = 8, .sparse = true},
            {.bytes = 8, .complete = true, .sparse = true},
    });
    FakeBodyWriter writer({{.retain = true}, {.retain = true}});

    auto result = execute_pipe(reader, writer, {.buffer_size = 4096, .low_water = 16});

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(writer.buffered_before_write(), (std::vector<std::size_t>{16, 16}));
    // The drained coalesce buffer stays in the chain with free room, but the
    // sink's handle makes it shared: the later reads open a fresh buffer.
    EXPECT_EQ(writer.nodes_before_write(), (std::vector<std::size_t>{1, 2}));
    EXPECT_EQ(writer.shared_with_retained(), 0U);
    EXPECT_EQ(writer.written(), pattern_bytes(32));
}

TEST(HttpBodyPipeTest, ZeroLowWaterForwardsSparseReadsUncopied) {
    FakeBodyReader reader({
            {.bytes = 8, .sparse = true},
            {.bytes = 8, .complete = true, .sparse = true},
    });
    FakeBodyWriter writer;
    HttpBodyPipeOptions options;
    options.low_water = kUnbufferedBodyPipeLowWater;

    auto result = execute_pipe(reader, writer, options);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(writer.node_data(), reader.delivered_data());
    EXPECT_EQ(writer.written(), pattern_bytes(16));
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
        EXPECT_EQ(reader.abort_calls(), 0U);
        EXPECT_EQ(writer.abort_calls(), 0U);
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
        EXPECT_EQ(reader.abort_calls(), 1U);
        EXPECT_EQ(writer.abort_calls(), 1U);
    }

    {
        FakeBodyReader reader({
                {.bytes = static_cast<std::size_t>(48 * 1024)},
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
