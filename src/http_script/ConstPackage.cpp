#include "ConstPackage.h"

#include "ScriptExchangeCtx.h"

#include "../common/mem/BufPool.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fiber::http_script {
namespace {

constexpr std::size_t kTypeCount = static_cast<std::size_t>(ConstType::Count);
constexpr std::size_t kMinBucketCount = 4;

std::size_t type_index(ConstType type) noexcept { return static_cast<std::size_t>(type); }

unsigned char normalize_byte(unsigned char ch) noexcept {
    if (ch == '-') {
        return '_';
    }
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<unsigned char>(ch - 'A' + 'a');
    }
    return ch;
}

std::string normalize_name(std::string_view name) {
    std::string result;
    result.reserve(name.size());
    for (unsigned char ch: name) {
        result.push_back(static_cast<char>(normalize_byte(ch)));
    }
    return result;
}

std::uint64_t normalized_hash(std::string_view name) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch: name) {
        hash ^= normalize_byte(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool normalized_equal(std::string_view normalized, std::string_view input) noexcept {
    if (normalized.size() != input.size()) {
        return false;
    }
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        if (static_cast<unsigned char>(normalized[i]) != normalize_byte(static_cast<unsigned char>(input[i]))) {
            return false;
        }
    }
    return true;
}

const char *debug_name(ConstType type) noexcept {
    switch (type) {
        case ConstType::Path:
            return "$path";
        case ConstType::Query:
            return "$query";
        case ConstType::Header:
            return "$header";
        case ConstType::Cookie:
            return "$cookie";
        case ConstType::Context:
            return "$context";
        case ConstType::Count:
            break;
    }
    return "$constant";
}

std::size_t bucket_count_for(std::size_t entry_count) noexcept {
    if (entry_count == 0) {
        return 0;
    }
    std::size_t bucket_count = kMinBucketCount;
    while (bucket_count / 2 < entry_count) {
        bucket_count <<= 1U;
    }
    return bucket_count;
}

} // namespace

struct ConstPackage::Ref {
    const ConstPackage *owner = nullptr;
    ConstIndex index = kInvalidConstIndex;
    std::uint32_t name_size = 0;

    [[nodiscard]] char *name_data() noexcept { return reinterpret_cast<char *>(this + 1); }
    [[nodiscard]] const char *name_data() const noexcept { return reinterpret_cast<const char *>(this + 1); }
    [[nodiscard]] std::string_view name() const noexcept { return {name_data(), name_size}; }
};

struct ConstPackage::Storage {
    // Compiled scripts retain pointers to Ref. Names share the same stable pool allocation
    // immediately after each Ref; build-only HostCallables are not retained by the package.
    fiber::mem::BufPool ref_pool;
    std::array<std::uint32_t, kTypeCount + 1> entry_offsets{};
    std::array<std::uint32_t, kTypeCount + 1> bucket_offsets{};
    std::unique_ptr<Entry[]> entries;
    std::unique_ptr<ConstIndex[]> buckets;
    std::size_t size = 0;
};

struct ConstPackage::Builder::State {
    struct Record {
        Ref *ref = nullptr;
        fiber::script::Library::HostCallable callable;
    };

    State() : package(new ConstPackage) {}

    std::unique_ptr<ConstPackage> package;
    fiber::mem::BufPool record_pool;
    std::array<std::unordered_map<std::string_view, Record *>, kTypeCount> lookup;
    std::array<std::vector<Record *>, kTypeCount> records;
    std::size_t size = 0;
};

static_assert(sizeof(ConstPackage::Entry) == 16);

ConstPackage::ConstPackage() : storage_(std::make_unique<Storage>()) {}

ConstPackage::~ConstPackage() = default;

ConstPackage::Builder::Builder() : state_(std::make_unique<State>()) {}

ConstPackage::Builder::~Builder() = default;

const fiber::script::Library::HostCallable *ConstPackage::Builder::add_constant(ConstType type, std::string_view name) {
    if (!state_ || type == ConstType::Count || name.size() > std::numeric_limits<std::uint32_t>::max() ||
        state_->size >= kInvalidConstIndex) {
        return nullptr;
    }

    const std::size_t ti = type_index(type);
    std::string normalized = normalize_name(name);
    auto found = state_->lookup[ti].find(normalized);
    if (found != state_->lookup[ti].end()) {
        return &found->second->callable;
    }

    if (normalized.size() > std::numeric_limits<std::size_t>::max() - sizeof(Ref)) {
        return nullptr;
    }
    void *ref_memory = state_->package->storage_->ref_pool.alloc(sizeof(Ref) + normalized.size(), alignof(Ref));
    if (ref_memory == nullptr) {
        return nullptr;
    }
    auto *ref = new (ref_memory) Ref{};
    ref->owner = state_->package.get();
    ref->name_size = static_cast<std::uint32_t>(normalized.size());
    if (!normalized.empty()) {
        std::memcpy(ref->name_data(), normalized.data(), normalized.size());
    }

    void *record_memory = state_->record_pool.alloc(sizeof(State::Record), alignof(State::Record));
    if (record_memory == nullptr) {
        return nullptr;
    }
    auto *record = new (record_memory) State::Record{};
    record->ref = ref;
    record->callable.kind = fiber::script::Library::HostCallable::Kind::SyncConstant;
    record->callable.userdata = ref;
    record->callable.constant = &ConstPackage::constant_fn;
    record->callable.debug_name = debug_name(type);

    state_->lookup[ti].emplace(ref->name(), record);
    state_->records[ti].push_back(record);
    ++state_->size;
    return &record->callable;
}

std::shared_ptr<const ConstPackage> ConstPackage::Builder::build() {
    if (!state_) {
        return {};
    }

    std::size_t bucket_count = 0;
    for (const auto &records: state_->records) {
        const std::size_t type_buckets = bucket_count_for(records.size());
        if (type_buckets > std::numeric_limits<std::uint32_t>::max() - bucket_count) {
            return {};
        }
        bucket_count += type_buckets;
    }

    auto entries = std::unique_ptr<Entry[]>(state_->size == 0 ? nullptr : new (std::nothrow) Entry[state_->size]);
    if (state_->size != 0 && !entries) {
        return {};
    }
    auto buckets =
            std::unique_ptr<ConstIndex[]>(bucket_count == 0 ? nullptr : new (std::nothrow) ConstIndex[bucket_count]);
    if (bucket_count != 0 && !buckets) {
        return {};
    }
    if (bucket_count != 0) {
        std::fill_n(buckets.get(), bucket_count, kInvalidConstIndex);
    }

    ConstIndex entry_index = 0;
    std::uint32_t bucket_begin = 0;
    for (std::size_t ti = 0; ti < kTypeCount; ++ti) {
        const auto &records = state_->records[ti];
        const std::size_t type_bucket_count = bucket_count_for(records.size());
        state_->package->storage_->entry_offsets[ti] = entry_index;
        state_->package->storage_->bucket_offsets[ti] = bucket_begin;
        const std::uint32_t bucket_mask =
                type_bucket_count == 0 ? 0 : static_cast<std::uint32_t>(type_bucket_count - 1);

        for (State::Record *record: records) {
            record->ref->index = entry_index;
            entries[entry_index] = Entry{
                    .name_data = record->ref->name_data(),
                    .index = entry_index,
                    .name_size = record->ref->name_size,
            };

            std::size_t slot = static_cast<std::size_t>(normalized_hash(record->ref->name())) & bucket_mask;
            std::size_t step = 1;
            while (buckets[bucket_begin + slot] != kInvalidConstIndex) {
                slot = (slot + step) & bucket_mask;
                ++step;
            }
            buckets[bucket_begin + slot] = entry_index;
            ++entry_index;
        }
        bucket_begin += static_cast<std::uint32_t>(type_bucket_count);
    }
    state_->package->storage_->entry_offsets[kTypeCount] = entry_index;
    state_->package->storage_->bucket_offsets[kTypeCount] = bucket_begin;

    state_->package->storage_->entries = std::move(entries);
    state_->package->storage_->buckets = std::move(buckets);
    state_->package->storage_->size = state_->size;
    std::shared_ptr<ConstPackage> package(std::move(state_->package));
    state_.reset();
    return std::shared_ptr<const ConstPackage>(std::move(package));
}

fiber::script::AbiResult ConstPackage::constant_fn(void *userdata,
                                                   const fiber::script::Library::HostCallFrame &frame) noexcept {
    const auto *ref = static_cast<const Ref *>(userdata);
    auto *context = static_cast<ScriptExchangeCtx *>(frame.attach);
    if (ref == nullptr || context == nullptr || ref->owner == nullptr || ref->index == kInvalidConstIndex) {
        return fiber::script::AbiResult::abort(fiber::script::ScriptAbortReason::InvalidState);
    }
    return context->constant(ref->owner, ref->index);
}

std::size_t ConstPackage::size() const noexcept { return storage_->size; }

std::span<const ConstPackage::Entry> ConstPackage::entries(ConstType type) const noexcept {
    if (type == ConstType::Count) {
        return {};
    }
    const std::size_t ti = type_index(type);
    const std::uint32_t begin = storage_->entry_offsets[ti];
    const std::uint32_t end = storage_->entry_offsets[ti + 1];
    if (begin == end) {
        return {};
    }
    return {storage_->entries.get() + begin, end - begin};
}

ConstIndex ConstPackage::find(ConstType type, std::string_view name) const noexcept {
    if (type == ConstType::Count) {
        return kInvalidConstIndex;
    }
    const std::size_t ti = type_index(type);
    const std::uint32_t bucket_begin = storage_->bucket_offsets[ti];
    const std::uint32_t bucket_count = storage_->bucket_offsets[ti + 1] - bucket_begin;
    if (bucket_count == 0) {
        return kInvalidConstIndex;
    }
    const std::uint32_t bucket_mask = bucket_count - 1;

    std::size_t slot = static_cast<std::size_t>(normalized_hash(name)) & bucket_mask;
    std::size_t step = 1;
    for (std::size_t probed = 0; probed < bucket_count; ++probed) {
        const ConstIndex index = storage_->buckets[bucket_begin + slot];
        if (index == kInvalidConstIndex) {
            return kInvalidConstIndex;
        }
        if (normalized_equal(storage_->entries[index].name(), name)) {
            return index;
        }
        slot = (slot + step) & bucket_mask;
        ++step;
    }
    return kInvalidConstIndex;
}

const void *ConstPackage::identity() const noexcept { return this; }

} // namespace fiber::http_script
