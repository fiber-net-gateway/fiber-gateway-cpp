#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http_script/ScriptRequestBody.h>

namespace {

using fiber::async::DetachedTask;
using fiber::common::IoErr;
using fiber::common::IoResult;
using fiber::mem::IoBuf;
using fiber::mem::IoBufChain;

struct ReadStep {
    std::vector<std::string> nodes;
    bool complete = false;
    IoErr error = IoErr::None;
};

// A source that delivers exactly what the test scripts, ignoring max_bytes and any declared
// length, which is what read_body_into must not trust.
class ScriptedBody {
public:
    explicit ScriptedBody(std::vector<ReadStep> steps) noexcept : steps_(std::move(steps)) {}

    fiber::async::Task<IoResult<IoBufChain>> read_body(std::size_t max_bytes,
                                                       std::chrono::milliseconds /*timeout*/) noexcept {
        requested_.push_back(max_bytes);
        IoBufChain chain;
        if (next_ >= steps_.size()) {
            chain.mark_complete();
            co_return chain;
        }
        const ReadStep &step = steps_[next_++];
        if (step.error != IoErr::None) {
            co_return std::unexpected(step.error);
        }
        for (const std::string &bytes: step.nodes) {
            IoBuf node = IoBuf::allocate(bytes.size() + 1);
            std::memcpy(node.writable_data(), bytes.data(), bytes.size());
            node.commit(bytes.size());
            chain.append(std::move(node));
        }
        if (step.complete) {
            chain.mark_complete();
        }
        co_return chain;
    }

    fiber::async::Task<IoResult<void>> discard_body(std::chrono::milliseconds /*timeout*/) noexcept {
        co_return IoResult<void>{};
    }

    IoResult<void> abort(IoErr /*reason*/) noexcept { return {}; }

    [[nodiscard]] const std::vector<std::size_t> &requested() const noexcept { return requested_; }

private:
    std::vector<ReadStep> steps_;
    std::vector<std::size_t> requested_;
    std::size_t next_ = 0;
};

constexpr std::size_t kCanaryBytes = 8;

struct ReadOutcome {
    IoResult<void> result;
    std::string buffer; // len bytes of destination, then the canary
};

DetachedTask run_read(ScriptedBody *body, std::uint8_t *dst, std::size_t len, std::promise<IoResult<void>> *promise) {
    auto result = co_await fiber::http_script::read_body_into(fiber::http_script::make_script_request_body(*body), dst,
                                                              len, std::chrono::seconds(1));
    promise->set_value(std::move(result));
    fiber::event::EventLoop::current().stop();
    co_return;
}

ReadOutcome execute_read(ScriptedBody &body, std::size_t len) {
    ReadOutcome outcome;
    outcome.buffer.assign(len + kCanaryBytes, '#');
    fiber::event::EventLoopGroup group(1);
    std::promise<IoResult<void>> promise;
    auto future = promise.get_future();
    auto *dst = reinterpret_cast<std::uint8_t *>(outcome.buffer.data());

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return run_read(&body, dst, len, &promise); });
    outcome.result = future.get();
    group.join();
    return outcome;
}

std::string_view canary(const ReadOutcome &outcome, std::size_t len) {
    return std::string_view(outcome.buffer).substr(len);
}

} // namespace

TEST(ScriptRequestBodyTest, ReadsAcrossNodesAndReads) {
    ScriptedBody body({{.nodes = {"ab", "c"}}, {.nodes = {"defg"}, .complete = true}});
    ReadOutcome outcome = execute_read(body, 7);
    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_EQ(outcome.buffer, "abcdefg########");
}

TEST(ScriptRequestBodyTest, CompletionMayArriveAfterTheLastByte) {
    ScriptedBody body({{.nodes = {"abc"}}, {.complete = true}});
    ReadOutcome outcome = execute_read(body, 3);
    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_EQ(outcome.buffer, "abc########");
}

TEST(ScriptRequestBodyTest, RefusesAnOverrunWithoutWritingIt) {
    ScriptedBody body({{.nodes = {"abcdefgh"}, .complete = true}});
    ReadOutcome outcome = execute_read(body, 4);
    ASSERT_FALSE(outcome.result.has_value());
    EXPECT_EQ(outcome.result.error(), IoErr::Invalid);
    EXPECT_EQ(outcome.buffer, std::string(4 + kCanaryBytes, '#'));
}

TEST(ScriptRequestBodyTest, RefusesAnOverrunAfterTheBufferIsFull) {
    ScriptedBody body({{.nodes = {"abcd"}}, {.nodes = {"e"}, .complete = true}});
    ReadOutcome outcome = execute_read(body, 4);
    ASSERT_FALSE(outcome.result.has_value());
    EXPECT_EQ(outcome.result.error(), IoErr::Invalid);
    EXPECT_EQ(canary(outcome, 4), std::string(kCanaryBytes, '#'));
}

TEST(ScriptRequestBodyTest, RefusesAShortBody) {
    ScriptedBody body({{.nodes = {"ab"}, .complete = true}});
    ReadOutcome outcome = execute_read(body, 4);
    ASSERT_FALSE(outcome.result.has_value());
    EXPECT_EQ(outcome.result.error(), IoErr::Invalid);
}

TEST(ScriptRequestBodyTest, RefusesAnEmptyIncompleteRead) {
    ScriptedBody body({{}});
    ReadOutcome outcome = execute_read(body, 4);
    ASSERT_FALSE(outcome.result.has_value());
    EXPECT_EQ(outcome.result.error(), IoErr::Invalid);
}

TEST(ScriptRequestBodyTest, ReportsReadErrors) {
    ScriptedBody body({{.nodes = {"ab"}}, {.error = IoErr::ConnReset}});
    ReadOutcome outcome = execute_read(body, 4);
    ASSERT_FALSE(outcome.result.has_value());
    EXPECT_EQ(outcome.result.error(), IoErr::ConnReset);
}

TEST(ScriptRequestBodyTest, AsksForOneByteBeyondTheEndAtMost64K) {
    ScriptedBody small({{.nodes = {"abc"}, .complete = true}});
    ASSERT_TRUE(execute_read(small, 3).result.has_value());
    EXPECT_EQ(small.requested(), (std::vector<std::size_t>{4}));

    const std::string full(std::size_t{1024} * 1024, 'x');
    ScriptedBody large({{.nodes = {full}, .complete = true}});
    ASSERT_TRUE(execute_read(large, full.size()).result.has_value());
    EXPECT_EQ(large.requested(), (std::vector<std::size_t>{std::size_t{64} * 1024}));
}
