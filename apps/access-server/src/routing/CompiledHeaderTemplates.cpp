#include "CompiledHeaderTemplates.h"

#include <cassert>
#include <limits>
#include <utility>

namespace fiber::access_server {
namespace {

bool has_case_insensitive_duplicate(std::span<const CompiledTemplateEntry> entries) noexcept {
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::uint64_t hash = http::http_header_name_hash(entries[i].name);
        for (std::size_t j = 0; j < i; ++j) {
            if (http::http_header_name_hash(entries[j].name) == hash &&
                http::http_header_name_equals_ci(entries[j].name, entries[i].name)) {
                return true;
            }
        }
    }
    return false;
}

bool exceeds_index_limits(std::span<const CompiledTemplateEntry> entries) noexcept {
    constexpr std::size_t kMaxEntries = std::numeric_limits<std::uint32_t>::max() / 4U;
    constexpr std::size_t kMaxNameBytes =
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) - (sizeof(std::uint32_t) - 1);
    if (entries.size() > kMaxEntries) {
        return true;
    }

    std::size_t name_bytes = 0;
    for (const CompiledTemplateEntry &entry: entries) {
        if (entry.name.size() > kMaxNameBytes - name_bytes) {
            return true;
        }
        name_bytes += entry.name.size();
    }
    return false;
}

} // namespace

CompiledHeaderTemplates::EntryView CompiledHeaderTemplates::ConstIterator::operator*() const noexcept {
    if (iterator_.index() == 1) {
        const auto entry = *std::get<IndexedIterator>(iterator_);
        return EntryView(entry.name(), entry.hash(), entry.value());
    }
    const CompiledTemplateEntry &entry = *std::get<OrderedIterator>(iterator_);
    return EntryView(entry.name, http::http_header_name_hash(entry.name), entry.value);
}

CompiledHeaderTemplates::ConstIterator &CompiledHeaderTemplates::ConstIterator::operator++() noexcept {
    if (iterator_.index() == 1) {
        ++std::get<IndexedIterator>(iterator_);
    } else if (iterator_.index() == 2) {
        ++std::get<OrderedIterator>(iterator_);
    }
    return *this;
}

bool CompiledHeaderTemplates::ConstIterator::operator==(const ConstIterator &other) const noexcept {
    if (iterator_.index() != other.iterator_.index()) {
        return false;
    }
    if (iterator_.index() == 1) {
        return std::get<IndexedIterator>(iterator_) == std::get<IndexedIterator>(other.iterator_);
    }
    if (iterator_.index() == 2) {
        return std::get<OrderedIterator>(iterator_) == std::get<OrderedIterator>(other.iterator_);
    }
    return true;
}

void CompiledHeaderTemplates::Builder::insert(std::string name, CompiledTemplate value) {
    entries_.push_back(CompiledTemplateEntry{
            .name = std::move(name),
            .value = std::move(value),
    });
}

CompiledHeaderTemplates CompiledHeaderTemplates::Builder::build() && {
    if (has_case_insensitive_duplicate(entries_) || exceeds_index_limits(entries_)) {
        return CompiledHeaderTemplates(std::move(entries_));
    }

    IndexedStorage::Builder builder(entries_.size());
    for (CompiledTemplateEntry &entry: entries_) {
        const bool inserted = builder.insert(entry.name, std::move(entry.value));
        assert(inserted);
        (void) inserted;
    }
    return CompiledHeaderTemplates(std::move(builder).build());
}

bool CompiledHeaderTemplates::contains(std::string_view name) const noexcept {
    if (const auto *indexed = std::get_if<IndexedStorage>(&storage_)) {
        return indexed->contains(name);
    }
    for (const CompiledTemplateEntry &entry: std::get<OrderedStorage>(storage_)) {
        if (http::http_header_name_equals_ci(entry.name, name)) {
            return true;
        }
    }
    return false;
}

bool CompiledHeaderTemplates::contains(std::string_view lowcase_name, std::uint64_t hash) const noexcept {
    if (const auto *indexed = std::get_if<IndexedStorage>(&storage_)) {
        return indexed->contains(lowcase_name, hash);
    }
    for (const CompiledTemplateEntry &entry: std::get<OrderedStorage>(storage_)) {
        if (http::http_header_name_hash(entry.name) == hash &&
            http::http_header_name_equals_ci(entry.name, lowcase_name)) {
            return true;
        }
    }
    return false;
}

std::size_t CompiledHeaderTemplates::size() const noexcept {
    if (const auto *indexed = std::get_if<IndexedStorage>(&storage_)) {
        return indexed->size();
    }
    return std::get<OrderedStorage>(storage_).size();
}

CompiledHeaderTemplates::ConstIterator CompiledHeaderTemplates::begin() const noexcept {
    if (const auto *indexed = std::get_if<IndexedStorage>(&storage_)) {
        return ConstIterator(indexed->begin());
    }
    return ConstIterator(std::get<OrderedStorage>(storage_).begin());
}

CompiledHeaderTemplates::ConstIterator CompiledHeaderTemplates::end() const noexcept {
    if (const auto *indexed = std::get_if<IndexedStorage>(&storage_)) {
        return ConstIterator(indexed->end());
    }
    return ConstIterator(std::get<OrderedStorage>(storage_).end());
}

} // namespace fiber::access_server
