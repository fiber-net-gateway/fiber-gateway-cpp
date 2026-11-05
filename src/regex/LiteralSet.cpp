#include <fiber/regex/LiteralSet.h>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <vector>

namespace fiber::regex {

namespace detail {

namespace {

constexpr std::uint32_t kNoRow = std::numeric_limits<std::uint32_t>::max();

// A dense row costs 1 KiB, and a word list has tens of thousands of nodes, nearly
// all with one or two edges. So only the root and the rare busy node get a row.
constexpr std::size_t kDenseEdges = 8;

// Node 0 is the root. Reaching it again means no literal is in progress, which is
// why 0 can double as "no edge".
struct Node {
    std::uint32_t edge_begin = 0;
    std::uint32_t edge_count = 0;
    std::uint32_t dense_row = kNoRow;
    std::uint32_t fail = 0; // longest proper suffix that is also a prefix
    std::uint32_t output = 0; // nearest node on the fail chain that ends a literal
    std::uint32_t id_begin = 0;
    std::uint32_t id_count = 0;
};

struct TrieNode {
    std::vector<std::pair<std::uint8_t, std::uint32_t>> edges{};
    std::vector<std::uint32_t> ids{};
    std::uint32_t fail = 0;
    std::uint32_t output = 0;

    [[nodiscard]] std::uint32_t edge(std::uint8_t byte) const noexcept {
        const auto it = std::find_if(edges.begin(), edges.end(), [byte](const auto &e) { return e.first == byte; });
        return it == edges.end() ? 0 : it->second;
    }
};

std::array<std::uint8_t, 256> make_fold_table(bool ignore_case) noexcept {
    std::array<std::uint8_t, 256> table{};
    for (std::size_t c = 0; c < table.size(); ++c) {
        const bool upper = ignore_case && c >= 'A' && c <= 'Z';
        table[c] = static_cast<std::uint8_t>(upper ? c + ('a' - 'A') : c);
    }
    return table;
}

} // namespace

struct LiteralBuilderState {
    explicit LiteralBuilderState(bool ignore_case) noexcept : fold(make_fold_table(ignore_case)) {}

    std::array<std::uint8_t, 256> fold;
    std::vector<TrieNode> trie{TrieNode{}};
    std::uint32_t count = 0;
};

struct CompiledLiterals {
    std::array<std::uint8_t, 256> fold{};
    std::vector<Node> nodes{};
    std::vector<std::uint8_t> edge_bytes{};
    std::vector<std::uint32_t> edge_targets{};
    std::vector<std::uint32_t> dense{};
    std::vector<std::uint32_t> ids{};
    std::uint32_t count = 0;

    [[nodiscard]] std::uint32_t follow(const Node &node, std::uint8_t byte) const noexcept {
        if (node.dense_row != kNoRow) {
            return dense[(std::size_t{node.dense_row} * 256U) + byte];
        }
        const std::uint8_t *begin = edge_bytes.data() + node.edge_begin;
        const std::uint8_t *end = begin + node.edge_count;
        const std::uint8_t *it = std::lower_bound(begin, end, byte);
        return it != end && *it == byte ? edge_targets[static_cast<std::size_t>(it - edge_bytes.data())] : 0;
    }

    [[nodiscard]] std::uint32_t step(std::uint32_t state, std::uint8_t byte) const noexcept {
        for (;;) {
            const Node &node = nodes[state];
            const std::uint32_t next = follow(node, byte);
            if (next != 0 || state == 0) {
                return next;
            }
            state = node.fail;
        }
    }

    // The node to start reporting from, or 0 if nothing ends here.
    [[nodiscard]] std::uint32_t report_from(std::uint32_t state) const noexcept {
        const Node &node = nodes[state];
        return node.id_count != 0 ? state : node.output;
    }

    void report(std::uint32_t state, MatchSet &hits) const noexcept {
        for (std::uint32_t at = report_from(state); at != 0; at = nodes[at].output) {
            const Node &node = nodes[at];
            for (std::uint32_t i = 0; i < node.id_count; ++i) {
                hits.insert(ids[node.id_begin + i]);
            }
        }
    }
};

} // namespace detail

LiteralSet::Builder::Builder(bool ignore_case) : state_(std::make_unique<detail::LiteralBuilderState>(ignore_case)) {}

LiteralSet::Builder::~Builder() = default;
LiteralSet::Builder::Builder(Builder &&) noexcept = default;
LiteralSet::Builder &LiteralSet::Builder::operator=(Builder &&) noexcept = default;

std::expected<std::uint32_t, Error> LiteralSet::Builder::add(std::string_view literal) {
    if (literal.empty()) {
        return std::unexpected(Error{.code = ErrorCode::Syntax, .offset = 0});
    }
    detail::LiteralBuilderState &state = *state_;
    std::uint32_t at = 0;
    for (const char raw: literal) {
        const std::uint8_t byte = state.fold[static_cast<unsigned char>(raw)];
        std::uint32_t next = state.trie[at].edge(byte);
        if (next == 0) {
            next = static_cast<std::uint32_t>(state.trie.size());
            state.trie.emplace_back();
            state.trie[at].edges.emplace_back(byte, next);
        }
        at = next;
    }
    const std::uint32_t id = state.count++;
    state.trie[at].ids.push_back(id);
    return id;
}

LiteralSet LiteralSet::Builder::build() && {
    std::vector<detail::TrieNode> &trie = state_->trie;

    // Breadth first, so that a node's fail link is settled before its children need it.
    std::vector<std::uint32_t> queue;
    queue.reserve(trie.size());
    for (const auto &[byte, child]: trie[0].edges) {
        queue.push_back(child);
    }
    for (std::size_t head = 0; head < queue.size(); ++head) {
        const std::uint32_t at = queue[head];
        for (const auto &[byte, child]: trie[at].edges) {
            std::uint32_t fail = trie[at].fail;
            while (fail != 0 && trie[fail].edge(byte) == 0) {
                fail = trie[fail].fail;
            }
            const std::uint32_t target = trie[fail].edge(byte);
            trie[child].fail = target;
            trie[child].output = !trie[target].ids.empty() ? target : trie[target].output;
            queue.push_back(child);
        }
    }

    auto compiled = std::make_shared<detail::CompiledLiterals>();
    compiled->fold = state_->fold;
    compiled->count = state_->count;
    compiled->nodes.resize(trie.size());
    for (std::size_t i = 0; i < trie.size(); ++i) {
        detail::TrieNode &source = trie[i];
        detail::Node &node = compiled->nodes[i];
        std::sort(source.edges.begin(), source.edges.end());

        node.fail = source.fail;
        node.output = source.output;
        node.id_begin = static_cast<std::uint32_t>(compiled->ids.size());
        node.id_count = static_cast<std::uint32_t>(source.ids.size());
        compiled->ids.insert(compiled->ids.end(), source.ids.begin(), source.ids.end());

        if (i == 0 || source.edges.size() >= detail::kDenseEdges) {
            node.dense_row = static_cast<std::uint32_t>(compiled->dense.size() / 256U);
            compiled->dense.resize(compiled->dense.size() + 256U, 0);
            for (const auto &[byte, child]: source.edges) {
                compiled->dense[(std::size_t{node.dense_row} * 256U) + byte] = child;
            }
            continue;
        }
        node.edge_begin = static_cast<std::uint32_t>(compiled->edge_bytes.size());
        node.edge_count = static_cast<std::uint32_t>(source.edges.size());
        for (const auto &[byte, child]: source.edges) {
            compiled->edge_bytes.push_back(byte);
            compiled->edge_targets.push_back(child);
        }
    }
    return LiteralSet(std::move(compiled));
}

LiteralSet::LiteralSet(std::shared_ptr<const detail::CompiledLiterals> compiled) noexcept :
    compiled_(std::move(compiled)) {}

std::uint32_t LiteralSet::size() const noexcept { return compiled_->count; }

std::size_t LiteralSet::memory_bytes() const noexcept {
    const detail::CompiledLiterals &c = *compiled_;
    return sizeof(c) + (c.nodes.size() * sizeof(detail::Node)) + c.edge_bytes.size() +
           ((c.edge_targets.size() + c.dense.size() + c.ids.size()) * sizeof(std::uint32_t));
}

bool LiteralSet::contains_any(std::string_view text) const noexcept {
    const detail::CompiledLiterals &c = *compiled_;
    std::uint32_t state = 0;
    for (const char raw: text) {
        state = c.step(state, c.fold[static_cast<unsigned char>(raw)]);
        if (c.report_from(state) != 0) {
            return true;
        }
    }
    return false;
}

void LiteralSet::scan(std::string_view text, MatchSet &hits) const noexcept {
    Cursor cursor;
    feed(text, cursor, hits);
}

void LiteralSet::feed(std::string_view chunk, Cursor &cursor, MatchSet &hits) const noexcept {
    const detail::CompiledLiterals &c = *compiled_;
    std::uint32_t state = cursor.node;
    for (const char raw: chunk) {
        state = c.step(state, c.fold[static_cast<unsigned char>(raw)]);
        c.report(state, hits);
    }
    cursor.node = state;
}

} // namespace fiber::regex
