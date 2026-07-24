#include "SseParser.h"

#include <limits>

#include <common/json/Utf.h>

namespace fiber::ai_server {
namespace {

std::string_view field_value(std::string_view line, std::size_t prefix) noexcept {
    std::string_view value = line.substr(prefix);
    if (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1);
    }
    return value;
}

} // namespace

bool SseParser::feed(std::string_view chunk, bool final) noexcept {
    if (failed_ || final_) {
        error_ = SseParseError::FeedAfterFinal;
        failed_ = true;
        return false;
    }
    if (chunk.size() > max_event_bytes_ || pending_.size() > max_event_bytes_ - chunk.size()) {
        error_ = SseParseError::EventTooLarge;
        failed_ = true;
        return false;
    }
    pending_.append(chunk);
    final_ = final;
    return true;
}

std::size_t SseParser::find_event_end() const noexcept {
    std::size_t line_start = 0;
    while (line_start < pending_.size()) {
        const std::size_t newline = pending_.find('\n', line_start);
        if (newline == std::string::npos) {
            break;
        }
        std::size_t line_end = newline;
        if (line_end > line_start && pending_[line_end - 1] == '\r') {
            --line_end;
        }
        if (line_end == line_start) {
            return newline + 1;
        }
        line_start = newline + 1;
    }
    if (final_ && !pending_.empty()) {
        return pending_.size();
    }
    return std::string::npos;
}

bool SseParser::parse_event(std::string_view input) noexcept {
    if (!json::utf8_validate(input.data(), input.size())) {
        error_ = SseParseError::InvalidUtf8;
        failed_ = true;
        return false;
    }

    encoded_.clear();
    data_.clear();
    event_type_.clear();
    bool has_field = false;
    bool has_data = false;
    std::size_t line_start = 0;
    while (line_start < input.size()) {
        const std::size_t newline = input.find('\n', line_start);
        std::size_t line_end = newline == std::string_view::npos ? input.size() : newline;
        if (line_end > line_start && input[line_end - 1] == '\r') {
            --line_end;
        }
        const std::string_view line = input.substr(line_start, line_end - line_start);
        if (line.empty()) {
            break;
        }
        if (line.front() == ':') {
            encoded_.append(line);
            encoded_.push_back('\n');
            has_field = true;
        } else if (line.starts_with("event:")) {
            event_type_.assign(field_value(line, 6));
            has_field = true;
        } else if (line.starts_with("data:")) {
            const std::string_view value = field_value(line, 5);
            if (has_data) {
                data_.push_back('\n');
            }
            data_.append(value);
            encoded_.append("data: ");
            encoded_.append(value);
            encoded_.push_back('\n');
            has_data = true;
            has_field = true;
        }
        if (newline == std::string_view::npos) {
            break;
        }
        line_start = newline + 1;
    }

    if (!has_field) {
        event_ = {};
        return true;
    }
    if (protocol_ == LlmWireProtocol::AnthropicMessages && !event_type_.empty()) {
        std::string with_event;
        with_event.reserve(encoded_.size() + event_type_.size() + 9);
        with_event.append("event: ");
        with_event.append(event_type_);
        with_event.push_back('\n');
        with_event.append(encoded_);
        encoded_ = std::move(with_event);
    }
    encoded_.push_back('\n');

    const bool terminal = protocol_ == LlmWireProtocol::OpenAiChatCompletions && data_ == "[DONE]";
    if (terminal) {
        if (done_seen_) {
            event_ = {};
            return true;
        }
        done_seen_ = true;
    }
    event_ = SseEventView{
            .encoded = encoded_,
            .data = data_,
            .event_type = event_type_,
            .terminal = terminal,
    };
    return true;
}

SseParseStatus SseParser::next() noexcept {
    if (failed_) {
        return SseParseStatus::Error;
    }
    for (;;) {
        const std::size_t event_end = find_event_end();
        if (event_end == std::string::npos) {
            if (pending_.size() >= max_event_bytes_) {
                error_ = SseParseError::EventTooLarge;
                failed_ = true;
                return SseParseStatus::Error;
            }
            return final_ ? SseParseStatus::Complete : SseParseStatus::NeedMore;
        }
        std::string raw = pending_.substr(0, event_end);
        pending_.erase(0, event_end);
        if (!parse_event(raw)) {
            return SseParseStatus::Error;
        }
        if (!event_.encoded.empty()) {
            return SseParseStatus::Event;
        }
        if (pending_.empty() && final_) {
            return SseParseStatus::Complete;
        }
    }
}

} // namespace fiber::ai_server
