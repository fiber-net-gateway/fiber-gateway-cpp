#ifndef FIBER_AI_SERVER_SSE_PARSER_H
#define FIBER_AI_SERVER_SSE_PARSER_H

#include "LlmBody.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fiber::ai_server {

enum class SseParseStatus : std::uint8_t {
    Event,
    NeedMore,
    Complete,
    Error,
};

enum class SseParseError : std::uint8_t {
    EventTooLarge,
    InvalidUtf8,
    FeedAfterFinal,
};

struct SseEventView {
    std::string_view encoded;
    std::string_view data;
    std::string_view event_type;
    bool terminal = false;
};

class SseParser {
public:
    explicit SseParser(LlmWireProtocol protocol, std::size_t max_event_bytes = 1024 * 1024) noexcept :
        protocol_(protocol), max_event_bytes_(max_event_bytes) {}

    [[nodiscard]] bool feed(std::string_view chunk, bool final) noexcept;
    [[nodiscard]] SseParseStatus next() noexcept;

    [[nodiscard]] const SseEventView &event() const noexcept { return event_; }
    [[nodiscard]] SseParseError error() const noexcept { return error_; }
    [[nodiscard]] bool done_seen() const noexcept { return done_seen_; }

private:
    [[nodiscard]] std::size_t find_event_end() const noexcept;
    [[nodiscard]] bool parse_event(std::string_view input) noexcept;

    LlmWireProtocol protocol_ = LlmWireProtocol::OpenAiChatCompletions;
    std::size_t max_event_bytes_ = 0;
    std::string pending_;
    std::string encoded_;
    std::string data_;
    std::string event_type_;
    SseEventView event_;
    SseParseError error_ = SseParseError::EventTooLarge;
    bool final_ = false;
    bool failed_ = false;
    bool done_seen_ = false;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_SSE_PARSER_H
