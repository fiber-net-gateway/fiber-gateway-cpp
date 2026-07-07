//
// Created by dear on 2025/12/30.
//

#ifndef FIBER_JSONDECODE_H
#define FIBER_JSONDECODE_H

#include <cstddef>
#include <cstdint>

namespace fiber::json {

struct ParseError {
    const char *message = nullptr;
    std::size_t offset = 0;
};

enum class DecodeStatus {
    Ok,
    NeedMore,
    Complete,
    Error,
    Canceled,
};

struct DecodeCallbacks {
    void *ctx = nullptr;

    int (*on_null)(void *ctx) noexcept = nullptr;
    int (*on_bool)(void *ctx, bool value) noexcept = nullptr;

    int (*on_number)(void *ctx, const char *data, std::size_t len) noexcept = nullptr;
    int (*on_integer)(void *ctx, std::int64_t value) noexcept = nullptr;
    int (*on_double)(void *ctx, double value) noexcept = nullptr;

    int (*on_string)(void *ctx, const char *data, std::size_t len) noexcept = nullptr;
    int (*on_object_key)(void *ctx, const char *data, std::size_t len) noexcept = nullptr;

    int (*on_object_start)(void *ctx) noexcept = nullptr;
    int (*on_object_end)(void *ctx) noexcept = nullptr;
    int (*on_array_start)(void *ctx) noexcept = nullptr;
    int (*on_array_end)(void *ctx) noexcept = nullptr;
};

class Decoder {
public:
    explicit Decoder(const DecodeCallbacks &callbacks) noexcept;
    ~Decoder() noexcept;

    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;
    Decoder(Decoder &&) = delete;
    Decoder &operator=(Decoder &&) = delete;

    void reset() noexcept;
    [[nodiscard]] DecodeStatus parse(const char *data, std::size_t len) noexcept;
    [[nodiscard]] DecodeStatus finish() noexcept;

    [[nodiscard]] const ParseError &error() const noexcept;
    [[nodiscard]] std::size_t bytes_consumed() const noexcept;

private:
    struct Impl;

    Impl *impl_ = nullptr;
    ParseError init_error_;
};

[[nodiscard]] DecodeStatus decode(const char *data, std::size_t len, const DecodeCallbacks &callbacks,
                                  ParseError *error = nullptr) noexcept;

} // namespace fiber::json

#endif // FIBER_JSONDECODE_H
