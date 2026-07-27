#include "LlmAuditWriter.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <common/Assert.h>
#include <log/Log.h>

namespace fiber::ai_server {
namespace {

DEFINE_LOGGER(LOG_AUDIT_WRITER, "ai_server.audit_writer");

constexpr std::chrono::seconds kWriteRetryDelay{1};
constexpr int kMaxWriteIov = 64;
constexpr unsigned kMaxArchiveAttempts = 10000;

} // namespace

LlmAuditBuffer::LlmAuditBuffer(std::size_t max_bytes) noexcept : max_bytes_(max_bytes) {}

LlmAuditBuffer::~LlmAuditBuffer() { clear(); }

bool LlmAuditBuffer::append(const char *data, std::size_t size) noexcept {
    if (failed_ || (data == nullptr && size != 0) || size_ > max_bytes_ || size > max_bytes_ - size_) {
        failed_ = true;
        return false;
    }
    std::size_t offset = 0;
    while (offset < size) {
        auto allocate_chunk = [&]() noexcept -> bool {
            const std::size_t remaining = max_bytes_ - size_;
            const std::size_t requested = std::max<std::size_t>(4096, std::min(kChunkBytes, size - offset));
            const std::size_t capacity = std::min(requested, remaining);
            auto *chunk = new (std::nothrow) Chunk;
            if (!chunk) {
                failed_ = true;
                return false;
            }
            chunk->bytes = mem::IoBuf::allocate(capacity);
            if (!chunk->bytes) {
                delete chunk;
                failed_ = true;
                return false;
            }
            if (tail_) {
                tail_->next = chunk;
            } else {
                head_ = chunk;
            }
            tail_ = chunk;
            return true;
        };
        if ((tail_ == nullptr || tail_->bytes.writable() == 0) && !allocate_chunk()) {
            return false;
        }
        std::size_t copy = std::min(size - offset, tail_->bytes.writable());
        if (copy < size - offset) {
            while (copy > 0 && (static_cast<unsigned char>(data[offset + copy]) & 0xc0) == 0x80) {
                --copy;
            }
            if (copy == 0) {
                if (!allocate_chunk()) {
                    return false;
                }
                continue;
            }
        }
        std::memcpy(tail_->bytes.writable_data(), data + offset, copy);
        tail_->bytes.commit(copy);
        size_ += copy;
        offset += copy;
    }
    return true;
}

bool LlmAuditBuffer::assign(std::string_view value) noexcept {
    clear();
    failed_ = false;
    return append(value);
}

void LlmAuditBuffer::clear() noexcept {
    Chunk *chunk = head_;
    while (chunk) {
        Chunk *next = chunk->next;
        delete chunk;
        chunk = next;
    }
    head_ = nullptr;
    tail_ = nullptr;
    size_ = 0;
}

LlmAuditBuffer::Cursor LlmAuditBuffer::cursor() const noexcept { return Cursor{.next = head_}; }

bool LlmAuditBuffer::next_chunk(void *context, const char *&data, std::size_t &size, bool &done) noexcept {
    auto &cursor = *static_cast<Cursor *>(context);
    if (!cursor.next) {
        data = nullptr;
        size = 0;
        done = true;
        return true;
    }
    const Chunk *chunk = cursor.next;
    cursor.next = chunk->next;
    data = reinterpret_cast<const char *>(chunk->bytes.readable_data());
    size = chunk->bytes.readable();
    done = cursor.next == nullptr;
    return true;
}

int LlmAuditBuffer::fill_iov(struct iovec *iov, int max_iov) const noexcept {
    int count = 0;
    for (Chunk *chunk = head_; chunk && count < max_iov; chunk = chunk->next) {
        if (chunk->bytes.readable() == 0) {
            continue;
        }
        iov[count].iov_base = const_cast<std::uint8_t *>(chunk->bytes.readable_data());
        iov[count].iov_len = chunk->bytes.readable();
        ++count;
    }
    return count;
}

void LlmAuditBuffer::consume(std::size_t bytes) noexcept {
    FIBER_ASSERT(bytes <= size_);
    while (head_ && bytes != 0) {
        const std::size_t take = std::min(bytes, head_->bytes.readable());
        head_->bytes.consume(take);
        size_ -= take;
        bytes -= take;
        if (head_->bytes.readable() != 0) {
            break;
        }
        Chunk *completed = head_;
        head_ = head_->next;
        delete completed;
    }
    if (!head_) {
        tail_ = nullptr;
    }
}

LlmAuditRecord::LlmAuditRecord(std::size_t max_bytes) noexcept : bytes_(max_bytes) {}

void LlmAuditRecord::on_notify(LlmAuditRecord *record) noexcept { LlmAuditWriter::on_record_notify(record); }

std::expected<std::unique_ptr<LlmAuditWriter>, LlmAuditWriterError>
LlmAuditWriter::create(event::EventLoop &writer_loop, LlmAuditWriterOptions options) noexcept {
    if (options.path.empty() || options.max_record_bytes == 0 || options.max_pending_records == 0 ||
        options.max_archives == 0) {
        return std::unexpected(LlmAuditWriterError{.code = LlmAuditWriterErrorCode::InvalidOptions});
    }
    auto writer = std::unique_ptr<LlmAuditWriter>(new (std::nothrow) LlmAuditWriter(writer_loop, std::move(options)));
    if (!writer) {
        return std::unexpected(LlmAuditWriterError{.code = LlmAuditWriterErrorCode::Allocate, .system_error = ENOMEM});
    }
    int system_error = 0;
    LlmAuditWriterErrorCode error_code = LlmAuditWriterErrorCode::Open;
    if (!writer->open_file(error_code, system_error)) {
        return std::unexpected(LlmAuditWriterError{
                .code = error_code,
                .system_error = system_error,
        });
    }
    return writer;
}

LlmAuditWriter::LlmAuditWriter(event::EventLoop &writer_loop, LlmAuditWriterOptions options) noexcept :
    loop_(&writer_loop), options_(std::move(options)) {
    stopped_publisher_ = stopped_.acquire_publisher();
    FIBER_ASSERT(stopped_publisher_.has_value());
}

LlmAuditWriter::~LlmAuditWriter() {
    FIBER_ASSERT(outstanding_records_.load(std::memory_order_relaxed) == 0);
    FIBER_ASSERT(head_ == nullptr);
    FIBER_ASSERT(tail_ == nullptr);
    if (retry_timer_.is_in_heap()) {
        FIBER_ASSERT(loop_->in_loop());
        loop_->cancel<LlmAuditWriter, &LlmAuditWriter::retry_timer_>(*this);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

bool LlmAuditWriter::open_file(LlmAuditWriterErrorCode &error_code, int &system_error) noexcept {
    fd_ = ::open(options_.path.c_str(), O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd_ < 0) {
        error_code = LlmAuditWriterErrorCode::Open;
        system_error = errno;
        return false;
    }
    struct stat file_stat{};
    if (::fstat(fd_, &file_stat) != 0) {
        error_code = LlmAuditWriterErrorCode::Stat;
        system_error = errno;
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    if (!S_ISREG(file_stat.st_mode)) {
        error_code = LlmAuditWriterErrorCode::InvalidFile;
        system_error = EINVAL;
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    if (::fchmod(fd_, 0600) != 0) {
        error_code = LlmAuditWriterErrorCode::Open;
        system_error = errno;
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    if (!recover_tail(system_error)) {
        error_code = LlmAuditWriterErrorCode::Recover;
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

bool LlmAuditWriter::recover_tail(int &system_error) noexcept {
    struct stat file_stat{};
    if (::fstat(fd_, &file_stat) != 0) {
        system_error = errno;
        return false;
    }
    if (file_stat.st_size <= 0) {
        active_file_bytes_ = 0;
        system_error = 0;
        return true;
    }
    const auto size = static_cast<std::uint64_t>(file_stat.st_size);
    char last = 0;
    if (::pread(fd_, &last, 1, static_cast<off_t>(size - 1)) != 1) {
        system_error = errno != 0 ? errno : EIO;
        return false;
    }
    if (last == '\n') {
        active_file_bytes_ = size;
        system_error = 0;
        return true;
    }

    std::array<char, 4096> buffer{};
    std::uint64_t cursor = size;
    std::uint64_t keep = 0;
    while (cursor != 0) {
        const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(cursor, buffer.size()));
        cursor -= take;
        const ssize_t read = ::pread(fd_, buffer.data(), take, static_cast<off_t>(cursor));
        if (read != static_cast<ssize_t>(take)) {
            system_error = errno != 0 ? errno : EIO;
            return false;
        }
        for (std::size_t index = take; index != 0; --index) {
            if (buffer[index - 1] == '\n') {
                keep = cursor + index;
                cursor = 0;
                break;
            }
        }
    }
    if (::ftruncate(fd_, static_cast<off_t>(keep)) != 0) {
        system_error = errno;
        return false;
    }
    active_file_bytes_ = keep;
    system_error = 0;
    return true;
}

bool LlmAuditWriter::try_acquire() noexcept {
    if (!ready()) {
        admission_rejections_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::size_t current = outstanding_records_.load(std::memory_order_relaxed);
    while (current < options_.max_pending_records) {
        if (outstanding_records_.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel,
                                                       std::memory_order_relaxed)) {
            if (ready()) {
                return true;
            }
            outstanding_records_.fetch_sub(1, std::memory_order_acq_rel);
            admission_rejections_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    admission_rejections_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void LlmAuditWriter::release_acquired() noexcept {
    const std::size_t previous = outstanding_records_.fetch_sub(1, std::memory_order_acq_rel);
    FIBER_ASSERT(previous > 0);
}

std::unique_ptr<LlmAuditRecord> LlmAuditWriter::make_record() const noexcept {
    return std::unique_ptr<LlmAuditRecord>(new (std::nothrow) LlmAuditRecord(options_.max_record_bytes));
}

bool LlmAuditWriter::submit(std::unique_ptr<LlmAuditRecord> record) noexcept {
    if (!record || record->bytes_.failed() || record->bytes_.empty() ||
        record->bytes_.size() > options_.max_record_bytes) {
        release_acquired();
        return false;
    }
    record->writer_ = this;
    record->original_size_ = record->bytes_.size();
    LlmAuditRecord *submitted = record.release();
    submitted_records_.fetch_add(1, std::memory_order_relaxed);
    loop_->post<LlmAuditRecord, &LlmAuditRecord::notify_entry_, &LlmAuditRecord::on_notify>(*submitted);
    return true;
}

LlmAuditWriterStats LlmAuditWriter::stats() const noexcept {
    return LlmAuditWriterStats{
            .submitted_records = submitted_records_.load(std::memory_order_relaxed),
            .written_records = written_records_.load(std::memory_order_relaxed),
            .written_bytes = written_bytes_.load(std::memory_order_relaxed),
            .write_failures = write_failures_.load(std::memory_order_relaxed),
            .rotations = rotations_.load(std::memory_order_relaxed),
            .rotation_failures = rotation_failures_.load(std::memory_order_relaxed),
            .admission_rejections = admission_rejections_.load(std::memory_order_relaxed),
            .outstanding_records = outstanding_records_.load(std::memory_order_relaxed),
            .healthy = healthy_.load(std::memory_order_acquire),
    };
}

void LlmAuditWriter::on_record_notify(LlmAuditRecord *record) noexcept {
    FIBER_ASSERT(record != nullptr);
    FIBER_ASSERT(record->writer_ != nullptr);
    LlmAuditWriter &writer = *record->writer_;
    FIBER_ASSERT(writer.loop_->in_loop());
    writer.enqueue(record);
    writer.pump();
}

void LlmAuditWriter::enqueue(LlmAuditRecord *record) noexcept {
    record->next_ = nullptr;
    if (tail_) {
        tail_->next_ = record;
    } else {
        head_ = record;
    }
    tail_ = record;
}

bool LlmAuditWriter::should_rotate(const LlmAuditRecord &record) const noexcept {
    if (options_.rotate_bytes == 0 || active_file_bytes_ == 0) {
        return false;
    }
    return active_file_bytes_ >= options_.rotate_bytes ||
           record.original_size_ > options_.rotate_bytes - std::min(options_.rotate_bytes, active_file_bytes_);
}

bool LlmAuditWriter::rotate_file() noexcept {
    std::string archive_path;
    std::uint64_t sequence = next_archive_sequence_;
    for (unsigned attempt = 0; attempt < kMaxArchiveAttempts; ++attempt, ++sequence) {
        archive_path = options_.path;
        archive_path.push_back('.');
        archive_path.append(std::to_string(sequence));
        if (::link(options_.path.c_str(), archive_path.c_str()) == 0) {
            break;
        }
        if (errno != EEXIST || attempt + 1 == kMaxArchiveAttempts) {
            rotation_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    std::string replacement_path = archive_path;
    replacement_path.append(".tmp.");
    replacement_path.append(std::to_string(static_cast<unsigned long long>(::getpid())));
    const int replacement =
            ::open(replacement_path.c_str(), O_RDWR | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (replacement < 0) {
        (void) ::unlink(archive_path.c_str());
        rotation_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (::rename(replacement_path.c_str(), options_.path.c_str()) != 0) {
        ::close(replacement);
        (void) ::unlink(replacement_path.c_str());
        (void) ::unlink(archive_path.c_str());
        rotation_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const int previous = fd_;
    fd_ = replacement;
    ::close(previous);
    active_file_bytes_ = 0;
    next_archive_sequence_ = sequence + 1;
    rotations_.fetch_add(1, std::memory_order_relaxed);
    if (sequence > options_.max_archives) {
        std::string expired = options_.path;
        expired.push_back('.');
        expired.append(std::to_string(sequence - options_.max_archives));
        if (::unlink(expired.c_str()) != 0 && errno != ENOENT) {
            LOG(LOG_AUDIT_WRITER, WARN) << "audit archive retention cleanup failed path=" << log::quoted(expired)
                                        << " errno=" << errno;
        }
    }
    return true;
}

bool LlmAuditWriter::write_front() noexcept {
    FIBER_ASSERT(head_ != nullptr);
    LlmAuditRecord &record = *head_;
    if (!record.write_started_) {
        if (should_rotate(record) && !rotate_file()) {
            LOG(LOG_AUDIT_WRITER, WARN) << "audit rotation failed path=" << log::quoted(options_.path);
        }
        record.write_started_ = true;
    }

    std::array<iovec, kMaxWriteIov> iov{};
    while (!record.bytes_.empty()) {
        const int count = record.bytes_.fill_iov(iov.data(), static_cast<int>(iov.size()));
        FIBER_ASSERT(count > 0);
        const ssize_t written = ::writev(fd_, iov.data(), count);
        if (written > 0) {
            const auto bytes = static_cast<std::size_t>(written);
            record.bytes_.consume(bytes);
            active_file_bytes_ = bytes > std::numeric_limits<std::uint64_t>::max() - active_file_bytes_
                                         ? std::numeric_limits<std::uint64_t>::max()
                                         : active_file_bytes_ + bytes;
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        write_failures_.fetch_add(1, std::memory_order_relaxed);
        healthy_.store(false, std::memory_order_release);
        LOG(LOG_AUDIT_WRITER, ERROR) << "audit write failed path=" << log::quoted(options_.path)
                                     << " errno=" << (written < 0 ? errno : EIO);
        return false;
    }

    LlmAuditRecord *completed = head_;
    head_ = completed->next_;
    if (!head_) {
        tail_ = nullptr;
    }
    written_records_.fetch_add(1, std::memory_order_relaxed);
    written_bytes_.fetch_add(completed->original_size_, std::memory_order_relaxed);
    healthy_.store(true, std::memory_order_release);
    delete completed;
    release_acquired();
    return true;
}

void LlmAuditWriter::pump() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    while (head_) {
        if (!write_front()) {
            schedule_retry();
            return;
        }
    }
    if (shutdown_requested_) {
        finish_shutdown();
    }
}

void LlmAuditWriter::schedule_retry() noexcept {
    if (retry_timer_.is_in_heap()) {
        return;
    }
    loop_->post_at<LlmAuditWriter, &LlmAuditWriter::retry_timer_, &LlmAuditWriter::on_retry>(
            loop_->now() + kWriteRetryDelay, *this);
}

void LlmAuditWriter::on_retry(LlmAuditWriter *writer) noexcept {
    FIBER_ASSERT(writer != nullptr);
    writer->pump();
}

void LlmAuditWriter::on_shutdown_notify(LlmAuditWriter *writer) noexcept {
    FIBER_ASSERT(writer != nullptr);
    FIBER_ASSERT(writer->loop_->in_loop());
    writer->shutdown_requested_ = true;
    writer->pump();
}

void LlmAuditWriter::finish_shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (stopped_notified_ || head_ != nullptr || outstanding_records_.load(std::memory_order_acquire) != 0) {
        return;
    }
    if (retry_timer_.is_in_heap()) {
        loop_->cancel<LlmAuditWriter, &LlmAuditWriter::retry_timer_>(*this);
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    healthy_.store(false, std::memory_order_release);
    stopped_notified_ = true;
    stopped_publisher_->publish(true);
}

async::Task<void> LlmAuditWriter::shutdown() noexcept {
    accepting_.store(false, std::memory_order_release);
    auto stopped = stopped_.subscribe();
    auto snapshot = stopped.current();
    if (snapshot.value && *snapshot.value) {
        co_return;
    }
    loop_->post<LlmAuditWriter, &LlmAuditWriter::shutdown_notify_, &LlmAuditWriter::on_shutdown_notify>(*this);
    while (!snapshot.value || !*snapshot.value) {
        snapshot = co_await stopped.next(snapshot.version);
    }
}

} // namespace fiber::ai_server
