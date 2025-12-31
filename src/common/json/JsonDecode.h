//
// Created by dear on 2025/12/30.
//

#ifndef FIBER_JSONDECODE_H
#define FIBER_JSONDECODE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "JsGc.h"

namespace fiber::json {

struct ParseError {
    std::string message;
    std::size_t offset = 0;
};

struct DecodedString {
    bool is_byte = true;
    std::vector<std::uint8_t> bytes;
    std::vector<char16_t> u16;

    void clear() {
        is_byte = true;
        bytes.clear();
        u16.clear();
    }

    [[nodiscard]] std::size_t size() const {
        return is_byte ? bytes.size() : u16.size();
    }
};

class Parser {
public:
    explicit Parser(GcHeap &heap);
    Parser(const Parser &) = delete;
    Parser &operator=(const Parser &) = delete;
    Parser(Parser &&) = delete;
    Parser &operator=(Parser &&) = delete;

    [[nodiscard]] bool parse(const char *data, std::size_t len, JsValue &out);
    [[nodiscard]] bool parse(const std::string &data, JsValue &out);
    [[nodiscard]] const ParseError &error() const;

private:
    GcHeap &heap_;
    ParseError error_;
};

class StreamParser {
public:
    enum class Status {
        Ok,
        NeedMore,
        Complete,
        Error,
    };

    explicit StreamParser(GcHeap &heap);
    StreamParser(const StreamParser &) = delete;
    StreamParser &operator=(const StreamParser &) = delete;
    StreamParser(StreamParser &&) = delete;
    StreamParser &operator=(StreamParser &&) = delete;

    void reset();
    [[nodiscard]] Status parse(const char *data, std::size_t len);
    [[nodiscard]] Status finish();
    [[nodiscard]] const ParseError &error() const;
    [[nodiscard]] const JsValue &root() const;
    [[nodiscard]] bool has_result() const;

private:
    enum class ParseState {
        Start,
        ParseComplete,
        ParseError,
        MapStart,
        MapNeedKey,
        MapSep,
        MapNeedVal,
        MapGotVal,
        ArrayStart,
        ArrayNeedVal,
        ArrayGotVal,
    };

    struct ContainerFrame {
        JsNodeType type = JsNodeType::Undefined;
        GcArray *array = nullptr;
        GcObject *object = nullptr;
        DecodedString key;
        bool has_key = false;
    };

    GcHeap &heap_;
    ParseError error_;
    JsValue root_;
    bool has_result_ = false;
    bool complete_ = false;
    std::string buffer_;
    std::size_t pos_ = 0;
    std::size_t total_offset_ = 0;
    std::vector<ParseState> state_stack_;
    std::vector<ContainerFrame> containers_;

    Status parse_internal(bool final);
    void compact_buffer();
    void clear_error();
    [[nodiscard]] bool set_error(const char *message, std::size_t offset);
};

} // namespace fiber::json

#endif // FIBER_JSONDECODE_H
