#include "LlmError.h"

#include <algorithm>
#include <cstring>

#include <fiber/common/json/JsonEncode.h>

namespace fiber::ai_server {
namespace {

class IoBufSink final : public json::OutputSink {
public:
    explicit IoBufSink(mem::IoBuf &output) noexcept : output_(&output) {}

    [[nodiscard]] bool write(const char *data, std::size_t size) override {
        if (size == 0) {
            return true;
        }
        const std::size_t required = output_->readable() + size;
        if (!ensure_capacity(required)) {
            return false;
        }
        std::memcpy(output_->writable_data(), data, size);
        output_->commit(size);
        return true;
    }

private:
    [[nodiscard]] bool ensure_capacity(std::size_t required) noexcept {
        if (*output_ && output_->capacity() >= required) {
            return true;
        }
        std::size_t capacity = std::max<std::size_t>(output_->capacity(), 256);
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                capacity = required;
                break;
            }
            capacity *= 2;
        }
        mem::IoBuf replacement = mem::IoBuf::allocate(capacity);
        if (!replacement) {
            return false;
        }
        if (*output_ && output_->readable() > 0) {
            std::memcpy(replacement.writable_data(), output_->readable_data(), output_->readable());
            replacement.commit(output_->readable());
        }
        *output_ = std::move(replacement);
        return true;
    }

    mem::IoBuf *output_ = nullptr;
};

bool ok(json::Generator::Result result) noexcept { return result == json::Generator::Result::OK; }

LlmErrorEncodeError encode_error(json::Generator::Result result, const mem::IoBuf &output) noexcept {
    if (!output) {
        return LlmErrorEncodeError::OutOfMemory;
    }
    if (result == json::Generator::Result::InvalidString) {
        return LlmErrorEncodeError::InvalidUtf8;
    }
    return LlmErrorEncodeError::EncodeFailed;
}

std::expected<mem::IoBuf, LlmErrorEncodeError> encode_openai(const LlmError &error) noexcept {
    mem::IoBuf output;
    IoBufSink sink(output);
    json::Generator generator(sink);
    generator.set_option(json::Generator::Option::ValidateUtf8);

    json::Generator::Result result = generator.map_open();
    if (ok(result)) {
        result = generator.string("error", 5);
    }
    if (ok(result)) {
        result = generator.map_open();
    }
    if (ok(result)) {
        result = generator.string("message", 7);
    }
    if (ok(result)) {
        result = generator.string(error.message.data(), error.message.size());
    }
    if (ok(result)) {
        result = generator.string("type", 4);
    }
    if (ok(result)) {
        result = generator.string(error.type.data(), error.type.size());
    }
    if (ok(result)) {
        result = generator.string("param", 5);
    }
    if (ok(result)) {
        result =
                error.field.empty() ? generator.null_value() : generator.string(error.field.data(), error.field.size());
    }
    if (ok(result)) {
        result = generator.string("code", 4);
    }
    if (ok(result)) {
        result = generator.string(error.code.data(), error.code.size());
    }
    if (ok(result)) {
        result = generator.map_close();
    }
    if (ok(result)) {
        result = generator.map_close();
    }
    if (!ok(result)) {
        return std::unexpected(encode_error(result, output));
    }
    return output;
}

std::expected<mem::IoBuf, LlmErrorEncodeError> encode_anthropic(const LlmError &error) noexcept {
    mem::IoBuf output;
    IoBufSink sink(output);
    json::Generator generator(sink);
    generator.set_option(json::Generator::Option::ValidateUtf8);

    json::Generator::Result result = generator.map_open();
    if (ok(result)) {
        result = generator.string("type", 4);
    }
    if (ok(result)) {
        result = generator.string("error", 5);
    }
    if (ok(result)) {
        result = generator.string("error", 5);
    }
    if (ok(result)) {
        result = generator.map_open();
    }
    if (ok(result)) {
        result = generator.string("type", 4);
    }
    if (ok(result)) {
        result = generator.string(error.type.data(), error.type.size());
    }
    if (ok(result)) {
        result = generator.string("message", 7);
    }
    if (ok(result)) {
        result = generator.string(error.message.data(), error.message.size());
    }
    if (ok(result)) {
        result = generator.map_close();
    }
    if (ok(result)) {
        result = generator.string("request_id", 10);
    }
    if (ok(result)) {
        result = generator.null_value();
    }
    if (ok(result)) {
        result = generator.map_close();
    }
    if (!ok(result)) {
        return std::unexpected(encode_error(result, output));
    }
    return output;
}

} // namespace

std::expected<mem::IoBuf, LlmErrorEncodeError> encode_llm_error(LlmWireProtocol protocol,
                                                                const LlmError &error) noexcept {
    return protocol == LlmWireProtocol::OpenAiChatCompletions ? encode_openai(error) : encode_anthropic(error);
}

} // namespace fiber::ai_server
