#include "AccessConfigCodec.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <fiber/common/json/JsonParse.h>
#include <fiber/common/json/JsonParser.h>
#include <fiber/common/json/JsonValue.h>
#include <fiber/common/mem/BufPool.h>

namespace fiber::access_server {
namespace {

using json::JsonArray;
using json::JsonObject;

template<typename T>
using DecodeResult = std::expected<T, AccessConfigError>;

enum class AccessJsonKind : std::uint8_t {
    Null,
    Bool,
    Integer,
    Double,
    BigNumber,
    Text,
    Array,
    Object,
};

class AccessJsonValue {
public:
    [[nodiscard]] AccessJsonKind kind() const noexcept { return kind_; }
    [[nodiscard]] bool is_null() const noexcept { return kind_ == AccessJsonKind::Null; }
    [[nodiscard]] bool is_bool() const noexcept { return kind_ == AccessJsonKind::Bool; }
    [[nodiscard]] bool is_integer() const noexcept { return kind_ == AccessJsonKind::Integer; }
    [[nodiscard]] bool is_double() const noexcept { return kind_ == AccessJsonKind::Double; }
    [[nodiscard]] bool is_big_number() const noexcept { return kind_ == AccessJsonKind::BigNumber; }
    [[nodiscard]] bool is_text() const noexcept { return kind_ == AccessJsonKind::Text; }
    [[nodiscard]] bool is_array() const noexcept { return kind_ == AccessJsonKind::Array; }
    [[nodiscard]] bool is_object() const noexcept { return kind_ == AccessJsonKind::Object; }

    [[nodiscard]] bool as_bool() const noexcept { return value_.boolean; }
    [[nodiscard]] std::int64_t as_integer() const noexcept { return value_.integer; }
    [[nodiscard]] double as_double() const noexcept { return value_.number.value; }
    [[nodiscard]] std::string_view number_text() const noexcept { return value_.number.text; }
    [[nodiscard]] std::string_view as_text() const noexcept { return value_.text; }
    [[nodiscard]] const JsonArray<AccessJsonValue> &as_array() const noexcept { return value_.array; }
    [[nodiscard]] const JsonObject<AccessJsonValue> &as_object() const noexcept { return value_.object; }

    void set_null() noexcept {
        value_.integer = 0;
        kind_ = AccessJsonKind::Null;
    }

    void set_bool(bool value) noexcept {
        value_.boolean = value;
        kind_ = AccessJsonKind::Bool;
    }

    void set_integer(std::int64_t value) noexcept {
        value_.integer = value;
        kind_ = AccessJsonKind::Integer;
    }

    void set_number(double value, std::string_view text) noexcept {
        std::construct_at(&value_.number, Number{.value = value, .text = text});
        kind_ = AccessJsonKind::Double;
    }

    void set_big_number(std::string_view text) noexcept {
        std::construct_at(&value_.number, Number{.value = 0, .text = text});
        kind_ = AccessJsonKind::BigNumber;
    }

    void set_text(std::string_view value) noexcept {
        std::construct_at(&value_.text, value);
        kind_ = AccessJsonKind::Text;
    }

    void set_array(JsonArray<AccessJsonValue> value) noexcept {
        std::construct_at(&value_.array, value);
        kind_ = AccessJsonKind::Array;
    }

    void set_object(JsonObject<AccessJsonValue> value) noexcept {
        std::construct_at(&value_.object, value);
        kind_ = AccessJsonKind::Object;
    }

private:
    struct Number {
        double value = 0;
        std::string_view text;
    };

    union Value {
        bool boolean;
        std::int64_t integer;
        Number number;
        std::string_view text;
        JsonArray<AccessJsonValue> array;
        JsonObject<AccessJsonValue> object;

        constexpr Value() noexcept : integer(0) {}
    };

    AccessJsonKind kind_ = AccessJsonKind::Null;
    Value value_;
};

static_assert(std::is_trivially_copyable_v<AccessJsonValue>);

json::ParseStatus parse_access_json_value(json::JsonParser &parser, mem::BufPool &pool, AccessJsonValue &out,
                                          std::string_view input) noexcept {
    const json::Token *token = parser.current_token();
    if (!token || token->role != json::TokenRole::Value) {
        (void) parser.fail("expected JSON value");
        return json::ParseStatus::Error;
    }

    AccessJsonValue result;
    switch (token->kind) {
        case json::TokenKind::Null:
            result.set_null();
            break;
        case json::TokenKind::Bool:
            result.set_bool(token->bval);
            break;
        case json::TokenKind::Integer:
            result.set_integer(token->inum);
            break;
        case json::TokenKind::Double: {
            const std::size_t offset = parser.current_offset();
            const std::size_t end = parser.current_end_offset();
            if (offset > end || end > input.size()) {
                (void) parser.fail("invalid JSON number range");
                return json::ParseStatus::Error;
            }
            result.set_number(token->fnum, input.substr(offset, end - offset));
            break;
        }
        case json::TokenKind::BigNumber:
            result.set_big_number(token->view);
            break;
        case json::TokenKind::Text: {
            std::string_view text;
            if (json::parse_text(parser, pool, text) != json::ParseStatus::Done) {
                return json::ParseStatus::Error;
            }
            result.set_text(text);
            break;
        }
        case json::TokenKind::StartArr: {
            JsonArray<AccessJsonValue> array;
            auto element_parser = [input](json::JsonParser &value_parser, mem::BufPool &value_pool,
                                          AccessJsonValue &value) noexcept {
                return parse_access_json_value(value_parser, value_pool, value, input);
            };
            if (json::parse_array(parser, pool, array, element_parser) != json::ParseStatus::Done) {
                return json::ParseStatus::Error;
            }
            result.set_array(array);
            break;
        }
        case json::TokenKind::StartObj: {
            JsonObject<AccessJsonValue> object;
            auto property_parser = [input](json::JsonParser &value_parser, mem::BufPool &value_pool,
                                           AccessJsonValue &value) noexcept {
                return parse_access_json_value(value_parser, value_pool, value, input);
            };
            if (json::parse_object(parser, pool, object, property_parser) != json::ParseStatus::Done) {
                return json::ParseStatus::Error;
            }
            result.set_object(object);
            break;
        }
        case json::TokenKind::EndObj:
        case json::TokenKind::EndArr:
            (void) parser.fail("expected JSON value");
            return json::ParseStatus::Error;
    }

    out = result;
    return json::ParseStatus::Done;
}

AccessConfigError make_error(AccessConfigErrorCode code, std::string field, std::string message,
                             std::size_t offset = 0) {
    return AccessConfigError{
            .code = code,
            .offset = offset,
            .field = std::move(field),
            .message = std::move(message),
    };
}

template<typename T>
DecodeResult<T> invalid_field(std::string_view field, std::string message) {
    return std::unexpected(make_error(AccessConfigErrorCode::InvalidField, std::string(field), std::move(message)));
}

template<typename T>
DecodeResult<T> out_of_range(std::string_view field, std::string message) {
    return std::unexpected(make_error(AccessConfigErrorCode::OutOfRange, std::string(field), std::move(message)));
}

std::string child_path(std::string_view parent, std::string_view child) {
    if (parent.empty()) {
        return std::string(child);
    }
    std::string path;
    path.reserve(parent.size() + 1 + child.size());
    path.append(parent);
    path.push_back('.');
    path.append(child);
    return path;
}

std::string index_path(std::string_view parent, std::size_t index) {
    std::array<char, 32> digits{};
    const auto conversion = std::to_chars(digits.data(), digits.data() + digits.size(), index);
    std::string path;
    path.reserve(parent.size() + 2 + static_cast<std::size_t>(conversion.ptr - digits.data()));
    path.append(parent);
    path.push_back('[');
    path.append(digits.data(), conversion.ptr);
    path.push_back(']');
    return path;
}

std::string_view trim_java(std::string_view value) noexcept {
    while (!value.empty() && static_cast<unsigned char>(value.front()) <= 0x20U) {
        value.remove_prefix(1);
    }
    while (!value.empty() && static_cast<unsigned char>(value.back()) <= 0x20U) {
        value.remove_suffix(1);
    }
    return value;
}

bool equals_ascii_case(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        unsigned char left = static_cast<unsigned char>(lhs[i]);
        unsigned char right = static_cast<unsigned char>(rhs[i]);
        if (left >= 'A' && left <= 'Z') {
            left = static_cast<unsigned char>(left + ('a' - 'A'));
        }
        if (right >= 'A' && right <= 'Z') {
            right = static_cast<unsigned char>(right + ('a' - 'A'));
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

template<typename T>
bool parse_signed_decimal(std::string_view value, T &out) noexcept {
    static_assert(std::is_signed_v<T>);
    if (value.empty()) {
        return false;
    }
    const char *begin = value.data();
    const char *end = begin + value.size();
    bool positive_sign = false;
    if (*begin == '+') {
        positive_sign = true;
        ++begin;
        if (begin == end) {
            return false;
        }
    }
    T result = 0;
    const auto conversion = std::from_chars(begin, end, result);
    if (conversion.ec != std::errc() || conversion.ptr != end) {
        return false;
    }
    if (positive_sign && result < 0) {
        return false;
    }
    out = result;
    return true;
}

template<typename T>
bool parse_unsigned_decimal(std::string_view value, T &out) noexcept {
    static_assert(std::is_unsigned_v<T>);
    if (value.empty()) {
        return false;
    }
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), out);
    return conversion.ec == std::errc() && conversion.ptr == value.data() + value.size();
}

DecodeResult<std::string> java_string(const AccessJsonValue &value, std::string_view field) {
    if (value.is_text()) {
        return std::string(value.as_text());
    }
    if (value.is_integer()) {
        std::array<char, 32> buffer{};
        const auto conversion = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value.as_integer());
        return std::string(buffer.data(), conversion.ptr);
    }
    if (value.is_bool()) {
        return std::string(value.as_bool() ? "true" : "false");
    }
    if (value.is_double()) {
        // Jackson returns the original numeric token when coercing a floating
        // point value to String, including exponent spelling and trailing .0.
        return std::string(value.number_text());
    }
    if (value.is_big_number()) {
        return std::string(value.number_text());
    }
    return invalid_field<std::string>(field, "expected a scalar string value or null");
}

DecodeResult<std::optional<std::string>> nullable_java_string(const AccessJsonValue &value, std::string_view field) {
    if (value.is_null()) {
        return std::optional<std::string>{};
    }
    auto decoded = java_string(value, field);
    if (!decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    return std::optional<std::string>(std::move(*decoded));
}

DecodeResult<std::int32_t> java_int32(const AccessJsonValue &value, std::string_view field) {
    if (value.is_null()) {
        return 0;
    }
    if (value.is_integer()) {
        const std::int64_t integer = value.as_integer();
        if (integer < std::numeric_limits<std::int32_t>::min() || integer > std::numeric_limits<std::int32_t>::max()) {
            return out_of_range<std::int32_t>(field, "integer exceeds Java int range");
        }
        return static_cast<std::int32_t>(integer);
    }
    if (value.is_text()) {
        std::int32_t integer = 0;
        if (!parse_signed_decimal(trim_java(value.as_text()), integer)) {
            return invalid_field<std::int32_t>(field, "expected Java int");
        }
        return integer;
    }
    if (value.is_double()) {
        const double number = value.as_double();
        if (number < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
            number > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
            return out_of_range<std::int32_t>(field, "number exceeds Java int range");
        }
        return static_cast<std::int32_t>(number);
    }
    return invalid_field<std::int32_t>(field, "expected Java int");
}

DecodeResult<std::optional<bool>> nullable_java_bool(const AccessJsonValue &value, std::string_view field) {
    if (value.is_null()) {
        return std::optional<bool>{};
    }
    if (value.is_bool()) {
        return std::optional<bool>(value.as_bool());
    }
    if (value.is_integer()) {
        return std::optional<bool>(value.as_integer() != 0);
    }
    if (value.is_text()) {
        const std::string_view text = trim_java(value.as_text());
        if (text.empty()) {
            return std::optional<bool>{};
        }
        if (equals_ascii_case(text, "true")) {
            return std::optional<bool>(true);
        }
        if (equals_ascii_case(text, "false")) {
            return std::optional<bool>(false);
        }
    }
    return invalid_field<std::optional<bool>>(field, "expected Java Boolean");
}

template<typename Enum>
using EnumLookup = std::initializer_list<std::pair<std::string_view, Enum>>;

template<typename Enum>
DecodeResult<std::optional<Enum>> java_enum(const AccessJsonValue &value, std::string_view field,
                                            EnumLookup<Enum> values) {
    if (value.is_null()) {
        return std::optional<Enum>{};
    }
    if (value.is_text()) {
        for (const auto &[name, candidate]: values) {
            if (value.as_text() == name) {
                return std::optional<Enum>(candidate);
            }
        }
        // READ_UNKNOWN_ENUM_VALUES_AS_NULL is enabled in the Java mapper.
        return std::optional<Enum>{};
    }
    if (value.is_integer()) {
        const std::int64_t ordinal = value.as_integer();
        if (ordinal < 0 || ordinal >= static_cast<std::int64_t>(values.size())) {
            return std::optional<Enum>{};
        }
        auto iterator = values.begin();
        std::advance(iterator, ordinal);
        return std::optional<Enum>(iterator->second);
    }
    return invalid_field<std::optional<Enum>>(field, "expected enum name, ordinal, or null");
}

DecodeResult<std::optional<std::int64_t>> duration_millis(const AccessJsonValue &value, std::string_view field) {
    if (value.is_null()) {
        return std::optional<std::int64_t>{};
    }
    if (value.is_integer()) {
        return std::optional<std::int64_t>(value.as_integer());
    }
    if (!value.is_text()) {
        return invalid_field<std::optional<std::int64_t>>(field, "unsupported duration token");
    }

    const std::string_view text = trim_java(value.as_text());
    if (text.empty()) {
        return std::optional<std::int64_t>{};
    }

    std::size_t digit_end = 0;
    while (digit_end < text.size() && text[digit_end] >= '0' && text[digit_end] <= '9') {
        ++digit_end;
    }
    if (digit_end == 0) {
        return invalid_field<std::optional<std::int64_t>>(field, "unsupported duration");
    }

    const std::string_view suffix = text.substr(digit_end);
    if (!suffix.empty() && !equals_ascii_case(suffix, "ms") && !equals_ascii_case(suffix, "s")) {
        return invalid_field<std::optional<std::int64_t>>(field, "unsupported duration");
    }

    std::uint32_t raw = 0;
    if (!parse_unsigned_decimal(text.substr(0, digit_end), raw) ||
        raw > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return out_of_range<std::optional<std::int64_t>>(field, "duration number exceeds Java int range");
    }

    std::uint32_t millis_bits = raw;
    if (equals_ascii_case(suffix, "s")) {
        // DurationDeserializer multiplies Java ints before widening, so the
        // two's-complement overflow is part of the accepted wire behavior.
        millis_bits *= 1000U;
    }
    const auto millis = std::bit_cast<std::int32_t>(millis_bits);
    return std::optional<std::int64_t>(millis);
}

DecodeResult<std::optional<std::int64_t>> data_size(const AccessJsonValue &value, std::string_view field) {
    if (value.is_null()) {
        return std::optional<std::int64_t>{};
    }
    if (value.is_integer()) {
        return std::optional<std::int64_t>(value.as_integer());
    }
    if (!value.is_text()) {
        return invalid_field<std::optional<std::int64_t>>(field, "unsupported data size token");
    }

    const std::string_view text = trim_java(value.as_text());
    std::size_t digit_end = 0;
    while (digit_end < text.size() && text[digit_end] >= '0' && text[digit_end] <= '9') {
        ++digit_end;
    }
    if (digit_end == 0 || text.size() - digit_end > 1) {
        return invalid_field<std::optional<std::int64_t>>(field, "unsupported data size");
    }

    unsigned int shift = 0;
    if (digit_end != text.size()) {
        switch (text[digit_end]) {
            case 'k':
            case 'K':
                shift = 10;
                break;
            case 'm':
            case 'M':
                shift = 20;
                break;
            case 'g':
            case 'G':
                shift = 30;
                break;
            default:
                return invalid_field<std::optional<std::int64_t>>(field, "unsupported data size");
        }
    }

    std::uint64_t raw = 0;
    if (!parse_unsigned_decimal(text.substr(0, digit_end), raw) ||
        raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return out_of_range<std::optional<std::int64_t>>(field, "data size number exceeds Java long range");
    }
    const std::uint64_t shifted = raw << shift;
    const std::int64_t result = std::bit_cast<std::int64_t>(shifted);
    if (result <= 0) {
        return invalid_field<std::optional<std::int64_t>>(field, "data size string must produce a positive value");
    }
    return std::optional<std::int64_t>(result);
}

void set_string_entry(StringConfigMap &entries, std::string name, std::optional<std::string> value) {
    for (StringConfigEntry &entry: entries) {
        if (entry.name == name) {
            entry.value = std::move(value);
            return;
        }
    }
    entries.push_back(StringConfigEntry{.name = std::move(name), .value = std::move(value)});
}

DecodeResult<StringConfigMap> string_map(const AccessJsonValue &value, std::string_view field) {
    if (value.is_null()) {
        return StringConfigMap{};
    }
    if (!value.is_object()) {
        return invalid_field<StringConfigMap>(field, "expected object or null");
    }

    StringConfigMap result;
    for (const auto &entry: value.as_object()) {
        const std::string path = child_path(field, entry.key);
        auto decoded = nullable_java_string(entry.value, path);
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        set_string_entry(result, std::string(entry.key), std::move(*decoded));
    }
    return result;
}

bool nullable_string_equal(const std::optional<std::string> &lhs, const std::optional<std::string> &rhs) {
    return lhs == rhs;
}

DecodeResult<NullableStringSet> string_set(const AccessJsonValue &value, std::string_view field) {
    if (value.is_null()) {
        return NullableStringSet{};
    }
    if (!value.is_array()) {
        return invalid_field<NullableStringSet>(field, "expected array or null");
    }

    NullableStringSet result;
    const JsonArray<AccessJsonValue> &array = value.as_array();
    result.reserve(array.size());
    for (std::size_t i = 0; i < array.size(); ++i) {
        auto decoded = nullable_java_string(array[i], index_path(field, i));
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        if (std::find_if(result.begin(), result.end(), [&](const auto &current) {
                return nullable_string_equal(current, *decoded);
            }) == result.end()) {
            result.push_back(std::move(*decoded));
        }
    }
    return result;
}

DecodeResult<std::optional<RouteBodyConfig>> route_body(const AccessJsonValue &value, std::string_view field) {
    if (value.is_null()) {
        return std::optional<RouteBodyConfig>{};
    }
    if (!value.is_object()) {
        return invalid_field<std::optional<RouteBodyConfig>>(field, "expected body object or null");
    }

    RouteBodyConfig result;
    for (const auto &entry: value.as_object()) {
        const std::string path = child_path(field, entry.key);
        if (entry.key == "type") {
            auto decoded = java_enum<BodyType>(
                    entry.value, path,
                    {{"TEXT", BodyType::Text}, {"BASE64", BodyType::Base64}, {"TEMPLATE", BodyType::Template}});
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.type = *decoded;
        } else if (entry.key == "content") {
            auto decoded = nullable_java_string(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.content = std::move(*decoded);
        }
    }
    return std::optional<RouteBodyConfig>(std::move(result));
}

DecodeResult<RouteConfig> route_config(const AccessJsonValue &value, std::string_view field) {
    if (!value.is_object()) {
        return invalid_field<RouteConfig>(field, "expected route object");
    }

    RouteConfig result;
    for (const auto &entry: value.as_object()) {
        const std::string path = child_path(field, entry.key);
        if (entry.key == "path") {
            auto decoded = nullable_java_string(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.path = std::move(*decoded);
        } else if (entry.key == "type") {
            auto decoded = java_enum<RouteType>(entry.value, path,
                                                {{"PROXY", RouteType::Proxy}, {"RESPONSE", RouteType::Response}});
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.type = *decoded;
        } else if (entry.key == "service") {
            auto decoded = nullable_java_string(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.service = std::move(*decoded);
        } else if (entry.key == "cluster") {
            auto decoded = nullable_java_string(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.cluster = std::move(*decoded);
        } else if (entry.key == "addresses") {
            auto decoded = string_set(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.addresses = std::move(*decoded);
        } else if (entry.key == "condition") {
            auto decoded = nullable_java_string(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.condition = std::move(*decoded);
        } else if (entry.key == "proxy_headers") {
            auto decoded = string_map(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.proxy_headers = std::move(*decoded);
        } else if (entry.key == "response_headers") {
            auto decoded = string_map(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.response_headers = std::move(*decoded);
        } else if (entry.key == "context") {
            auto decoded = string_map(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.context = std::move(*decoded);
        } else if (entry.key == "rewrite") {
            auto decoded = nullable_java_string(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.rewrite = std::move(*decoded);
        } else if (entry.key == "status") {
            auto decoded = java_int32(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.status = *decoded;
        } else if (entry.key == "body") {
            auto decoded = route_body(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.body = std::move(*decoded);
        } else if (entry.key == "timeout") {
            auto decoded = duration_millis(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.timeout_millis = *decoded;
        } else if (entry.key == "max_client_body_size") {
            auto decoded = data_size(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.max_client_body_size = *decoded;
        } else if (entry.key == "max_proxy_body_size") {
            auto decoded = data_size(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.max_proxy_body_size = *decoded;
        } else if (entry.key == "websocket_timeout") {
            auto decoded = duration_millis(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.websocket_timeout_millis = *decoded;
        } else if (entry.key == "flush") {
            auto decoded = nullable_java_bool(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.flush = *decoded;
        } else if (entry.key == "allows") {
            auto decoded = string_set(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.allows = std::move(*decoded);
        }
    }
    return result;
}

DecodeResult<std::optional<std::vector<std::optional<RouteConfig>>>> route_list(const AccessJsonValue &value,
                                                                                std::string_view field) {
    if (value.is_null()) {
        return std::optional<std::vector<std::optional<RouteConfig>>>{};
    }
    if (!value.is_array()) {
        return invalid_field<std::optional<std::vector<std::optional<RouteConfig>>>>(field, "expected array or null");
    }

    std::vector<std::optional<RouteConfig>> routes;
    const JsonArray<AccessJsonValue> &array = value.as_array();
    routes.reserve(array.size());
    for (std::size_t i = 0; i < array.size(); ++i) {
        if (array[i].is_null()) {
            routes.emplace_back();
            continue;
        }
        auto decoded = route_config(array[i], index_path(field, i));
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        routes.emplace_back(std::move(*decoded));
    }
    return std::optional<std::vector<std::optional<RouteConfig>>>(std::move(routes));
}

DecodeResult<std::uint8_t> net_mask(const AccessJsonValue &value, std::string_view field) {
    if (value.is_null()) {
        return invalid_field<std::uint8_t>(field, "Java HostStrategy.setNet rejects null");
    }
    auto decoded = java_string(value, field);
    if (!decoded) {
        return std::unexpected(std::move(decoded.error()));
    }

    std::uint8_t result = 0;
    std::string_view text = *decoded;
    std::size_t offset = 0;
    while (true) {
        const std::size_t separator = text.find(',', offset);
        const std::string_view item =
                separator == std::string_view::npos ? text.substr(offset) : text.substr(offset, separator - offset);
        if (item == "S_VDI") {
            result |= kNetVdi;
        } else if (item == "S_OFFICE") {
            result |= kNetOffice;
        } else if (item == "S_INTERNET") {
            result |= kNetInternet;
        } else if (item == "S_CUSTOM") {
            result |= kNetCustom;
        } else {
            return invalid_field<std::uint8_t>(field, "unknown HostStrategy net enum");
        }
        if (separator == std::string_view::npos) {
            return result;
        }
        offset = separator + 1;
    }
}

DecodeResult<HostStrategyConfig> host_strategy(const AccessJsonValue &value, std::string_view field) {
    if (!value.is_object()) {
        return invalid_field<HostStrategyConfig>(field, "expected HostStrategy object");
    }

    HostStrategyConfig result;
    for (const auto &entry: value.as_object()) {
        const std::string path = child_path(field, entry.key);
        if (entry.key == "https") {
            auto decoded = java_enum<HttpsStrategy>(entry.value, path,
                                                    {{"S_NOT_MUST", HttpsStrategy::NotRequired},
                                                     {"S_301", HttpsStrategy::Redirect301},
                                                     {"S_302", HttpsStrategy::Redirect302},
                                                     {"S_307", HttpsStrategy::Redirect307},
                                                     {"S_308", HttpsStrategy::Redirect308}});
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.https = *decoded;
        } else if (entry.key == "net") {
            auto decoded = net_mask(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.net_mask = *decoded;
        }
    }
    return result;
}

void set_host_entry(std::vector<HostConfigEntry> &entries, std::string pattern,
                    std::optional<HostStrategyConfig> strategy) {
    for (HostConfigEntry &entry: entries) {
        if (entry.pattern == pattern) {
            entry.strategy = std::move(strategy);
            return;
        }
    }
    entries.push_back(HostConfigEntry{.pattern = std::move(pattern), .strategy = std::move(strategy)});
}

DecodeResult<std::optional<std::vector<HostConfigEntry>>> host_map(const AccessJsonValue &value,
                                                                   std::string_view field) {
    if (value.is_null()) {
        return std::optional<std::vector<HostConfigEntry>>{};
    }
    if (!value.is_object()) {
        return invalid_field<std::optional<std::vector<HostConfigEntry>>>(field, "expected object or null");
    }

    std::vector<HostConfigEntry> hosts;
    for (const auto &entry: value.as_object()) {
        const std::string path = child_path(field, entry.key);
        std::optional<HostStrategyConfig> strategy;
        if (!entry.value.is_null()) {
            auto decoded = host_strategy(entry.value, path);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            strategy.emplace(std::move(*decoded));
        }
        set_host_entry(hosts, std::string(entry.key), std::move(strategy));
    }
    return std::optional<std::vector<HostConfigEntry>>(std::move(hosts));
}

DecodeResult<ProjectConfig> project_config(const JsonObject<AccessJsonValue> &object) {
    ProjectConfig result;
    for (const auto &entry: object) {
        if (entry.key == "version") {
            auto decoded = java_int32(entry.value, "version");
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.version = *decoded;
        } else if (entry.key == "host") {
            auto decoded = host_map(entry.value, "host");
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.hosts = std::move(*decoded);
        } else if (entry.key == "routes") {
            auto decoded = route_list(entry.value, "routes");
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            result.routes = std::move(*decoded);
        }
    }
    return result;
}

DecodeResult<GrayMatchConfig> gray_match_config(const JsonObject<AccessJsonValue> &object) {
    GrayMatchConfig result;
    for (const auto &entry: object) {
        const std::string entry_path(entry.key);
        if (entry.value.is_null() || !entry.value.is_object()) {
            return invalid_field<GrayMatchConfig>(entry_path, "expected gray-match object");
        }

        GrayMatchConfigEntry decoded_entry{
                .entry = std::string(entry.key),
        };
        for (const auto &property: entry.value.as_object()) {
            const std::string path = child_path(entry_path, property.key);
            if (property.key == "ratio") {
                auto decoded = java_int32(property.value, path);
                if (!decoded) {
                    return std::unexpected(std::move(decoded.error()));
                }
                decoded_entry.ratio = *decoded;
            } else if (property.key == "cidrs") {
                auto decoded = string_set(property.value, path);
                if (!decoded) {
                    return std::unexpected(std::move(decoded.error()));
                }
                decoded_entry.cidrs = std::move(*decoded);
            }
        }

        const auto existing = std::find_if(result.begin(), result.end(), [&](const GrayMatchConfigEntry &current) {
            return current.entry == decoded_entry.entry;
        });
        if (existing == result.end()) {
            result.push_back(std::move(decoded_entry));
        } else {
            *existing = std::move(decoded_entry);
        }
    }
    return result;
}

DecodeResult<AccessJsonValue> parse_json(std::string_view content, mem::BufPool &pool, json::JsonParser &parser) {
    if (!parser.feed(content.data(), content.size())) {
        const json::ParseError &error = parser.error();
        return std::unexpected(make_error(AccessConfigErrorCode::InvalidJson, {},
                                          error.message ? error.message : "invalid JSON", error.offset));
    }
    parser.finish();
    AccessJsonValue root;
    const auto status = json::parse_document(
            parser, pool, root,
            [content](json::JsonParser &value_parser, mem::BufPool &value_pool, AccessJsonValue &out) noexcept {
                return parse_access_json_value(value_parser, value_pool, out, content);
            });
    if (status != json::ParseStatus::Done) {
        const json::ParseError &error = parser.error();
        return std::unexpected(make_error(AccessConfigErrorCode::InvalidJson, {},
                                          error.message ? error.message : "invalid JSON", error.offset));
    }
    return root;
}

} // namespace

ProjectConfigResult parse_project_config(std::string_view content) {
    if (content.empty()) {
        return std::optional<ProjectConfig>{};
    }

    mem::BufPool pool;
    json::JsonParser parser;
    auto root = parse_json(content, pool, parser);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    if (root->is_null()) {
        return std::optional<ProjectConfig>{};
    }
    if (!root->is_object()) {
        return std::unexpected(
                make_error(AccessConfigErrorCode::InvalidRoot, {}, "project configuration must be an object or null"));
    }

    auto decoded = project_config(root->as_object());
    if (!decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    return std::optional<ProjectConfig>(std::move(*decoded));
}

GrayMatchConfigResult parse_gray_match_config(std::string_view content) {
    if (content.empty()) {
        return std::optional<GrayMatchConfig>{};
    }

    mem::BufPool pool;
    json::JsonParser parser;
    auto root = parse_json(content, pool, parser);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    if (root->is_null()) {
        return std::optional<GrayMatchConfig>(GrayMatchConfig{});
    }
    if (!root->is_object()) {
        return std::unexpected(make_error(AccessConfigErrorCode::InvalidRoot, {},
                                          "gray-match configuration must be an object or null"));
    }
    auto decoded = gray_match_config(root->as_object());
    if (!decoded) {
        return std::unexpected(std::move(decoded.error()));
    }
    return std::optional<GrayMatchConfig>(std::move(*decoded));
}

std::vector<std::string> parse_project_list(std::string_view content) {
    if (content.empty()) {
        return {};
    }

    content = trim_java(content);
    if (content.empty()) {
        // Java "".split(";") returns one empty element.
        return {std::string()};
    }

    std::vector<std::string> projects;
    std::size_t offset = 0;
    while (true) {
        const std::size_t separator = content.find(';', offset);
        if (separator == std::string_view::npos) {
            projects.emplace_back(content.substr(offset));
            break;
        }
        projects.emplace_back(content.substr(offset, separator - offset));
        offset = separator + 1;
    }
    while (!projects.empty() && projects.back().empty()) {
        projects.pop_back();
    }
    return projects;
}

} // namespace fiber::access_server
