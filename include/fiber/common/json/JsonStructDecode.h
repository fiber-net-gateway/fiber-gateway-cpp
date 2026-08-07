#ifndef FIBER_JSONSTRUCTDECODE_H
#define FIBER_JSONSTRUCTDECODE_H

#include <bitset>
#include <concepts>
#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>
#include <type_traits>

#include "JsonParse.h"
#include "JsonStructMetadata.h"

namespace fiber::json {

template<typename T>
struct CustomValueDecoder;

template<typename T>
[[nodiscard]] ParseStatus parse_value(JsonParser &parser, mem::BufPool &pool, T &out) noexcept;

namespace detail {

template<typename>
inline constexpr bool dependent_false = false;

template<typename T>
struct NullableTraits : std::false_type {};

template<typename T>
struct NullableTraits<Nullable<T>> : std::true_type {
    using value_type = T;
};

template<typename T>
struct OptionalTraits : std::false_type {};

template<typename T>
struct OptionalTraits<std::optional<T>> : std::true_type {
    using value_type = T;
};

template<typename T>
struct ArrayTraits : std::false_type {};

template<typename T>
struct ArrayTraits<JsonArray<T>> : std::true_type {
    using value_type = T;
};

template<typename T>
struct ObjectTraits : std::false_type {};

template<typename T>
struct ObjectTraits<JsonObject<T>> : std::true_type {
    using value_type = T;
};

template<typename T>
concept HasCustomValueDecoder = requires(JsonParser &parser, mem::BufPool &pool, T &out) {
    { CustomValueDecoder<T>::parse(parser, pool, out) } noexcept -> std::same_as<ParseStatus>;
};

template<typename Descriptor, typename Struct>
[[nodiscard]] ParseStatus parse_field_value(const Descriptor &field, JsonParser &parser, mem::BufPool &pool,
                                            Struct &out) noexcept {
    if constexpr (std::same_as<typename Descriptor::category, MemberFieldTag>) {
        return parse_value(parser, pool, field.get(out));
    } else if constexpr (std::same_as<typename Descriptor::category, CustomFieldTag>) {
        using Value = typename Descriptor::value_type;
        static_assert(std::is_nothrow_invocable_r_v<ParseStatus, decltype(Descriptor::parser), JsonParser &,
                                                    mem::BufPool &, Value &>,
                      "custom JSON field parser must be noexcept and return ParseStatus");
        return std::invoke(Descriptor::parser, parser, pool, field.get(out));
    } else if constexpr (std::same_as<typename Descriptor::category, ConstantFieldTag>) {
        using Value = typename Descriptor::value_type;
        static_assert(std::is_nothrow_default_constructible_v<Value>,
                      "constant JSON field type must be nothrow default constructible");
        static_assert(noexcept(std::declval<const Value &>() == std::declval<const Value &>()),
                      "constant JSON field comparison must be noexcept");

        Value value{};
        if (parse_value(parser, pool, value) != ParseStatus::Done) {
            return ParseStatus::Error;
        }
        if (!(value == field.expected)) {
            return fail(parser, field.mismatch_message);
        }
        return ParseStatus::Done;
    } else {
        static_assert(std::same_as<typename Descriptor::category, IgnoredFieldTag>);
        using Value = typename Descriptor::value_type;
        static_assert(std::is_nothrow_default_constructible_v<Value>,
                      "ignored JSON field type must be nothrow default constructible");
        Value ignored{};
        return parse_value(parser, pool, ignored);
    }
}

template<typename Descriptor, typename Struct>
[[nodiscard]] ParseStatus finish_missing_field(const Descriptor &field, JsonParser &parser, Struct &out) noexcept {
    if constexpr (Descriptor::missing_policy == MissingFieldPolicy::Required) {
        return fail(parser, "missing required JSON field");
    } else if constexpr (std::same_as<typename Descriptor::category, MemberFieldTag> ||
                         std::same_as<typename Descriptor::category, CustomFieldTag>) {
        using Value = std::remove_cvref_t<decltype(field.get(out))>;
        if constexpr (NullableTraits<Value>::value) {
            field.get(out).set_absent();
        }
    }
    return ParseStatus::Done;
}

template<JsonStruct T>
[[nodiscard]] ParseStatus parse_struct(JsonParser &parser, mem::BufPool &pool, T &out) noexcept {
    constexpr StructDecodeOptions Options = struct_decode_options<T>();
    std::bitset<struct_field_count<T>> present;

    auto field_parser = [&present](std::string_view name, JsonParser &field_value_parser, mem::BufPool &field_pool,
                                   T &value) noexcept {
        ObjectFieldStatus field_status = ObjectFieldStatus::Unknown;
        const bool found = visit_field_by_name<T>(name, [&](auto index, const auto &field) noexcept {
            const std::size_t field_index = decltype(index)::value;
            if (present[field_index]) {
                if constexpr (Options.duplicate_fields == DuplicateFieldPolicy::Reject) {
                    (void) field_value_parser.fail("duplicate JSON field");
                    field_status = ObjectFieldStatus::Error;
                    return;
                }
                if constexpr (Options.duplicate_fields == DuplicateFieldPolicy::KeepFirst) {
                    field_status = ObjectFieldStatus::Unknown;
                    return;
                }
            } else {
                present[field_index] = true;
            }

            field_status = to_object_field_status(parse_field_value(field, field_value_parser, field_pool, value));
        });

        if (!found && Options.unknown_fields == UnknownFieldPolicy::Reject) {
            (void) field_value_parser.fail("unknown JSON field");
            return ObjectFieldStatus::Error;
        }
        return field_status;
    };

    auto finalizer = [&present](JsonParser &object_parser, mem::BufPool & /*field_pool*/, T &value) noexcept {
        ParseStatus status = ParseStatus::Done;
        for_each_field<T>([&](auto index, const auto &field) noexcept {
            if (status == ParseStatus::Done && !present[decltype(index)::value]) {
                status = finish_missing_field(field, object_parser, value);
            }
        });
        return status;
    };

    return parse_object_fields(parser, pool, out, field_parser, finalizer);
}

} // namespace detail

template<typename T>
[[nodiscard]] ParseStatus parse_value(JsonParser &parser, mem::BufPool &pool, T &out) noexcept {
    using Value = std::remove_cvref_t<T>;

    if constexpr (detail::HasCustomValueDecoder<Value>) {
        return CustomValueDecoder<Value>::parse(parser, pool, out);
    } else if constexpr (std::same_as<Value, std::nullptr_t>) {
        return parse_null(parser, pool, out);
    } else if constexpr (std::same_as<Value, bool>) {
        return parse_bool(parser, pool, out);
    } else if constexpr (std::integral<Value>) {
        return parse_integral<Value>(parser, pool, out);
    } else if constexpr (std::same_as<Value, double>) {
        return parse_double(parser, pool, out);
    } else if constexpr (std::same_as<Value, std::string_view>) {
        return parse_text(parser, pool, out);
    } else if constexpr (std::same_as<Value, JsonAny>) {
        return parse_any(parser, pool, out);
    } else if constexpr (detail::NullableTraits<Value>::value) {
        using Element = typename detail::NullableTraits<Value>::value_type;
        auto value_parser = [](JsonParser &value_parser, mem::BufPool &value_pool, Element &value) noexcept {
            return parse_value(value_parser, value_pool, value);
        };
        return parse_nullable(parser, pool, out, value_parser);
    } else if constexpr (detail::OptionalTraits<Value>::value) {
        using Element = typename detail::OptionalTraits<Value>::value_type;
        auto value_parser = [](JsonParser &value_parser, mem::BufPool &value_pool, Element &value) noexcept {
            return parse_value(value_parser, value_pool, value);
        };
        return parse_optional(parser, pool, out, value_parser);
    } else if constexpr (detail::ArrayTraits<Value>::value) {
        using Element = typename detail::ArrayTraits<Value>::value_type;
        auto element_parser = [](JsonParser &value_parser, mem::BufPool &value_pool, Element &value) noexcept {
            return parse_value(value_parser, value_pool, value);
        };
        return parse_array(parser, pool, out, element_parser);
    } else if constexpr (detail::ObjectTraits<Value>::value) {
        using Element = typename detail::ObjectTraits<Value>::value_type;
        auto property_parser = [](JsonParser &value_parser, mem::BufPool &value_pool, Element &value) noexcept {
            return parse_value(value_parser, value_pool, value);
        };
        return parse_object(parser, pool, out, property_parser);
    } else if constexpr (JsonStruct<Value>) {
        return detail::parse_struct(parser, pool, out);
    } else {
        static_assert(detail::dependent_false<Value>, "no JSON value decoder registered for this type");
    }
}

} // namespace fiber::json

#endif // FIBER_JSONSTRUCTDECODE_H
