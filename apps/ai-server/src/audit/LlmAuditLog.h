#ifndef FIBER_AI_SERVER_LLM_AUDIT_LOG_H
#define FIBER_AI_SERVER_LLM_AUDIT_LOG_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/common/mem/IoBuf.h>

namespace fiber::ai_server {

inline constexpr std::size_t kDefaultLlmAuditMaxRecordBytes = 128 * 1024 * 1024;
inline constexpr std::uint64_t kDefaultLlmAuditRotateBytes = 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kDefaultLlmAuditMaxArchives = 30;

struct LlmAuditLogOptions {
    std::string path;
    std::size_t max_record_bytes = kDefaultLlmAuditMaxRecordBytes;
    std::uint64_t rotate_bytes = kDefaultLlmAuditRotateBytes;
    std::uint32_t max_archives = kDefaultLlmAuditMaxArchives;
};

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
    struct Chunk {
        mem::IoBuf bytes;
        Chunk *next = nullptr;
    };

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

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_LLM_AUDIT_LOG_H
