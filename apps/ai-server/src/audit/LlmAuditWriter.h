#ifndef FIBER_AI_SERVER_LLM_AUDIT_WRITER_H
#define FIBER_AI_SERVER_LLM_AUDIT_WRITER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/uio.h>

#include <async/Task.h>
#include <async/Watch.h>
#include <common/IoError.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <common/mem/IoBuf.h>
#include <event/EventLoop.h>

namespace fiber::ai_server {

inline constexpr std::size_t kDefaultLlmAuditMaxRecordBytes = 128 * 1024 * 1024;
inline constexpr std::size_t kDefaultLlmAuditMaxPendingRecords = 256;
inline constexpr std::uint64_t kDefaultLlmAuditRotateBytes = 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kDefaultLlmAuditMaxArchives = 30;

struct LlmAuditWriterOptions {
    std::string path;
    std::size_t max_record_bytes = kDefaultLlmAuditMaxRecordBytes;
    std::size_t max_pending_records = kDefaultLlmAuditMaxPendingRecords;
    std::uint64_t rotate_bytes = kDefaultLlmAuditRotateBytes;
    std::uint32_t max_archives = kDefaultLlmAuditMaxArchives;
};

enum class LlmAuditWriterErrorCode : std::uint8_t {
    InvalidOptions,
    Allocate,
    Open,
    Stat,
    InvalidFile,
    Recover,
};

struct LlmAuditWriterError {
    LlmAuditWriterErrorCode code = LlmAuditWriterErrorCode::InvalidOptions;
    int system_error = 0;
};

struct LlmAuditWriterStats {
    std::uint64_t submitted_records = 0;
    std::uint64_t written_records = 0;
    std::uint64_t written_bytes = 0;
    std::uint64_t write_failures = 0;
    std::uint64_t rotations = 0;
    std::uint64_t rotation_failures = 0;
    std::uint64_t admission_rejections = 0;
    std::size_t outstanding_records = 0;
    bool healthy = false;
};

class LlmAuditWriter;

class LlmAuditBuffer final : public common::NonCopyable, public common::NonMovable {
public:
    struct Cursor;

    explicit LlmAuditBuffer(std::size_t max_bytes = static_cast<std::size_t>(-1)) noexcept;
    ~LlmAuditBuffer();

    [[nodiscard]] bool append(const char *data, std::size_t size) noexcept;
    [[nodiscard]] bool append(std::string_view value) noexcept { return append(value.data(), value.size()); }
    [[nodiscard]] bool append(char value) noexcept { return append(&value, 1); }
    [[nodiscard]] bool assign(std::string_view value) noexcept;

    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] Cursor cursor() const noexcept;

    static bool next_chunk(void *context, const char *&data, std::size_t &size, bool &done) noexcept;

private:
    friend class LlmAuditWriter;

    struct Chunk {
        mem::IoBuf bytes;
        Chunk *next = nullptr;
    };

    [[nodiscard]] int fill_iov(struct iovec *iov, int max_iov) const noexcept;
    void consume(std::size_t bytes) noexcept;

    static constexpr std::size_t kChunkBytes = 64 * 1024;

    Chunk *head_ = nullptr;
    Chunk *tail_ = nullptr;
    std::size_t size_ = 0;
    std::size_t max_bytes_ = 0;
    bool failed_ = false;
};

struct LlmAuditBuffer::Cursor {
    const Chunk *next = nullptr;
};

class LlmAuditRecord final : public common::NonCopyable, public common::NonMovable {
public:
    explicit LlmAuditRecord(std::size_t max_bytes) noexcept;
    ~LlmAuditRecord() = default;

    [[nodiscard]] LlmAuditBuffer &bytes() noexcept { return bytes_; }
    [[nodiscard]] const LlmAuditBuffer &bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

private:
    friend class LlmAuditWriter;

    static void on_notify(LlmAuditRecord *record) noexcept;

    LlmAuditBuffer bytes_;
    event::EventLoop::NotifyEntry notify_entry_{};
    LlmAuditWriter *writer_ = nullptr;
    LlmAuditRecord *next_ = nullptr;
    std::size_t original_size_ = 0;
    bool write_started_ = false;
};

class LlmAuditWriter final : public common::NonCopyable, public common::NonMovable {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<LlmAuditWriter>, LlmAuditWriterError>
    create(event::EventLoop &writer_loop, LlmAuditWriterOptions options) noexcept;

    ~LlmAuditWriter();

    [[nodiscard]] bool try_acquire() noexcept;
    void release_acquired() noexcept;
    [[nodiscard]] std::unique_ptr<LlmAuditRecord> make_record() const noexcept;
    [[nodiscard]] bool submit(std::unique_ptr<LlmAuditRecord> record) noexcept;

    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return accepting_.load(std::memory_order_acquire) && healthy_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t max_record_bytes() const noexcept { return options_.max_record_bytes; }
    [[nodiscard]] LlmAuditWriterStats stats() const noexcept;

private:
    friend class LlmAuditRecord;

    explicit LlmAuditWriter(event::EventLoop &writer_loop, LlmAuditWriterOptions options) noexcept;

    [[nodiscard]] bool open_file(LlmAuditWriterErrorCode &error_code, int &system_error) noexcept;
    [[nodiscard]] bool recover_tail(int &system_error) noexcept;
    [[nodiscard]] bool should_rotate(const LlmAuditRecord &record) const noexcept;
    [[nodiscard]] bool rotate_file() noexcept;
    [[nodiscard]] bool write_front() noexcept;

    void enqueue(LlmAuditRecord *record) noexcept;
    void pump() noexcept;
    void schedule_retry() noexcept;
    void finish_shutdown() noexcept;

    static void on_record_notify(LlmAuditRecord *record) noexcept;
    static void on_retry(LlmAuditWriter *writer) noexcept;
    static void on_shutdown_notify(LlmAuditWriter *writer) noexcept;

    event::EventLoop *loop_ = nullptr;
    LlmAuditWriterOptions options_;
    async::Watch<bool> stopped_{false};
    std::optional<async::Watch<bool>::Publisher> stopped_publisher_;
    event::EventLoop::NotifyEntry shutdown_notify_{};
    event::EventLoop::TimerEntry retry_timer_{};
    LlmAuditRecord *head_ = nullptr;
    LlmAuditRecord *tail_ = nullptr;
    std::atomic<std::size_t> outstanding_records_{0};
    std::atomic<bool> accepting_{true};
    std::atomic<bool> healthy_{true};
    std::atomic<std::uint64_t> submitted_records_{0};
    std::atomic<std::uint64_t> written_records_{0};
    std::atomic<std::uint64_t> written_bytes_{0};
    std::atomic<std::uint64_t> write_failures_{0};
    std::atomic<std::uint64_t> rotations_{0};
    std::atomic<std::uint64_t> rotation_failures_{0};
    std::atomic<std::uint64_t> admission_rejections_{0};
    std::uint64_t active_file_bytes_ = 0;
    std::uint64_t next_archive_sequence_ = 1;
    int fd_ = -1;
    bool shutdown_requested_ = false;
    bool stopped_notified_ = false;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_LLM_AUDIT_WRITER_H
