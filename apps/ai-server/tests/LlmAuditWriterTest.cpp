#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <unistd.h>

#include <async/Spawn.h>
#include <event/EventLoopGroup.h>

#include "audit/LlmAuditWriter.h"

namespace {

using namespace std::chrono_literals;
using fiber::ai_server::LlmAuditWriter;
using fiber::ai_server::LlmAuditWriterOptions;

class TemporaryAuditPath {
public:
    TemporaryAuditPath() {
        char path[] = "/tmp/fiber-audit-writer-XXXXXX";
        fd_ = ::mkstemp(path);
        if (fd_ >= 0) {
            path_ = path;
            (void) ::close(fd_);
            fd_ = -1;
        }
    }

    ~TemporaryAuditPath() {
        if (fd_ >= 0) {
            (void) ::close(fd_);
        }
        if (!path_.empty()) {
            (void) ::unlink(path_.c_str());
            for (unsigned index = 1; index <= 8; ++index) {
                const std::string archive = path_ + "." + std::to_string(index);
                (void) ::unlink(archive.c_str());
            }
        }
    }

    [[nodiscard]] bool valid() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::string &path() const noexcept { return path_; }

private:
    std::string path_;
    int fd_ = -1;
};

std::string read_file(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool wait_for_written(const LlmAuditWriter &writer, std::uint64_t count) noexcept {
    for (std::size_t index = 0; index < 5000; ++index) {
        if (writer.stats().written_records >= count) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

bool submit(LlmAuditWriter &writer, std::string_view line) noexcept {
    if (!writer.try_acquire()) {
        return false;
    }
    auto record = writer.make_record();
    if (!record) {
        writer.release_acquired();
        return false;
    }
    if (!record->bytes().append(line)) {
        writer.release_acquired();
        return false;
    }
    return writer.submit(std::move(record));
}

void shutdown_writer(fiber::event::EventLoopGroup &group, LlmAuditWriter &writer) {
    std::promise<void> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&writer, &promise]() -> fiber::async::DetachedTask {
        co_await writer.shutdown();
        promise.set_value();
    });
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
}

TEST(LlmAuditWriterTest, WritesWholeRecordsAndDrainsBeforeShutdown) {
    TemporaryAuditPath path;
    ASSERT_TRUE(path.valid());
    fiber::event::EventLoopGroup group(1);
    auto created = LlmAuditWriter::create(group.at(0), LlmAuditWriterOptions{
                                                               .path = path.path(),
                                                               .rotate_bytes = 0,
                                                       });
    ASSERT_TRUE(created);
    std::unique_ptr<LlmAuditWriter> writer = std::move(*created);
    group.start();

    ASSERT_TRUE(submit(*writer, "{\"id\":1}\n"));
    ASSERT_TRUE(submit(*writer, "{\"id\":2,\"content\":\"complete\"}\n"));
    ASSERT_TRUE(wait_for_written(*writer, 2));
    shutdown_writer(group, *writer);
    group.stop();
    group.join();

    EXPECT_EQ(read_file(path.path()), "{\"id\":1}\n{\"id\":2,\"content\":\"complete\"}\n");
    EXPECT_EQ(writer->stats().outstanding_records, 0u);
}

TEST(LlmAuditWriterTest, RecoversOnlyTheIncompleteTailAtStartup) {
    TemporaryAuditPath path;
    ASSERT_TRUE(path.valid());
    {
        std::ofstream output(path.path(), std::ios::binary | std::ios::trunc);
        output << "{\"id\":1}\n{\"id\":2";
    }
    fiber::event::EventLoop loop;
    auto created = LlmAuditWriter::create(loop, LlmAuditWriterOptions{
                                                        .path = path.path(),
                                                        .rotate_bytes = 0,
                                                });
    ASSERT_TRUE(created);
    created->reset();

    EXPECT_EQ(read_file(path.path()), "{\"id\":1}\n");
}

TEST(LlmAuditWriterTest, AppliesBoundedAdmissionBeforeQueueing) {
    TemporaryAuditPath path;
    ASSERT_TRUE(path.valid());
    fiber::event::EventLoop loop;
    auto created = LlmAuditWriter::create(loop, LlmAuditWriterOptions{
                                                        .path = path.path(),
                                                        .max_pending_records = 2,
                                                        .rotate_bytes = 0,
                                                });
    ASSERT_TRUE(created);
    LlmAuditWriter &writer = **created;

    EXPECT_TRUE(writer.try_acquire());
    EXPECT_TRUE(writer.try_acquire());
    EXPECT_FALSE(writer.try_acquire());
    writer.release_acquired();
    writer.release_acquired();
    EXPECT_EQ(writer.stats().outstanding_records, 0u);
    EXPECT_EQ(writer.stats().admission_rejections, 1u);
}

TEST(LlmAuditWriterTest, RotatesOnlyBetweenCompleteRecords) {
    TemporaryAuditPath path;
    ASSERT_TRUE(path.valid());
    fiber::event::EventLoopGroup group(1);
    auto created = LlmAuditWriter::create(group.at(0), LlmAuditWriterOptions{
                                                               .path = path.path(),
                                                               .rotate_bytes = 12,
                                                               .max_archives = 2,
                                                       });
    ASSERT_TRUE(created);
    std::unique_ptr<LlmAuditWriter> writer = std::move(*created);
    group.start();

    ASSERT_TRUE(submit(*writer, "{\"id\":1}\n"));
    ASSERT_TRUE(submit(*writer, "{\"id\":2}\n"));
    ASSERT_TRUE(wait_for_written(*writer, 2));
    shutdown_writer(group, *writer);
    group.stop();
    group.join();

    EXPECT_EQ(read_file(path.path() + ".1"), "{\"id\":1}\n");
    EXPECT_EQ(read_file(path.path()), "{\"id\":2}\n");
}

} // namespace
