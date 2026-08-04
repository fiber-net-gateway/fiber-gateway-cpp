#ifndef FIBER_ACCESS_SERVER_COMPILED_HEADER_TEMPLATES_H
#define FIBER_ACCESS_SERVER_COMPILED_HEADER_TEMPLATES_H

#include "../../../../src/http/HeaderMap.h"
#include "CompiledTemplate.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fiber::access_server {

struct CompiledTemplateEntry {
    std::string name;
    CompiledTemplate value;
};

class CompiledHeaderTemplates {
    using IndexedStorage = http::HeaderMap<CompiledTemplate>;
    using OrderedStorage = std::vector<CompiledTemplateEntry>;

public:
    class EntryView {
    public:
        [[nodiscard]] std::string_view name() const noexcept { return name_; }
        [[nodiscard]] std::uint64_t hash() const noexcept { return hash_; }
        [[nodiscard]] const CompiledTemplate &value() const noexcept { return *value_; }

    private:
        friend class CompiledHeaderTemplates;
        friend class ConstIterator;

        EntryView(std::string_view name, std::uint64_t hash, const CompiledTemplate &value) noexcept :
            name_(name), hash_(hash), value_(&value) {}

        std::string_view name_;
        std::uint64_t hash_ = 0;
        const CompiledTemplate *value_ = nullptr;
    };

    class ConstIterator {
        using IndexedIterator = IndexedStorage::ConstIterator;
        using OrderedIterator = OrderedStorage::const_iterator;

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = EntryView;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = EntryView;

        ConstIterator() noexcept = default;

        [[nodiscard]] EntryView operator*() const noexcept;

        ConstIterator &operator++() noexcept;

        ConstIterator operator++(int) noexcept {
            ConstIterator copy = *this;
            ++(*this);
            return copy;
        }

        [[nodiscard]] bool operator==(const ConstIterator &other) const noexcept;
        [[nodiscard]] bool operator!=(const ConstIterator &other) const noexcept { return !(*this == other); }

    private:
        friend class CompiledHeaderTemplates;

        explicit ConstIterator(IndexedIterator iterator) noexcept : iterator_(iterator) {}
        explicit ConstIterator(OrderedIterator iterator) noexcept : iterator_(iterator) {}

        std::variant<std::monostate, IndexedIterator, OrderedIterator> iterator_;
    };

    class Builder {
    public:
        Builder() = default;
        explicit Builder(std::size_t expected_size) { entries_.reserve(expected_size); }

        Builder(const Builder &) = delete;
        Builder &operator=(const Builder &) = delete;
        Builder(Builder &&) noexcept = default;
        Builder &operator=(Builder &&) noexcept = default;

        void insert(std::string name, CompiledTemplate value);

        [[nodiscard]] std::span<CompiledTemplateEntry> entries() noexcept { return entries_; }
        [[nodiscard]] std::span<const CompiledTemplateEntry> entries() const noexcept { return entries_; }
        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
        [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

        [[nodiscard]] CompiledHeaderTemplates build() &&;

    private:
        OrderedStorage entries_;
    };

    CompiledHeaderTemplates() noexcept = default;
    CompiledHeaderTemplates(const CompiledHeaderTemplates &) = default;
    CompiledHeaderTemplates &operator=(const CompiledHeaderTemplates &) = default;
    CompiledHeaderTemplates(CompiledHeaderTemplates &&) noexcept = default;
    CompiledHeaderTemplates &operator=(CompiledHeaderTemplates &&) noexcept = default;
    ~CompiledHeaderTemplates() = default;

    [[nodiscard]] bool contains(std::string_view name) const noexcept;
    [[nodiscard]] bool contains(std::string_view lowcase_name, std::uint64_t hash) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] ConstIterator begin() const noexcept;
    [[nodiscard]] ConstIterator end() const noexcept;

private:
    explicit CompiledHeaderTemplates(IndexedStorage storage) noexcept : storage_(std::move(storage)) {}
    explicit CompiledHeaderTemplates(OrderedStorage storage) noexcept : storage_(std::move(storage)) {}

    std::variant<IndexedStorage, OrderedStorage> storage_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_COMPILED_HEADER_TEMPLATES_H
