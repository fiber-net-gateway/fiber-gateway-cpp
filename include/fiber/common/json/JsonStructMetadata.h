#ifndef FIBER_JSONSTRUCTMETADATA_H
#define FIBER_JSONSTRUCTMETADATA_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace fiber::json {

enum class MissingFieldPolicy : std::uint8_t {
    Required,
    KeepDefault,
};

enum class UnknownFieldPolicy : std::uint8_t {
    Ignore,
    Reject,
};

enum class DuplicateFieldPolicy : std::uint8_t {
    KeepFirst,
    KeepLast,
    Reject,
};

struct StructDecodeOptions {
    UnknownFieldPolicy unknown_fields = UnknownFieldPolicy::Ignore;
    DuplicateFieldPolicy duplicate_fields = DuplicateFieldPolicy::KeepLast;
};

template<typename T>
struct StructMetadata;

template<typename T>
concept JsonStruct = requires { StructMetadata<std::remove_cvref_t<T>>::fields; };

template<typename T>
struct MemberPointerTraits;

template<typename Owner, typename Value>
struct MemberPointerTraits<Value Owner::*> {
    using owner_type = Owner;
    using value_type = Value;
};

struct MemberFieldTag {};
struct CustomFieldTag {};
struct ConstantFieldTag {};
struct IgnoredFieldTag {};
struct BaseFieldsTag {};

template<auto Member, MissingFieldPolicy Missing>
struct FieldDescriptor {
    using category = MemberFieldTag;
    using pointer_type = decltype(Member);
    using traits = MemberPointerTraits<pointer_type>;
    using owner_type = typename traits::owner_type;
    using value_type = typename traits::value_type;

    static constexpr auto member = Member;
    static constexpr MissingFieldPolicy missing_policy = Missing;

    std::string_view name;

    template<typename Object>
    [[nodiscard]] constexpr decltype(auto) get(Object &object) const noexcept {
        return object.*Member;
    }

    template<typename Object>
    [[nodiscard]] constexpr decltype(auto) get(const Object &object) const noexcept {
        return object.*Member;
    }
};

template<auto Member, auto Parser, MissingFieldPolicy Missing>
struct CustomFieldDescriptor {
    using category = CustomFieldTag;
    using pointer_type = decltype(Member);
    using traits = MemberPointerTraits<pointer_type>;
    using owner_type = typename traits::owner_type;
    using value_type = typename traits::value_type;

    static constexpr auto member = Member;
    static constexpr auto parser = Parser;
    static constexpr MissingFieldPolicy missing_policy = Missing;

    std::string_view name;

    template<typename Object>
    [[nodiscard]] constexpr decltype(auto) get(Object &object) const noexcept {
        return object.*Member;
    }

    template<typename Object>
    [[nodiscard]] constexpr decltype(auto) get(const Object &object) const noexcept {
        return object.*Member;
    }
};

template<typename Expected, MissingFieldPolicy Missing>
struct ConstantFieldDescriptor {
    using category = ConstantFieldTag;
    using value_type = Expected;

    static constexpr MissingFieldPolicy missing_policy = Missing;

    std::string_view name;
    Expected expected;
    const char *mismatch_message;
};

template<typename Value, MissingFieldPolicy Missing>
struct IgnoredFieldDescriptor {
    using category = IgnoredFieldTag;
    using value_type = Value;

    static constexpr MissingFieldPolicy missing_policy = Missing;

    std::string_view name;
};

template<typename Base>
struct BaseFieldsDescriptor {
    using category = BaseFieldsTag;
    using base_type = Base;
};

template<auto Member>
[[nodiscard]] consteval auto field(std::string_view name) {
    return FieldDescriptor<Member, MissingFieldPolicy::Required>{.name = name};
}

template<auto Member>
[[nodiscard]] consteval auto optional_field(std::string_view name) {
    return FieldDescriptor<Member, MissingFieldPolicy::KeepDefault>{.name = name};
}

template<auto Member, auto Parser>
[[nodiscard]] consteval auto custom_field(std::string_view name) {
    return CustomFieldDescriptor<Member, Parser, MissingFieldPolicy::Required>{.name = name};
}

template<auto Member, auto Parser>
[[nodiscard]] consteval auto optional_custom_field(std::string_view name) {
    return CustomFieldDescriptor<Member, Parser, MissingFieldPolicy::KeepDefault>{.name = name};
}

template<typename Expected>
[[nodiscard]] consteval auto optional_constant_field(std::string_view name, Expected expected,
                                                     const char *mismatch_message = "unexpected JSON field value") {
    return ConstantFieldDescriptor<std::remove_cvref_t<Expected>, MissingFieldPolicy::KeepDefault>{
            .name = name,
            .expected = expected,
            .mismatch_message = mismatch_message,
    };
}

template<typename Value>
[[nodiscard]] consteval auto optional_ignored_field(std::string_view name) {
    return IgnoredFieldDescriptor<Value, MissingFieldPolicy::KeepDefault>{.name = name};
}

template<typename Base>
[[nodiscard]] consteval auto base_fields() {
    return BaseFieldsDescriptor<Base>{};
}

namespace detail {

template<typename Struct, typename Item>
[[nodiscard]] consteval auto expand_struct_item(Item item) {
    if constexpr (std::is_same_v<typename Item::category, BaseFieldsTag>) {
        using Base = typename Item::base_type;
        static_assert(std::is_base_of_v<Base, Struct>, "JSON metadata base is not a base class of the struct");
        static_assert(JsonStruct<Base>, "JSON metadata base has no registered fields");
        return StructMetadata<Base>::fields;
    } else {
        if constexpr (std::is_same_v<typename Item::category, MemberFieldTag> ||
                      std::is_same_v<typename Item::category, CustomFieldTag>) {
            static_assert(std::is_base_of_v<typename Item::owner_type, Struct>,
                          "JSON field member does not belong to the struct or one of its bases");
        }
        return std::tuple{item};
    }
}

template<typename Tuple, std::size_t... I>
[[nodiscard]] consteval bool unique_field_names_impl(const Tuple &fields, std::index_sequence<I...>) {
    std::array<std::string_view, sizeof...(I)> names{std::get<I>(fields).name...};
    for (std::size_t left = 0; left < names.size(); ++left) {
        for (std::size_t right = left + 1; right < names.size(); ++right) {
            if (names[left] == names[right]) {
                return false;
            }
        }
    }
    return true;
}

} // namespace detail

template<typename Struct, typename... Items>
[[nodiscard]] consteval auto define_struct(Items... items) {
    return std::tuple_cat(detail::expand_struct_item<Struct>(items)...);
}

template<typename Tuple>
[[nodiscard]] consteval bool unique_field_names(const Tuple &fields) {
    constexpr std::size_t FieldCount = std::tuple_size_v<std::remove_cvref_t<Tuple>>;
    return detail::unique_field_names_impl(fields, std::make_index_sequence<FieldCount>{});
}

template<JsonStruct T>
inline constexpr std::size_t struct_field_count =
        std::tuple_size_v<std::remove_cvref_t<decltype(StructMetadata<std::remove_cvref_t<T>>::fields)>>;

template<JsonStruct T>
[[nodiscard]] constexpr StructDecodeOptions struct_decode_options() noexcept {
    using Value = std::remove_cvref_t<T>;
    if constexpr (requires { StructMetadata<Value>::options; }) {
        return StructMetadata<Value>::options;
    }
    return {};
}

template<JsonStruct T, typename Function, std::size_t... I>
constexpr void for_each_field_impl(Function &&function, std::index_sequence<I...>) noexcept {
    const auto &fields = StructMetadata<std::remove_cvref_t<T>>::fields;
    (function(std::integral_constant<std::size_t, I>{}, std::get<I>(fields)), ...);
}

template<JsonStruct T, typename Function>
constexpr void for_each_field(Function &&function) noexcept {
    for_each_field_impl<T>(std::forward<Function>(function), std::make_index_sequence<struct_field_count<T>>{});
}

template<JsonStruct T, typename Function, std::size_t... I>
[[nodiscard]] bool visit_field_by_name_impl(std::string_view name, Function &&function,
                                            std::index_sequence<I...>) noexcept {
    const auto &fields = StructMetadata<std::remove_cvref_t<T>>::fields;
    bool found = false;
    auto visit_one = [&]<std::size_t Index>() noexcept {
        const auto &field_descriptor = std::get<Index>(fields);
        if (!found && field_descriptor.name == name) {
            found = true;
            function(std::integral_constant<std::size_t, Index>{}, field_descriptor);
        }
    };
    (visit_one.template operator()<I>(), ...);
    return found;
}

template<JsonStruct T, typename Function>
[[nodiscard]] bool visit_field_by_name(std::string_view name, Function &&function) noexcept {
    return visit_field_by_name_impl<T>(name, std::forward<Function>(function),
                                       std::make_index_sequence<struct_field_count<T>>{});
}

} // namespace fiber::json

// FIBER_JSON_STRUCT must be used at global namespace scope. The field macros
// refer to the Self alias introduced by it.
#define FIBER_JSON_STRUCT(TYPE, ...)                                                                                   \
    template<>                                                                                                         \
    struct fiber::json::StructMetadata<TYPE> {                                                                         \
        using Self = TYPE;                                                                                             \
        static constexpr auto fields = ::fiber::json::define_struct<Self>(__VA_ARGS__);                                \
        static_assert(::fiber::json::unique_field_names(fields), "duplicate JSON field name");                         \
    }

#define FIBER_JSON_FIELD(MEMBER) ::fiber::json::field<&Self::MEMBER>(#MEMBER)
#define FIBER_JSON_NAMED_FIELD(MEMBER, NAME) ::fiber::json::field<&Self::MEMBER>(NAME)
#define FIBER_JSON_OPTIONAL_FIELD(MEMBER) ::fiber::json::optional_field<&Self::MEMBER>(#MEMBER)
#define FIBER_JSON_NAMED_OPTIONAL_FIELD(MEMBER, NAME) ::fiber::json::optional_field<&Self::MEMBER>(NAME)
#define FIBER_JSON_CUSTOM_FIELD(MEMBER, NAME, PARSER) ::fiber::json::custom_field<&Self::MEMBER, PARSER>(NAME)
#define FIBER_JSON_OPTIONAL_CUSTOM_FIELD(MEMBER, NAME, PARSER)                                                         \
    ::fiber::json::optional_custom_field<&Self::MEMBER, PARSER>(NAME)
#define FIBER_JSON_BASE(TYPE) ::fiber::json::base_fields<TYPE>()
#define FIBER_JSON_OPTIONAL_CONSTANT(NAME, VALUE, MESSAGE) ::fiber::json::optional_constant_field(NAME, VALUE, MESSAGE)
#define FIBER_JSON_OPTIONAL_IGNORED(TYPE, NAME) ::fiber::json::optional_ignored_field<TYPE>(NAME)

#endif // FIBER_JSONSTRUCTMETADATA_H
