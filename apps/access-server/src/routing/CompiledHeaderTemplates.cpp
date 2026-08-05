#include "CompiledHeaderTemplates.h"

#include <cassert>
#include <utility>

namespace fiber::access_server {
CompiledHeaderTemplates::Builder::Builder(std::size_t expected_size) {
    entries_.reserve(expected_size < kMaxEntries ? expected_size : kMaxEntries);
}

std::expected<void, CompiledHeaderTemplates::InsertError>
CompiledHeaderTemplates::Builder::insert(std::string name, CompiledTemplate value) {
    if (entries_.size() >= kMaxEntries || name.size() > kMaxNameBytes - name_bytes_) {
        return std::unexpected(InsertError::TooLarge);
    }
    for (const CompiledTemplateEntry &entry: entries_) {
        if (http::http_header_name_equals_ci(entry.name, name)) {
            return std::unexpected(InsertError::DuplicateName);
        }
    }

    name_bytes_ += name.size();
    entries_.push_back(CompiledTemplateEntry{
            .name = std::move(name),
            .value = std::move(value),
    });
    return {};
}

CompiledHeaderTemplates CompiledHeaderTemplates::Builder::build() && {
    Storage::Builder builder(entries_.size());
    for (CompiledTemplateEntry &entry: entries_) {
        const bool inserted = builder.insert(entry.name, std::move(entry.value));
        assert(inserted);
        (void) inserted;
    }
    return CompiledHeaderTemplates(std::move(builder).build());
}

bool CompiledHeaderTemplates::contains(std::string_view name) const noexcept { return storage_.contains(name); }

bool CompiledHeaderTemplates::contains(std::string_view lowcase_name, std::uint64_t hash) const noexcept {
    return storage_.contains(lowcase_name, hash);
}

std::size_t CompiledHeaderTemplates::size() const noexcept { return storage_.size(); }

CompiledHeaderTemplates::ConstIterator CompiledHeaderTemplates::begin() const noexcept { return storage_.begin(); }

CompiledHeaderTemplates::ConstIterator CompiledHeaderTemplates::end() const noexcept { return storage_.end(); }

} // namespace fiber::access_server
