#include <fiber/http_script/ConstPackage.h>

#include <fiber/http_script/ScriptExchangeCtx.h>

#include <fiber/common/mem/BufPool.h>

#include <array>
#include <cstring>
#include <new>
#include <utility>

namespace fiber::http_script {
namespace {

unsigned char normalize_byte(unsigned char ch) noexcept {
    if (ch == '-') {
        return '_';
    }
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<unsigned char>(ch - 'A' + 'a');
    }
    return ch;
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
        case ConstType::Count:
            break;
    }
    return "$constant";
}

} // namespace

std::uint64_t const_name_hash(std::string_view name) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch: name) {
        hash ^= normalize_byte(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool const_name_equal(std::string_view normalized, std::string_view probe) noexcept {
    if (normalized.size() != probe.size()) {
        return false;
    }
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        if (static_cast<unsigned char>(normalized[i]) != normalize_byte(static_cast<unsigned char>(probe[i]))) {
            return false;
        }
    }
    return true;
}

// Compile-time constant identity embedded in a package. The inline name bytes are the
// normalized key; `hash` is const_name_hash over them, precomputed so the request-time
// lookup never rehashes. Compiled scripts retain pointers to Ref for their lifetime.
struct ConstPackage::Ref {
    std::uint64_t hash = 0;
    std::uint32_t name_size = 0;
    bool sealed = false;
    ConstType type = ConstType::Count;

    [[nodiscard]] char *name_data() noexcept { return reinterpret_cast<char *>(this + 1); }
    [[nodiscard]] const char *name_data() const noexcept { return reinterpret_cast<const char *>(this + 1); }
    [[nodiscard]] std::string_view name() const noexcept { return {name_data(), name_size}; }
};

struct ConstPackage::Storage {
    // Refs (with inline names) are bump-allocated here and never move; compiled scripts
    // hold raw pointers into this pool. The package therefore cannot be copied or moved.
    fiber::mem::BufPool ref_pool;
    std::size_t size = 0;
};

struct ConstPackage::Builder::State {
    struct Record {
        Ref *ref = nullptr;
        Record *next = nullptr;
        fiber::script::Library::HostCallable callable;
    };

    // The package shared_ptr (with its control block) is allocated once up front so
    // build() never allocates and is genuinely noexcept. Raw `new` (not make_shared)
    // because the private constructor is only accessible from this nesting context.
    // Construction failure propagates to Builder's constructor catch.
    State() : package(std::shared_ptr<ConstPackage>(new ConstPackage)) {}

    std::shared_ptr<ConstPackage> package;
    fiber::mem::BufPool record_pool;
    // Pool-allocated singly-linked list; dedup scans it linearly. Compile-time only, with
    // tens of entries per package, so O(n) per add beats keeping a throwing hash index.
    Record *records = nullptr;
    std::size_t size = 0;
};

ConstPackage::ConstPackage() : storage_(std::make_unique<Storage>()) {}

ConstPackage::~ConstPackage() = default;

ConstPackage::Builder::Builder() noexcept {
    // The only allocating step: State + ConstPackage + Storage + the shared control
    // block. OOM leaves state_ null — an unusable builder whose add_constant returns
    // nullptr and build returns an empty package (reported as failed constant resolution).
    try {
        state_ = std::make_unique<State>();
    } catch (const std::bad_alloc &) {
        state_.reset();
    }
}

ConstPackage::Builder::~Builder() = default;

const fiber::script::Library::HostCallable *ConstPackage::Builder::add_constant(ConstType type,
                                                                                std::string_view name) noexcept {
    if (!state_ || type == ConstType::Count || name.size() > kMaxConstNameLength) {
        return nullptr;
    }

    // Normalize into a stack buffer; normalization is byte-for-byte, so the length is
    // unchanged and the capped name needs no heap allocation (builder stays non-throwing).
    std::array<char, kMaxConstNameLength> buffer{};
    for (std::size_t i = 0; i < name.size(); ++i) {
        buffer[i] = static_cast<char>(normalize_byte(static_cast<unsigned char>(name[i])));
    }
    const std::string_view normalized{buffer.data(), name.size()};

    for (const State::Record *record = state_->records; record != nullptr; record = record->next) {
        if (record->ref->type == type && record->ref->name() == normalized) {
            return &record->callable;
        }
    }

    void *ref_memory = state_->package->storage_->ref_pool.alloc(sizeof(Ref) + normalized.size(), alignof(Ref));
    if (ref_memory == nullptr) {
        return nullptr;
    }
    auto *ref = new (ref_memory) Ref{};
    ref->type = type;
    ref->name_size = static_cast<std::uint32_t>(normalized.size());
    ref->hash = const_name_hash(normalized);
    if (!normalized.empty()) {
        std::memcpy(ref->name_data(), normalized.data(), normalized.size());
    }

    void *record_memory = state_->record_pool.alloc(sizeof(State::Record), alignof(State::Record));
    if (record_memory == nullptr) {
        return nullptr; // the orphaned Ref stays in the package pool, harmless
    }
    auto *record = new (record_memory) State::Record{};
    record->ref = ref;
    record->next = state_->records;
    state_->records = record;
    record->callable.kind = fiber::script::Library::HostCallable::Kind::SyncConstant;
    record->callable.userdata = ref;
    record->callable.constant = &ConstPackage::constant_fn;
    record->callable.debug_name = debug_name(type);

    ++state_->size;
    return &record->callable;
}

std::shared_ptr<const ConstPackage> ConstPackage::Builder::build() noexcept {
    if (!state_) {
        return {};
    }

    // Seal every Ref so constant_fn can reject any callable that escaped an unsealed
    // builder (execution is only reachable through scripts, which require build()).
    for (State::Record *record = state_->records; record != nullptr; record = record->next) {
        record->ref->sealed = true;
    }
    state_->package->storage_->size = state_->size;
    std::shared_ptr<const ConstPackage> package = std::move(state_->package);
    state_.reset();
    return package;
}

fiber::script::AbiResult ConstPackage::constant_fn(void *userdata,
                                                   const fiber::script::Library::HostCallFrame &frame) noexcept {
    const auto *ref = static_cast<const Ref *>(userdata);
    auto *context = static_cast<ScriptExchangeCtx *>(frame.attach);
    if (ref == nullptr || !ref->sealed || context == nullptr) {
        return fiber::script::AbiResult::abort(fiber::script::ScriptAbortReason::InvalidState);
    }
    return context->resolve_constant(ref->type, ref->name(), ref->hash);
}

std::size_t ConstPackage::size() const noexcept { return storage_->size; }

} // namespace fiber::http_script
