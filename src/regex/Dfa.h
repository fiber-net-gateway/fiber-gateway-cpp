#ifndef FIBER_REGEX_DFA_H
#define FIBER_REGEX_DFA_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fiber/common/mem/BufPool.h>
#include <fiber/regex/Regex.h>
#include <fiber/regex/RegexSet.h>

#include "regex/Program.h"
#include "regex/Sparse.h"

namespace fiber::regex::detail {

enum class DfaKind : std::uint8_t {
    FirstMatch, // leftmost-first, like Perl
    LongestMatch, // leftmost-longest; used by the reverse search
    ManyMatch, // every match in a RegexSet
};

// A DFA state: an ordered list of instruction list heads plus flags, followed in
// memory by one next pointer per byte class and one for end of text.
struct DfaState {
    const std::uint32_t *inst = nullptr;
    std::uint32_t ninst = 0;
    std::uint32_t flag = 0;
    std::uint32_t hash = 0;

    [[nodiscard]] DfaState **next() noexcept { return reinterpret_cast<DfaState **>(this + 1); }
};

// The list of instructions the DFA is working through. Marks separate threads that
// started at different positions, in leftmost-longest mode.
class DfaWorkq : public SparseSet {
public:
    void reset(std::uint32_t size, std::uint32_t max_mark) {
        size_ = size;
        max_mark_ = max_mark;
        ensure_capacity(size + max_mark);
        clear_queue();
    }

    void clear_queue() noexcept {
        clear();
        next_mark_ = size_;
    }

    [[nodiscard]] bool is_mark(std::uint32_t id) const noexcept { return id >= size_; }
    [[nodiscard]] std::uint32_t max_mark() const noexcept { return max_mark_; }
    [[nodiscard]] std::uint32_t capacity() const noexcept { return size_ + max_mark_; }

    void mark() noexcept { insert_new(next_mark_++); }

private:
    std::uint32_t size_ = 0;
    std::uint32_t max_mark_ = 0;
    std::uint32_t next_mark_ = 0;
};

class DfaWorkspace;

// The states built so far for one program and kind. Lives in Scratch and is used
// by one thread only, so unlike RE2 there are no locks and no atomics.
class DfaCache {
public:
    static constexpr std::size_t kStartStates = 8;

    explicit DfaCache(std::uint64_t epoch) noexcept : epoch_(epoch) {}

    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t state_count() const noexcept { return count_; }

    [[nodiscard]] DfaState *&start(std::size_t index) noexcept { return start_[index]; }

    // Finds the state or adds it. Returns nullptr when the budget does not allow it.
    [[nodiscard]] DfaState *intern(DfaWorkspace &workspace, const std::uint32_t *inst, std::uint32_t ninst,
                                   std::uint32_t flag, std::uint32_t next_count) noexcept;

    void reset(DfaWorkspace &workspace) noexcept;

private:
    [[nodiscard]] bool grow_table(DfaWorkspace &workspace) noexcept;

    mem::BufPool pool_{std::size_t{64} * 1024U};
    std::vector<DfaState *> table_{};
    std::array<DfaState *, kStartStates> start_{};
    std::size_t count_ = 0;
    std::size_t bytes_ = 0;
    std::uint64_t epoch_;
};

// Memory for DFA searches, in Scratch. The budget covers every cache together, so
// the total is workers times budget however many rules there are.
class DfaWorkspace {
public:
    explicit DfaWorkspace(std::size_t budget) noexcept : budget_(budget) {}

    // Finds the cache and marks it as the most recently used.
    [[nodiscard]] DfaCache &cache_for(const Program &program, DfaKind kind);

    // Claims `bytes` for `self`, evicting the least recently used caches if it has
    // to. Fails when that leaves nothing but `self`.
    [[nodiscard]] bool reserve(const DfaCache *self, std::size_t bytes) noexcept;
    void release(std::size_t bytes) noexcept { used_ -= bytes; }

    [[nodiscard]] std::size_t bytes_used() const noexcept { return used_; }
    [[nodiscard]] std::size_t cache_count() const noexcept { return lru_.size(); }

    DfaWorkq q0{};
    DfaWorkq q1{};
    std::vector<std::uint32_t> stack{};
    std::array<std::vector<std::uint32_t>, 2> buffers{};
    std::vector<std::uint32_t> saved_start{};
    std::vector<std::uint32_t> saved_state{};

private:
    struct Entry {
        Entry(std::uint64_t cache_key, std::uint64_t epoch) noexcept : key(cache_key), cache(epoch) {}

        std::uint64_t key;
        DfaCache cache;
    };

    std::size_t budget_;
    std::size_t used_ = 0;
    std::list<Entry> lru_{}; // most recently used first
    std::unordered_map<std::uint64_t, std::list<Entry>::iterator> caches_{};
};

// Where a stream is between two chunks. A cache reset can happen in between, as
// other requests share the cache, so the state is kept by value too: `state` is
// only trusted while `epoch` still names the cache that built it.
struct DfaCursor {
    std::vector<std::uint32_t> inst{};
    std::uint32_t flag = 0;
    DfaState *state = nullptr;
    std::uint64_t epoch = 0;
    bool started = false;
    bool dead = false;
};

enum class DfaSearchKind : std::uint8_t { FirstMatch, LongestMatch, FullMatch, ManyMatch };

struct DfaRequest {
    std::string_view context{};
    std::size_t begin = 0;
    std::size_t end = 0;
    bool anchored = false;
    DfaSearchKind kind = DfaSearchKind::FirstMatch;
    bool want_position = false; // otherwise any match will do
    MatchSet *matches = nullptr;
};

struct DfaResult {
    MatchStatus status = MatchStatus::NoMatch;
    // Where the match ends, or where it begins for a reversed program.
    std::size_t position = 0;
};

// Searches with a lazily built DFA. It finds where a match ends, but not captures.
// Unlike RE2 it cannot fail: when memory runs short it degrades to stepping without
// a cache, which is slower but needs none.
[[nodiscard]] DfaResult dfa_search(const Program &program, DfaWorkspace &workspace, const DfaRequest &request,
                                   WorkBudget &budget) noexcept;

// Feeds the next chunk of a stream through a ManyMatch program.
[[nodiscard]] MatchStatus dfa_feed(const Program &program, DfaWorkspace &workspace, DfaCursor &cursor,
                                   std::string_view chunk, MatchSet &matches, WorkBudget &budget) noexcept;

// Ends the stream, which settles the matches that depend on the end of the text.
[[nodiscard]] MatchStatus dfa_finish(const Program &program, DfaWorkspace &workspace, DfaCursor &cursor,
                                     MatchSet &matches, WorkBudget &budget) noexcept;

} // namespace fiber::regex::detail

#endif // FIBER_REGEX_DFA_H
