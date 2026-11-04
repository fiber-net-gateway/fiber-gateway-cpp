#include "script/std/RegexCache.h"

#include <cstddef>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace fiber::script::std_lib {

namespace {

constexpr std::size_t kCapacity = 128;

struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
};

class PatternCache {
public:
    const regex::Regex *get(std::string_view pattern) {
        const auto it = index_.find(pattern);
        if (it != index_.end()) {
            entries_.splice(entries_.begin(), entries_, it->second);
            return as_pointer(entries_.front());
        }

        if (entries_.size() == kCapacity) {
            index_.erase(entries_.back().pattern);
            entries_.pop_back();
        }
        auto compiled = regex::Regex::compile(pattern);
        entries_.push_front(Entry{.pattern = std::string(pattern),
                                  .regex = compiled ? std::optional(std::move(*compiled)) : std::nullopt});
        index_.emplace(entries_.front().pattern, entries_.begin());
        return as_pointer(entries_.front());
    }

private:
    struct Entry {
        std::string pattern{};
        std::optional<regex::Regex> regex{};
    };

    static const regex::Regex *as_pointer(const Entry &entry) noexcept {
        return entry.regex.has_value() ? &*entry.regex : nullptr;
    }

    std::list<Entry> entries_{};
    std::unordered_map<std::string, std::list<Entry>::iterator, StringHash, std::equal_to<>> index_{};
};

} // namespace

const regex::Regex *cached_regex(std::string_view pattern) {
    thread_local PatternCache cache;
    return cache.get(pattern);
}

regex::Scratch &regex_scratch() {
    thread_local regex::Scratch scratch;
    return scratch;
}

} // namespace fiber::script::std_lib
