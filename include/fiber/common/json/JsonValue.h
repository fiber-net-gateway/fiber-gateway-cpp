#ifndef FIBER_JSONVALUE_H
#define FIBER_JSONVALUE_H

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace fiber::json {

// Absent means an object field was not encountered. JSON values parsed through
// parse_nullable() transition to either Null or Present.
enum class NullableState : std::uint8_t {
    Absent,
    Null,
    Present,
};

template<typename T>
class Nullable {
public:
    constexpr Nullable() noexcept(std::is_nothrow_default_constructible_v<T>) = default;

    [[nodiscard]] constexpr NullableState state() const noexcept { return state_; }
    [[nodiscard]] constexpr bool is_absent() const noexcept { return state_ == NullableState::Absent; }
    [[nodiscard]] constexpr bool is_null() const noexcept { return state_ == NullableState::Null; }
    [[nodiscard]] constexpr bool is_present() const noexcept { return state_ == NullableState::Present; }
    [[nodiscard]] constexpr bool has_value() const noexcept { return is_present(); }

    [[nodiscard]] constexpr T &value() noexcept {
        assert(is_present());
        return value_;
    }

    [[nodiscard]] constexpr const T &value() const noexcept {
        assert(is_present());
        return value_;
    }

    [[nodiscard]] constexpr T &operator*() noexcept { return value(); }
    [[nodiscard]] constexpr const T &operator*() const noexcept { return value(); }
    [[nodiscard]] constexpr T *operator->() noexcept { return &value(); }
    [[nodiscard]] constexpr const T *operator->() const noexcept { return &value(); }

    constexpr void set_absent() noexcept { state_ = NullableState::Absent; }
    constexpr void set_null() noexcept { state_ = NullableState::Null; }

    constexpr void set_present(const T &value) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        value_ = value;
        state_ = NullableState::Present;
    }

    constexpr void set_present(T &&value) noexcept(std::is_nothrow_move_assignable_v<T>) {
        value_ = std::move(value);
        state_ = NullableState::Present;
    }

    [[nodiscard]] std::optional<T> to_optional() const {
        // std::optional cannot distinguish Absent from an explicit JSON null.
        if (!is_present()) {
            return std::nullopt;
        }
        return value_;
    }

private:
    NullableState state_ = NullableState::Absent;
    T value_{};
};

template<typename T>
class JsonArray {
public:
    // The pointed-to elements are owned by the BufPool used during parsing.
    constexpr JsonArray() noexcept = default;
    constexpr JsonArray(T *data, std::size_t size) noexcept : data_(data), size_(size) {}

    [[nodiscard]] constexpr T *data() noexcept { return data_; }
    [[nodiscard]] constexpr const T *data() const noexcept { return data_; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] constexpr T &operator[](std::size_t index) noexcept { return data_[index]; }
    [[nodiscard]] constexpr const T &operator[](std::size_t index) const noexcept { return data_[index]; }

    [[nodiscard]] constexpr T *begin() noexcept { return data_; }
    [[nodiscard]] constexpr const T *begin() const noexcept { return data_; }
    [[nodiscard]] constexpr T *end() noexcept { return size_ == 0 ? data_ : data_ + size_; }
    [[nodiscard]] constexpr const T *end() const noexcept { return size_ == 0 ? data_ : data_ + size_; }

private:
    T *data_ = nullptr;
    std::size_t size_ = 0;
};

template<typename P>
class JsonObject {
public:
    struct Entry {
        std::string_view key;
        P value{};
    };

    // Entries preserve input order and duplicate keys. Their storage and key
    // text are owned by the BufPool used during parsing.
    constexpr JsonObject() noexcept = default;
    constexpr JsonObject(Entry *entries, std::size_t size) noexcept : entries_(entries), size_(size) {}

    [[nodiscard]] constexpr Entry *data() noexcept { return entries_; }
    [[nodiscard]] constexpr const Entry *data() const noexcept { return entries_; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] constexpr Entry &operator[](std::size_t index) noexcept { return entries_[index]; }
    [[nodiscard]] constexpr const Entry &operator[](std::size_t index) const noexcept { return entries_[index]; }

    [[nodiscard]] constexpr Entry *begin() noexcept { return entries_; }
    [[nodiscard]] constexpr const Entry *begin() const noexcept { return entries_; }
    [[nodiscard]] constexpr Entry *end() noexcept { return size_ == 0 ? entries_ : entries_ + size_; }
    [[nodiscard]] constexpr const Entry *end() const noexcept { return size_ == 0 ? entries_ : entries_ + size_; }

    [[nodiscard]] Entry *find_first(std::string_view key) noexcept {
        for (Entry &entry: *this) {
            if (entry.key == key) {
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const Entry *find_first(std::string_view key) const noexcept {
        for (const Entry &entry: *this) {
            if (entry.key == key) {
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] Entry *find_last(std::string_view key) noexcept {
        for (std::size_t i = size_; i > 0; --i) {
            if (entries_[i - 1].key == key) {
                return &entries_[i - 1];
            }
        }
        return nullptr;
    }

    [[nodiscard]] const Entry *find_last(std::string_view key) const noexcept {
        for (std::size_t i = size_; i > 0; --i) {
            if (entries_[i - 1].key == key) {
                return &entries_[i - 1];
            }
        }
        return nullptr;
    }

    // The convenience lookup uses last-wins semantics for duplicate keys.
    [[nodiscard]] Entry *find(std::string_view key) noexcept { return find_last(key); }
    [[nodiscard]] const Entry *find(std::string_view key) const noexcept { return find_last(key); }

private:
    Entry *entries_ = nullptr;
    std::size_t size_ = 0;
};

enum class JsonAnyKind : std::uint8_t {
    Null,
    Bool,
    Integer,
    Double,
    Text,
    Array,
    Object,
};

class JsonAny {
public:
    constexpr JsonAny() noexcept = default;

    [[nodiscard]] constexpr JsonAnyKind kind() const noexcept { return kind_; }
    [[nodiscard]] constexpr bool is_null() const noexcept { return kind_ == JsonAnyKind::Null; }
    [[nodiscard]] constexpr bool is_bool() const noexcept { return kind_ == JsonAnyKind::Bool; }
    [[nodiscard]] constexpr bool is_integer() const noexcept { return kind_ == JsonAnyKind::Integer; }
    [[nodiscard]] constexpr bool is_double() const noexcept { return kind_ == JsonAnyKind::Double; }
    [[nodiscard]] constexpr bool is_number() const noexcept { return is_integer() || is_double(); }
    [[nodiscard]] constexpr bool is_text() const noexcept { return kind_ == JsonAnyKind::Text; }
    [[nodiscard]] constexpr bool is_array() const noexcept { return kind_ == JsonAnyKind::Array; }
    [[nodiscard]] constexpr bool is_object() const noexcept { return kind_ == JsonAnyKind::Object; }

    [[nodiscard]] constexpr bool as_bool() const noexcept {
        assert(is_bool());
        return value_.boolean;
    }

    [[nodiscard]] constexpr std::int64_t as_integer() const noexcept {
        assert(is_integer());
        return value_.integer;
    }

    [[nodiscard]] constexpr double as_double() const noexcept {
        assert(is_double());
        return value_.number;
    }

    [[nodiscard]] constexpr std::string_view as_text() const noexcept {
        assert(is_text());
        return value_.text;
    }

    [[nodiscard]] constexpr JsonArray<JsonAny> &as_array() noexcept {
        assert(is_array());
        return value_.array;
    }

    [[nodiscard]] constexpr const JsonArray<JsonAny> &as_array() const noexcept {
        assert(is_array());
        return value_.array;
    }

    [[nodiscard]] constexpr JsonObject<JsonAny> &as_object() noexcept {
        assert(is_object());
        return value_.object;
    }

    [[nodiscard]] constexpr const JsonObject<JsonAny> &as_object() const noexcept {
        assert(is_object());
        return value_.object;
    }

    constexpr void set_null() noexcept {
        value_.integer = 0;
        kind_ = JsonAnyKind::Null;
    }

    constexpr void set_bool(bool value) noexcept {
        value_.boolean = value;
        kind_ = JsonAnyKind::Bool;
    }

    constexpr void set_integer(std::int64_t value) noexcept {
        value_.integer = value;
        kind_ = JsonAnyKind::Integer;
    }

    constexpr void set_double(double value) noexcept {
        value_.number = value;
        kind_ = JsonAnyKind::Double;
    }

    constexpr void set_text(std::string_view value) noexcept {
        std::construct_at(&value_.text, value);
        kind_ = JsonAnyKind::Text;
    }

    constexpr void set_array(JsonArray<JsonAny> value) noexcept {
        std::construct_at(&value_.array, value);
        kind_ = JsonAnyKind::Array;
    }

    constexpr void set_object(JsonObject<JsonAny> value) noexcept {
        std::construct_at(&value_.object, value);
        kind_ = JsonAnyKind::Object;
    }

private:
    union Value {
        bool boolean;
        std::int64_t integer;
        double number;
        std::string_view text;
        JsonArray<JsonAny> array;
        JsonObject<JsonAny> object;

        constexpr Value() noexcept : integer(0) {}
    };

    JsonAnyKind kind_ = JsonAnyKind::Null;
    Value value_;
};

static_assert(std::is_trivially_copyable_v<JsonAny>);

} // namespace fiber::json

#endif // FIBER_JSONVALUE_H
