#ifndef FIBER_AI_SERVER_CONFIG_NODE_POOL_H
#define FIBER_AI_SERVER_CONFIG_NODE_POOL_H

#include <cstddef>
#include <expected>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <utility>

#include <common/Assert.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>

namespace fiber::ai_server {

// Owner-loop-only keyed node leases. The first acquire creates and starts one
// subscription node; later acquires share it. Dropping the last Ref removes the
// node from lookup before requesting stop, so late callbacks can reject the
// retired node with contains(). Parent generations can therefore acquire all
// candidate children transactionally and release the old generation only after
// the candidate is complete.
template<typename Node>
class ConfigNodePool final : public common::NonCopyable, public common::NonMovable {
public:
    using Key = typename Node::Key;
    using CreateError = typename Node::CreateError;
    using CreateFn = std::expected<std::shared_ptr<Node>, CreateError> (*)(void *, Key);

    class Ref final {
    public:
        Ref() noexcept = default;
        Ref(const Ref &) = delete;
        Ref &operator=(const Ref &) = delete;

        Ref(Ref &&other) noexcept : pool_(std::exchange(other.pool_, nullptr)), node_(std::move(other.node_)) {}

        Ref &operator=(Ref &&other) noexcept {
            if (this != &other) {
                reset();
                pool_ = std::exchange(other.pool_, nullptr);
                node_ = std::move(other.node_);
            }
            return *this;
        }

        ~Ref() { reset(); }

        [[nodiscard]] explicit operator bool() const noexcept { return node_ != nullptr; }

        [[nodiscard]] Node &node() const noexcept {
            FIBER_ASSERT(node_ != nullptr);
            return *node_;
        }

        [[nodiscard]] const Key &key() const noexcept { return node().key(); }

        void reset() noexcept {
            if (pool_ == nullptr) {
                FIBER_ASSERT(node_ == nullptr);
                return;
            }
            ConfigNodePool *pool = std::exchange(pool_, nullptr);
            std::shared_ptr<Node> node = std::move(node_);
            pool->release(node);
        }

    private:
        friend class ConfigNodePool;

        Ref(ConfigNodePool &pool, std::shared_ptr<Node> node) noexcept : pool_(&pool), node_(std::move(node)) {
            FIBER_ASSERT(node_ != nullptr);
        }

        ConfigNodePool *pool_ = nullptr;
        std::shared_ptr<Node> node_;
    };

    ConfigNodePool(void *create_context, CreateFn create) noexcept : create_context_(create_context), create_(create) {
        FIBER_ASSERT(create_context_ != nullptr);
        FIBER_ASSERT(create_ != nullptr);
    }

    ~ConfigNodePool() { FIBER_ASSERT(entries_.empty()); }

    [[nodiscard]] std::expected<Ref, CreateError> acquire(Key key) {
        auto existing = entries_.find(key);
        if (existing != entries_.end()) {
            FIBER_ASSERT(existing->second.references != std::numeric_limits<std::size_t>::max());
            ++existing->second.references;
            return Ref(*this, existing->second.node);
        }

        auto created = create_(create_context_, std::move(key));
        if (!created) {
            return std::unexpected(std::move(created.error()));
        }
        std::shared_ptr<Node> node = std::move(*created);
        FIBER_ASSERT(node != nullptr);
        auto [it, inserted] = entries_.emplace(node->key(), Entry{.node = node, .references = 1});
        FIBER_ASSERT(inserted);
        (void) it;
        node->start();
        return Ref(*this, std::move(node));
    }

    [[nodiscard]] bool contains(const Node &node) const noexcept {
        const auto it = entries_.find(node.key());
        return it != entries_.end() && it->second.node.get() == &node;
    }

    template<typename Fn>
    void for_each(Fn &&fn) {
        for (auto &[key, entry]: entries_) {
            (void) key;
            fn(*entry.node);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

private:
    struct Entry {
        std::shared_ptr<Node> node;
        std::size_t references = 0;
    };

    void release(const std::shared_ptr<Node> &node) noexcept {
        FIBER_ASSERT(node != nullptr);
        auto it = entries_.find(node->key());
        FIBER_ASSERT(it != entries_.end());
        FIBER_ASSERT(it->second.node == node);
        FIBER_ASSERT(it->second.references > 0);
        if (--it->second.references != 0) {
            return;
        }

        std::shared_ptr<Node> retiring = std::move(it->second.node);
        entries_.erase(it);
        retiring->request_stop();
    }

    void *create_context_ = nullptr;
    CreateFn create_ = nullptr;
    std::map<Key, Entry, std::less<>> entries_;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_CONFIG_NODE_POOL_H
