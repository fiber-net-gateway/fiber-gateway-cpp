# C++ JSON → Struct 的类反射模板方案

## 1. 背景

C++ 当前缺少稳定、通用的静态反射能力，因此无法直接在编译期枚举一个结构体的成员名称、成员类型以及成员地址。

在已经具备 JSON Parser 和 SAX 事件接口的前提下，JSON 解码到 C++ struct 的主要问题不再是 JSON 语法解析，而是：

1. 如何声明一个结构体有哪些可序列化字段；
2. 如何建立 JSON 字段名到 C++ 成员的映射；
3. 如何在编译期获得成员类型，并选择对应的 decoder；
4. 如何统一处理嵌套结构、容器、`std::optional`、枚举等类型；
5. 如何尽量减少每个结构体需要手写的解码代码。

本文设计一种基于以下 C++ 能力的“类反射”机制：

- 成员指针；
- 模板特化；
- `std::tuple`；
- `if constexpr` 和 concepts；
- 少量预处理宏。

该机制不依赖真正的语言级 reflection，也不关心底层使用 DOM 还是 SAX。字段元数据可以同时被 SAX decoder、DOM decoder、JSON encoder、Schema 生成器等组件复用。

---

## 2. 设计目标

期望业务侧的使用形式如下：

```cpp
struct Address {
    std::string city;
    std::string street;
};

JSON_STRUCT(
    Address,
    JSON_FIELD(city),
    JSON_FIELD(street)
);

struct User {
    std::uint64_t id;
    std::string name;
    std::optional<std::uint32_t> age;
    Address address;
    std::vector<std::string> tags;
};

JSON_STRUCT(
    User,
    JSON_FIELD(id),
    JSON_FIELD(name),
    JSON_OPTIONAL_FIELD(age),
    JSON_FIELD(address),
    JSON_DEFAULT_FIELD(tags)
);
```

解码接口保持简单：

```cpp
auto result = json::decode<User>(input);

if (!result) {
    handle_error(result.error());
}

User user = std::move(*result);
```

对于 SAX 场景，也可以直接驱动一个目标对象：

```cpp
User user;
json::StructSaxDecoder<User> decoder(user);

parser.parse(input, decoder);
```

模板元数据层应满足：

- 字段访问类型安全；
- 字段类型在编译期确定；
- 不依赖成员偏移量；
- 不进行 `void*` 强制转换；
- 支持嵌套 struct；
- 支持自定义类型 decoder；
- 字段注册尽量简洁；
- 元数据与具体 JSON parser 解耦。

---

## 3. 总体结构

整个系统可以分为四层：

```text
JSON Parser / SAX Parser
        ↓
SAX Event Adapter
        ↓
类型 Decoder
        ↓
Struct Metadata
```

本文主要描述最下面两层：

```text
类型 Decoder
        ↓
Struct Metadata
```

其中：

- `Struct Metadata` 描述一个结构体有哪些字段；
- `Type Decoder` 根据字段类型选择合适的解码器；
- SAX 层根据当前 JSON key 找到字段描述，并将后续事件转发给该字段的类型 decoder。

---

## 4. 核心：使用成员指针描述字段

C++ 虽然不能枚举结构体成员，但可以显式取得成员指针：

```cpp
&User::id
&User::name
&User::address
```

成员指针保留了两个重要信息：

1. 它属于哪个结构体；
2. 它指向的成员是什么类型。

例如：

```cpp
auto member = &User::id;

User user;
user.*member = 100;
```

因此，一个 JSON 字段描述只需要保存：

- JSON 字段名；
- C++ 成员指针；
- 是否必填；
- 缺失时的处理策略；
- 可选的自定义 decoder。

最基础的字段描述可以定义为：

```cpp
namespace json {

template <auto MemberPtr>
struct FieldDescriptor {
    static constexpr auto member = MemberPtr;

    std::string_view name;
    bool required;
};

template <auto MemberPtr>
constexpr auto field(std::string_view name) {
    return FieldDescriptor<MemberPtr>{
        .name = name,
        .required = true,
    };
}

} // namespace json
```

注册字段：

```cpp
json::field<&User::id>("id")
```

访问字段：

```cpp
User user;

using Field = decltype(json::field<&User::id>("id"));

user.*Field::member = 100;
```

成员指针本身就是编译期常量，因此 decoder 可以在编译期推导出字段类型。

---

## 5. 从成员指针提取类型

为了实现通用 decoder，需要从：

```cpp
std::uint64_t User::*
```

中提取：

- owner type：`User`
- value type：`std::uint64_t`

可以定义成员指针 traits：

```cpp
template <class T>
struct MemberPointerTraits;

template <class Class, class Member>
struct MemberPointerTraits<Member Class::*> {
    using class_type = Class;
    using member_type = Member;
};
```

对于字段描述：

```cpp
template <auto MemberPtr>
struct FieldDescriptor {
    using pointer_type = decltype(MemberPtr);
    using traits = MemberPointerTraits<pointer_type>;

    using owner_type = typename traits::class_type;
    using value_type = typename traits::member_type;

    static constexpr auto member = MemberPtr;

    std::string_view name;
    bool required;
};
```

此后可直接获得字段值类型：

```cpp
using FieldType = FieldDescriptor<&User::id>::value_type;
// std::uint64_t
```

这一步是整个模板方案的核心。类型 decoder 不需要运行时类型信息，也不需要使用 RTTI。

---

## 6. 为结构体提供元数据

定义一个默认未实现的 traits：

```cpp
namespace json {

template <class T>
struct StructMetadata;

}
```

每个支持 JSON 映射的结构体，对该 traits 进行特化：

```cpp
template <>
struct json::StructMetadata<User> {
    using Self = User;

    static constexpr auto fields = std::tuple{
        json::field<&Self::id>("id"),
        json::field<&Self::name>("name"),
        json::field<&Self::age>("age"),
        json::field<&Self::address>("address"),
        json::field<&Self::tags>("tags"),
    };
};
```

判断一个类型是否注册了元数据：

```cpp
template <class T>
concept JsonStruct = requires {
    json::StructMetadata<T>::fields;
};
```

通用 decoder 可以通过 `JsonStruct<T>` 判断一个类型是否应当按 object 处理。

---

## 7. 使用宏减少注册代码

直接编写模板特化是可靠的，但使用体验略显冗长。可以用宏生成固定样板代码。

```cpp
#define JSON_STRUCT(TYPE, ...)                         \
    template <>                                        \
    struct json::StructMetadata<TYPE> {                \
        using Self = TYPE;                             \
        static constexpr auto fields =                 \
            std::tuple{__VA_ARGS__};                   \
    }

#define JSON_FIELD(MEMBER)                             \
    json::field<&Self::MEMBER>(#MEMBER)
```

业务侧：

```cpp
JSON_STRUCT(
    User,
    JSON_FIELD(id),
    JSON_FIELD(name),
    JSON_FIELD(age),
    JSON_FIELD(address),
    JSON_FIELD(tags)
);
```

其中：

```cpp
JSON_FIELD(id)
```

展开为：

```cpp
json::field<&Self::id>("id")
```

这里宏只承担两项工作：

1. 将成员名转换为字符串；
2. 减少模板特化的样板代码。

实际的类型推导、字段访问和 decoder 分发仍由模板完成，而不是由宏实现。

建议控制宏的职责，不要在宏中生成大段解码逻辑。

---

## 8. 字段描述的完整设计

实际工程中，字段描述通常需要携带更多策略。

```cpp
enum class MissingFieldPolicy {
    Error,
    KeepDefault,
    AssignDefault,
};

template <
    auto MemberPtr,
    MissingFieldPolicy MissingPolicy,
    class CustomDecoder = void>
struct FieldDescriptor {
    using pointer_type = decltype(MemberPtr);
    using traits = MemberPointerTraits<pointer_type>;

    using owner_type = typename traits::class_type;
    using value_type = typename traits::member_type;
    using custom_decoder_type = CustomDecoder;

    static constexpr auto member = MemberPtr;
    static constexpr auto missing_policy = MissingPolicy;

    std::string_view name;
};
```

可以提供不同的字段构造函数。

### 8.1 必填字段

```cpp
template <auto MemberPtr>
constexpr auto required_field(std::string_view name) {
    return FieldDescriptor<
        MemberPtr,
        MissingFieldPolicy::Error>{
        .name = name,
    };
}
```

宏：

```cpp
#define JSON_REQUIRED_FIELD(MEMBER) \
    json::required_field<&Self::MEMBER>(#MEMBER)
```

字段缺失时返回错误。

### 8.2 可缺失字段

```cpp
template <auto MemberPtr>
constexpr auto optional_field(std::string_view name) {
    return FieldDescriptor<
        MemberPtr,
        MissingFieldPolicy::KeepDefault>{
        .name = name,
    };
}
```

宏：

```cpp
#define JSON_OPTIONAL_FIELD(MEMBER) \
    json::optional_field<&Self::MEMBER>(#MEMBER)
```

这里的“optional”表示 JSON 字段可以缺失，并不等价于成员类型必须是 `std::optional<T>`。

例如：

```cpp
struct Config {
    int timeout_ms = 3000;
};
```

可以注册为：

```cpp
JSON_OPTIONAL_FIELD(timeout_ms)
```

缺失时保留 C++ 对象中的初始值 `3000`。

### 8.3 JSON 字段重命名

```cpp
#define JSON_NAMED_FIELD(MEMBER, JSON_NAME) \
    json::required_field<&Self::MEMBER>(JSON_NAME)
```

使用：

```cpp
JSON_NAMED_FIELD(user_id, "userId")
```

### 8.4 自定义 decoder

字段描述可以带一个 decoder 类型：

```cpp
template <auto MemberPtr, class Decoder>
constexpr auto custom_field(
    std::string_view name,
    Decoder)
{
    return FieldDescriptor<
        MemberPtr,
        MissingFieldPolicy::Error,
        Decoder>{
        .name = name,
    };
}
```

例如：

```cpp
JSON_CUSTOM_FIELD(created_at, "createdAt", TimestampDecoder{})
```

SAX 层发现字段带有自定义 decoder 时，优先使用该 decoder，而不是按字段类型走默认分发。

---

## 9. 字段值的统一访问

字段描述应提供统一的成员访问方法：

```cpp
template <
    auto MemberPtr,
    MissingFieldPolicy MissingPolicy,
    class CustomDecoder>
struct FieldDescriptor {
    // ...

    constexpr decltype(auto) get(owner_type& object) const noexcept {
        return object.*MemberPtr;
    }

    constexpr decltype(auto) get(
        const owner_type& object) const noexcept
    {
        return object.*MemberPtr;
    }
};
```

使用时：

```cpp
auto& value = field.get(user);
```

比在 decoder 中到处编写：

```cpp
user.*decltype(field)::member
```

更清晰，也便于以后增加 setter、只读字段或转换字段。

---

## 10. 编译期遍历字段

`StructMetadata<T>::fields` 是一个 `std::tuple`，因此可以用 `std::apply` 遍历。

```cpp
template <JsonStruct T, class Function>
constexpr void for_each_field(Function&& function) {
    std::apply(
        [&](const auto&... fields) {
            (function(fields), ...);
        },
        StructMetadata<T>::fields);
}
```

使用：

```cpp
for_each_field<User>([](const auto& field) {
    // field.name
    // field.value_type
    // field.member
});
```

这套遍历能力可以被多个模块复用：

- JSON SAX decoder；
- JSON encoder；
- debug printer；
- 字段校验器；
- JSON Schema 生成器；
- 配置差异比较器。

---

## 11. 根据 JSON key 查找字段

SAX parser 读取到 object key 后，需要在当前结构体元数据中找到对应字段。

最简单的实现是线性匹配：

```cpp
template <JsonStruct T, class Function>
bool visit_field_by_name(
    std::string_view name,
    Function&& function)
{
    bool found = false;

    for_each_field<T>([&](const auto& field) {
        if (!found && field.name == name) {
            found = true;
            function(field);
        }
    });

    return found;
}
```

调用形式：

```cpp
visit_field_by_name<User>(
    current_key,
    [&](const auto& field) {
        auto& member = field.get(user);
        start_decoder(member);
    });
```

`function` 是泛型 lambda。匹配到不同字段时，lambda 会针对对应的 `FieldDescriptor` 类型实例化，因此编译器知道具体字段类型。

这使得运行时只负责匹配字段名，而匹配后的类型处理仍然是编译期静态分发。

### 11.1 为什么不能直接返回字段指针

不同字段的 descriptor 类型不同：

```cpp
FieldDescriptor<&User::id>
FieldDescriptor<&User::name>
```

它们无法直接作为同一个具体返回类型。

因此常见做法是：

- visitor；
- 泛型 lambda 回调；
- `std::variant`；
- 类型擦除的运行时字段表。

对于 SAX decoder，visitor 通常最简单。

### 11.2 字段数量较少时使用线性查找

普通业务 struct 往往只有几个到几十个字段，线性匹配的优势是：

- 实现简单；
- 无额外静态表；
- 不需要哈希；
- 分支容易被编译器展开；
- 元数据结构保持纯编译期。

当结构体字段很多、解码频率极高时，可以再增加：

- 编译期哈希；
- 字符串长度分组；
- 首字符分组；
- 生成静态查找表；
- 代码生成的 switch。

元数据层不应强制绑定某一种查找方式。

---

## 12. 类型 decoder 的编译期分发

需要定义一个统一的类型分类机制。

```cpp
template <class T>
struct TypeDecoder;
```

也可以使用一个入口函数：

```cpp
template <class T>
auto make_decoder(T& output);
```

内部通过 `if constexpr` 判断类型类别：

```cpp
template <class T>
auto make_decoder(T& output) {
    using Value = std::remove_cvref_t<T>;

    if constexpr (Boolean<Value>) {
        return BooleanDecoder<Value>{output};
    } else if constexpr (Integer<Value>) {
        return IntegerDecoder<Value>{output};
    } else if constexpr (FloatingPoint<Value>) {
        return NumberDecoder<Value>{output};
    } else if constexpr (String<Value>) {
        return StringDecoder<Value>{output};
    } else if constexpr (Optional<Value>) {
        return OptionalDecoder<Value>{output};
    } else if constexpr (SequenceContainer<Value>) {
        return SequenceDecoder<Value>{output};
    } else if constexpr (StringMap<Value>) {
        return MapDecoder<Value>{output};
    } else if constexpr (JsonEnum<Value>) {
        return EnumDecoder<Value>{output};
    } else if constexpr (JsonStruct<Value>) {
        return StructDecoder<Value>{output};
    } else {
        static_assert(
            dependent_false<Value>,
            "No JSON decoder registered for this type");
    }
}
```

这样，字段匹配后只需要：

```cpp
auto& member = field.get(object);
auto decoder = make_decoder(member);
```

便可以递归构建对应字段的 SAX decoder。

---

## 13. 常用类型 traits

为了让 `make_decoder()` 易于扩展，建议将类型判断拆成 concepts。

### 13.1 `std::optional`

```cpp
template <class T>
struct IsOptional : std::false_type {};

template <class T>
struct IsOptional<std::optional<T>> : std::true_type {
    using value_type = T;
};

template <class T>
concept Optional =
    IsOptional<std::remove_cvref_t<T>>::value;
```

### 13.2 顺序容器

不要简单判断“是否有 `value_type` 和 `push_back`”，否则容易误识别业务类型。

建议显式支持：

```cpp
template <class T>
struct IsSequenceContainer : std::false_type {};

template <class T, class Allocator>
struct IsSequenceContainer<
    std::vector<T, Allocator>> : std::true_type
{
    using value_type = T;
};

template <class T, class Allocator>
struct IsSequenceContainer<
    std::deque<T, Allocator>> : std::true_type
{
    using value_type = T;
};
```

后续按需增加其它容器。

### 13.3 字符串映射

```cpp
template <class T>
struct IsStringMap : std::false_type {};

template <
    class Value,
    class Compare,
    class Allocator>
struct IsStringMap<
    std::map<std::string, Value, Compare, Allocator>>
    : std::true_type
{
    using mapped_type = Value;
};
```

如果需要支持 `std::string_view` key 或自定义字符串类型，也应通过显式 traits 扩展。

### 13.4 自定义 decoder 注册

可以提供：

```cpp
template <class T>
struct CustomTypeDecoder;
```

判断是否存在特化：

```cpp
template <class T>
concept HasCustomTypeDecoder = requires {
    typename CustomTypeDecoder<T>::type;
};
```

在默认类型分发前优先判断自定义 decoder：

```cpp
if constexpr (HasCustomTypeDecoder<Value>) {
    return typename CustomTypeDecoder<Value>::type{output};
}
```

适用于：

- 时间类型；
- IP 地址；
- UUID；
- URI；
- 自定义十进制类型；
- 业务 ID 类型。

---

## 14. SAX Struct Decoder 的状态模型

模板元数据不直接依赖 SAX 接口，但 Struct Decoder 通常至少需要维护以下状态：

```cpp
template <JsonStruct T>
class StructDecoder {
public:
    explicit StructDecoder(T& output);

private:
    T& output_;

    std::string current_key_;

    // 已出现字段的状态，用于检查 required 和重复字段。
    FieldPresenceSet presence_;

    // 当前子字段的 decoder。
    ChildDecoderStorage child_;
};
```

处理过程：

```text
object_begin
    ↓
读取 key
    ↓
visit_field_by_name<T>(key)
    ↓
取得 field.get(output_)
    ↓
根据字段类型创建 child decoder
    ↓
将后续 SAX 事件转发给 child decoder
    ↓
child decoder 完成
    ↓
继续读取下一个 key
    ↓
object_end 时检查 required 字段
```

核心伪代码：

```cpp
void on_key(std::string_view key) {
    bool found = visit_field_by_name<T>(
        key,
        [&](const auto& field) {
            mark_present(field);

            auto& member = field.get(output_);

            child_.emplace(
                make_decoder(member));
        });

    if (!found) {
        handle_unknown_field(key);
    }
}
```

这里最大的实现问题通常不是字段模板本身，而是如何存储“不同具体类型的 child decoder”。

可选方案包括：

1. decoder 继承统一的 SAX handler 接口；
2. 使用小对象类型擦除；
3. 使用 `std::variant` 保存所有可能 decoder；
4. parser 使用模板 continuation，不保存通用 handler；
5. 每个字段 descriptor 生成静态事件回调表。

如果已有 SAX 框架，应让字段元数据适配现有 handler 模型，而不是反过来修改字段元数据。

---

## 15. 字段出现状态

在 object 结束时，需要知道哪些字段出现过。

不能只通过成员当前值判断，因为：

- `0` 可能是有效输入；
- 空字符串可能是有效输入；
- `std::optional` 的 `null` 与字段缺失不同；
- 默认值可能与输入值相同。

因此需要单独记录 presence。

### 15.1 为字段分配编译期 index

字段描述可以增加 index，也可以在 tuple 遍历时使用下标。

```cpp
template <class T, class Function, std::size_t... I>
constexpr void for_each_field_impl(
    Function&& function,
    std::index_sequence<I...>)
{
    auto&& fields = StructMetadata<T>::fields;

    (function(
        std::integral_constant<std::size_t, I>{},
        std::get<I>(fields)), ...);
}
```

完整入口：

```cpp
template <JsonStruct T, class Function>
constexpr void for_each_field(Function&& function) {
    constexpr auto count =
        std::tuple_size_v<
            decltype(StructMetadata<T>::fields)>;

    for_each_field_impl<T>(
        std::forward<Function>(function),
        std::make_index_sequence<count>{});
}
```

Struct Decoder 可以使用：

```cpp
std::bitset<field_count<T>> presence_;
```

匹配字段时：

```cpp
presence_.set(index);
```

object 结束时遍历 descriptor，检查必填字段对应 bit 是否被设置。

---

## 16. 缺失字段与 `std::optional` 的区别

需要明确区分两个概念。

### JSON 字段可以缺失

由字段 descriptor 控制：

```cpp
JSON_OPTIONAL_FIELD(timeout_ms)
```

含义是 JSON 中没有该字段时不报错。

### JSON 值可以为 null

由字段值类型或 decoder 策略控制：

```cpp
std::optional<int> age;
```

含义是：

```json
{
  "age": null
}
```

可以被接受。

因此以下注册是合理的：

```cpp
struct User {
    std::optional<int> age;
};

JSON_STRUCT(
    User,
    JSON_REQUIRED_FIELD(age)
);
```

它表示：

- `age` 字段必须出现；
- 但它的值可以是 `null`。

同样：

```cpp
struct Config {
    int timeout_ms = 3000;
};

JSON_STRUCT(
    Config,
    JSON_OPTIONAL_FIELD(timeout_ms)
);
```

表示字段可以完全缺失，缺失时保留 `3000`。

不要根据字段类型是否为 `std::optional` 自动决定字段是否 required，否则会混淆协议语义。

---

## 17. 默认值策略

最简单的默认值方式是直接使用 C++ 成员初始值：

```cpp
struct Config {
    int timeout_ms = 3000;
    bool keep_alive = true;
};
```

解码前默认构造对象：

```cpp
Config config;
```

可缺失字段注册为：

```cpp
JSON_OPTIONAL_FIELD(timeout_ms)
JSON_OPTIONAL_FIELD(keep_alive)
```

字段缺失时保持成员当前值。

这种方式比把默认值重复写入元数据更简单：

```cpp
JSON_DEFAULT_FIELD(timeout_ms, 3000)
```

只有在以下情况才需要显式默认值：

- decoder 对已有对象做覆盖更新；
- 同一个 struct 在不同协议中有不同默认值；
- 默认值需要由 decode options 决定；
- 需要从 schema 中导出默认值。

对于普通“JSON 创建一个新对象”的场景，建议优先使用 C++ 成员初始化器作为默认值来源。

---

## 18. 未知字段策略

元数据层负责判断字段名是否匹配，未知字段的处理由 decode options 决定。

```cpp
enum class UnknownFieldPolicy {
    Ignore,
    Reject,
};
```

SAX 场景中，忽略未知字段不能只忽略当前事件，而是需要跳过整个 JSON value，包括嵌套 object 和 array。

因此 parser 或 SAX adapter 最好提供：

```cpp
SkipValueDecoder
```

当字段未知且策略为 `Ignore` 时，将后续事件交给 `SkipValueDecoder`，直到该 value 完整结束。

元数据层只需返回“未找到字段”，不应承担跳过 SAX value 的状态管理。

---

## 19. 重复字段策略

JSON object 可能出现重复 key：

```json
{
  "id": 1,
  "id": 2
}
```

通过 presence bit 可以检测重复字段。

```cpp
enum class DuplicateFieldPolicy {
    Reject,
    KeepFirst,
    KeepLast,
};
```

建议默认使用 `Reject`，特别是：

- 配置文件；
- 鉴权数据；
- 签名数据；
- 安全敏感协议。

如果选择 `KeepLast`，第二次出现时重新启动该字段 decoder，并覆盖旧值。

---

## 20. 字段名的生命周期

字段 descriptor 通常保存：

```cpp
std::string_view name;
```

它必须指向静态生命周期数据。

宏生成的：

```cpp
#MEMBER
```

是字符串字面量，满足要求。

显式重命名：

```cpp
JSON_NAMED_FIELD(user_id, "userId")
```

同样是字符串字面量。

不应允许业务侧传入运行时构造的 `std::string` 作为 metadata 字段名，因为 `StructMetadata<T>::fields` 通常是 `constexpr` 静态对象。

---

## 21. 编译期检查

注册元数据时应尽量在编译期发现错误。

### 21.1 检查字段 owner type

所有字段成员指针都应属于当前结构体：

```cpp
template <class Struct, class Field>
consteval bool field_belongs_to_struct() {
    return std::same_as<
        Struct,
        typename Field::owner_type>;
}
```

在 `StructMetadata<T>` 的辅助构造器中检查：

```cpp
static_assert(
    (field_belongs_to_struct<T, Fields>() && ...),
    "JSON field belongs to another struct");
```

### 21.2 检查重复 JSON 字段名

可以使用 `consteval` 两两比较字段名：

```cpp
static_assert(
    unique_field_names(fields),
    "Duplicate JSON field name");
```

字段数通常很少，编译期 O(N²) 比较可以接受。

### 21.3 检查字段是否可解码

```cpp
template <class T>
concept JsonDecodable =
    BuiltinJsonType<T> ||
    Optional<T> ||
    SequenceContainer<T> ||
    StringMap<T> ||
    JsonEnum<T> ||
    JsonStruct<T> ||
    HasCustomTypeDecoder<T>;
```

注册时检查：

```cpp
static_assert(
    JsonDecodable<typename Field::value_type>,
    "No JSON decoder for field type");
```

这样错误会在注册 struct 时出现，而不是在某个复杂模板调用栈深处出现。

---

## 22. 使用工厂函数代替直接构造 tuple

为了集中执行编译期校验，建议不要让宏直接构造裸 `std::tuple`，而是提供：

```cpp
template <class Struct, class... Fields>
consteval auto define_struct(Fields... fields) {
    static_assert(
        (std::same_as<
            Struct,
            typename Fields::owner_type> && ...));

    static_assert(unique_field_names(fields...));

    return std::tuple{fields...};
}
```

宏：

```cpp
#define JSON_STRUCT(TYPE, ...)                         \
    template <>                                        \
    struct json::StructMetadata<TYPE> {                \
        using Self = TYPE;                             \
        static constexpr auto fields =                 \
            json::define_struct<Self>(__VA_ARGS__);    \
    }
```

这样所有结构体注册都会经过统一检查。

---

## 23. 推荐的公开 API

### 23.1 字段声明

```cpp
JSON_FIELD(member)
JSON_REQUIRED_FIELD(member)
JSON_OPTIONAL_FIELD(member)
JSON_NAMED_FIELD(member, "jsonName")
JSON_NAMED_OPTIONAL_FIELD(member, "jsonName")
JSON_CUSTOM_FIELD(member, "jsonName", Decoder{})
```

可以规定：

```cpp
JSON_FIELD(member)
```

等价于：

```cpp
JSON_REQUIRED_FIELD(member)
```

避免出现不明确的默认语义。

### 23.2 结构体声明

```cpp
JSON_STRUCT(
    User,
    JSON_FIELD(id),
    JSON_FIELD(name),
    JSON_OPTIONAL_FIELD(age)
);
```

### 23.3 类型扩展

```cpp
template <>
struct json::CustomTypeDecoder<Timestamp> {
    using type = TimestampSaxDecoder;
};
```

枚举可以单独提供：

```cpp
JSON_ENUM(
    Status,
    JSON_ENUM_VALUE(Pending, "pending"),
    JSON_ENUM_VALUE(Running, "running"),
    JSON_ENUM_VALUE(Finished, "finished")
);
```

---

## 24. 完整使用示例

```cpp
struct Address {
    std::string city;
    std::string street;
};

JSON_STRUCT(
    Address,
    JSON_FIELD(city),
    JSON_FIELD(street)
);

enum class UserState {
    Active,
    Disabled,
};

JSON_ENUM(
    UserState,
    JSON_ENUM_VALUE(Active, "active"),
    JSON_ENUM_VALUE(Disabled, "disabled")
);

struct User {
    std::uint64_t id;
    std::string name;

    std::optional<std::uint32_t> age;

    Address address;
    std::vector<std::string> tags;

    UserState state = UserState::Active;

    int timeout_ms = 3000;
};

JSON_STRUCT(
    User,
    JSON_FIELD(id),
    JSON_NAMED_FIELD(name, "userName"),
    JSON_OPTIONAL_FIELD(age),
    JSON_FIELD(address),
    JSON_OPTIONAL_FIELD(tags),
    JSON_OPTIONAL_FIELD(state),
    JSON_OPTIONAL_FIELD(timeout_ms)
);
```

输入：

```json
{
  "id": 1001,
  "userName": "Tom",
  "age": null,
  "address": {
    "city": "Shanghai",
    "street": "Nanjing Road"
  },
  "tags": ["admin", "internal"]
}
```

处理逻辑：

1. SAX parser 进入根 object；
2. 创建 `StructDecoder<User>`；
3. 读取 key `id`；
4. 在 `StructMetadata<User>::fields` 中匹配字段；
5. 取得 `User::id` 的成员引用；
6. 根据成员类型创建整数 decoder；
7. 整数 decoder 消费 number 事件；
8. 继续处理下一字段；
9. 遇到 `address` 时创建 `StructDecoder<Address>`；
10. 根 object 结束时检查所有 required 字段；
11. `state` 和 `timeout_ms` 未出现，保留 C++ 初始值。

---

## 25. 与具体 SAX 实现的适配边界

模板元数据层最好只提供以下能力：

```cpp
template <class T>
concept JsonStruct;

template <class T, class Function>
void for_each_field(Function&&);

template <class T, class Function>
bool visit_field_by_name(
    std::string_view,
    Function&&);

template <class Field, class Object>
decltype(auto) get_field_value(
    const Field&,
    Object&);

template <class T>
constexpr std::size_t field_count;
```

SAX decoder 自己负责：

- object/array 状态；
- 当前 key；
- 子 decoder 生命周期；
- 跳过未知 value；
- 错误路径；
- parser 回调适配；
- number token 转换；
- 字符串分片拼接；
- 深度限制。

这样 metadata 机制可以独立测试，也不会绑定某一个 JSON parser。

---

## 26. 不建议采用的方式

### 26.1 保存成员 offset

例如：

```cpp
offsetof(User, id)
```

然后用：

```cpp
reinterpret_cast<char*>(&user) + offset
```

这种方式存在明显限制：

- 只适用于 standard-layout 类型；
- 丢失字段静态类型；
- 容易出现对齐和别名问题；
- 需要额外保存运行时类型信息；
- 无法自然支持自定义 setter；
- 可维护性差。

成员指针是更安全、更符合 C++ 类型系统的方案。

### 26.2 使用 `void*` 加类型枚举

例如：

```cpp
struct Field {
    std::string_view name;
    std::size_t offset;
    JsonType type;
};
```

这相当于自己实现一套不完整的运行时反射系统，会重复 C++ 编译器已经掌握的类型信息。

模板 descriptor 可以让编译器直接生成对应字段的静态解码逻辑，不需要运行时 switch 和不安全转换。

### 26.3 依赖聚合类型自动拆包

某些库可以通过 aggregate initialization 推断结构体有多少成员，并按位置访问成员，但通常不能可靠得到源码成员名称。

JSON 映射依赖字段名：

```json
{
  "userName": "Tom"
}
```

因此即使能按位置访问字段，仍然需要额外声明名称。相比之下，成员指针元数据更直接，也支持非 aggregate 类型。

### 26.4 每个 struct 手写 SAX handler

手写方式性能很好，但会产生大量重复代码：

- key 比较；
- 类型检查；
- 字段 presence；
- required 检查；
- 嵌套 decoder；
- 错误路径拼接。

模板元数据的价值就是把这些公共逻辑抽取到通用 decoder 中。

---

## 27. 性能考虑

这套方案本质上是静态多态：

- 字段值类型在编译期已知；
- 成员访问通过成员指针完成；
- 类型 decoder 由 `if constexpr` 选择；
- 不需要 RTTI；
- 不需要 `std::any`；
- 不需要动态反射对象；
- 不需要字段值拷贝。

主要运行时开销来自：

1. JSON key 到字段 descriptor 的匹配；
2. SAX child decoder 的类型擦除或状态存储；
3. 字符串和容器本身的内存分配；
4. 数字转换和校验。

对于普通字段数量，线性字段名比较通常足够。后续若 profiling 表明字段查找占比明显，再单独优化查找策略。

不要为了预期中的字段匹配性能问题，一开始就把元数据设计成复杂的运行时哈希表。

---

## 28. 推荐实现顺序

### 第一阶段：最小可用版本

支持：

- 基础类型；
- `std::string`；
- 嵌套 struct；
- `std::vector<T>`；
- `std::optional<T>`；
- required / optional 字段；
- 字段重命名；
- 未知字段忽略或拒绝。

核心 API：

```cpp
JSON_STRUCT
JSON_FIELD
JSON_OPTIONAL_FIELD
visit_field_by_name
make_decoder
```

### 第二阶段：增强类型支持

增加：

- enum；
- map；
- 时间和 UUID 等自定义 decoder；
- duplicate key 检测；
- 错误路径；
- 数值范围检查；
- null 策略。

### 第三阶段：编译期检查

增加：

- 重复字段名检查；
- owner type 检查；
- 字段是否可解码检查；
- 字段数量；
- presence bitset。

### 第四阶段：性能优化

根据 profiling 决定是否增加：

- 编译期字段 hash；
- 字段名长度分组；
- 首字符分派；
- 静态函数表；
- 代码生成。

---

## 29. 最终建议

推荐使用以下组合实现类反射机制：

```text
StructMetadata<T>
        +
FieldDescriptor<&T::member>
        +
std::tuple
        +
成员指针 traits
        +
visit_field_by_name()
        +
make_decoder()
        +
少量声明宏
```

其中最重要的设计原则是：

1. **宏只生成字段元数据，不生成解码实现；**
2. **成员指针负责类型安全的字段访问；**
3. **模板负责类型识别和静态 decoder 分发；**
4. **运行时只做 JSON key 匹配和 SAX 状态推进；**
5. **元数据层不依赖具体 JSON parser；**
6. **字段缺失与值为 null 必须分别建模；**
7. **默认值优先使用 C++ 成员初始化器；**
8. **自定义业务类型通过 traits 或 decoder 特化扩展。**

最终业务侧只需要注册字段：

```cpp
JSON_STRUCT(
    User,
    JSON_FIELD(id),
    JSON_NAMED_FIELD(name, "userName"),
    JSON_OPTIONAL_FIELD(age),
    JSON_FIELD(address),
    JSON_OPTIONAL_FIELD(tags)
);
```

通用 SAX decoder 即可利用这些编译期元数据，把 JSON 事件递归映射到 C++ 对象，而无需为每个结构体手写完整的解析代码。

---

## 30. Fiber 仓库落地方案

Fiber 当前的 typed JSON API 是基于 `JsonParser::current_token()` 的同步拉取式解析，而不是需要长期保存 child decoder 的 SAX handler。因此仓库实现保留本文的成员指针元数据方案，但将 `make_decoder()` 调整为同步入口：

```cpp
template<typename T>
json::ParseStatus json::parse_value(
    json::JsonParser& parser,
    mem::BufPool& pool,
    T& out) noexcept;
```

代码位于：

- `include/fiber/common/json/JsonStructMetadata.h`：字段 descriptor、基类字段展开、编译期字段名检查和声明宏；
- `include/fiber/common/json/JsonStructDecode.h`：`parse_value<T>()`、类型静态分派、presence 和 object 策略；
- `include/fiber/common/json/JsonParse.h`：object 完成后的事务式 finalizer 支持。

公开宏使用项目统一前缀：

```cpp
FIBER_JSON_STRUCT
FIBER_JSON_FIELD
FIBER_JSON_OPTIONAL_FIELD
FIBER_JSON_NAMED_FIELD
FIBER_JSON_NAMED_OPTIONAL_FIELD
FIBER_JSON_BASE
FIBER_JSON_CUSTOM_FIELD
FIBER_JSON_OPTIONAL_CUSTOM_FIELD
FIBER_JSON_OPTIONAL_CONSTANT
FIBER_JSON_OPTIONAL_IGNORED
```

第一阶段直接支持项目现有的 pool-backed 类型：

- `bool`、整数、`double` 和 `std::string_view`；
- `Nullable<T>` 和 `std::optional<T>`；
- `JsonArray<T>`、`JsonObject<T>` 和 `JsonAny`；
- 注册了 `StructMetadata<T>` 的嵌套结构体；
- `CustomValueDecoder<T>` 类型级扩展和字段级 custom parser。

`StructMetadata<T>` 默认忽略未知字段并对重复字段采用 last-wins，以保持现有 typed parser 行为。结构体可以通过 `StructDecodeOptions` 改为拒绝未知字段或重复字段。未出现的 optional 普通成员保留 C++ 默认值；未出现的 `Nullable<T>` 成员在提交前转为 `Absent`；required 字段在 finalizer 中检查。

Nacos DTO 只在 `apps/nacos/src/dto/JsonDecode.cpp` 注册字段和保留 `parse_json()` 薄入口，反射模板及递归解码逻辑不放在应用模块中。编码器暂时保持独立实现，待输出字段顺序、计算字段和 `Absent` 省略策略单独建模后再考虑复用元数据。
