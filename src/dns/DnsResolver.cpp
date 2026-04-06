#include "DnsResolver.h"

#include <algorithm>
#include <array>

#include "../async/Spawn.h"
#include "../async/WaitGroup.h"
#include "../common/Assert.h"
#include "../net/UdpSocket.h"

namespace fiber::dns {

namespace {

struct FamilyQueryState {
    common::IoErr err = common::IoErr::None;
    ResolveStatus status = ResolveStatus::ServerFailure;
    ResolveResult result{};
};

bool is_literal_allowed(net::IpAddress address, AddressPolicy policy) noexcept {
    switch (policy) {
        case AddressPolicy::V4Only:
            return address.is_v4();
        case AddressPolicy::V6Only:
            return address.is_v6();
        case AddressPolicy::V6First:
        case AddressPolicy::V4First:
            return true;
    }
    return false;
}

std::uint8_t status_rank(ResolveStatus status) noexcept {
    switch (status) {
        case ResolveStatus::FormatError:
            return 6;
        case ResolveStatus::ServerFailure:
            return 5;
        case ResolveStatus::NotImplemented:
            return 4;
        case ResolveStatus::Refused:
            return 3;
        case ResolveStatus::NxDomain:
            return 2;
        case ResolveStatus::NoData:
            return 1;
        case ResolveStatus::Success:
            return 0;
    }
    return 0;
}

const FamilyQueryState *preferred_family(AddressPolicy policy,
                                         const FamilyQueryState &v4,
                                         const FamilyQueryState &v6) noexcept {
    switch (policy) {
        case AddressPolicy::V6First:
        case AddressPolicy::V6Only:
            return &v6;
        case AddressPolicy::V4First:
        case AddressPolicy::V4Only:
            return &v4;
    }
    return &v6;
}

const FamilyQueryState *fallback_family(AddressPolicy policy,
                                        const FamilyQueryState &v4,
                                        const FamilyQueryState &v6) noexcept {
    return preferred_family(policy, v4, v6) == &v6 ? &v4 : &v6;
}

const FamilyQueryState *pick_status_family(AddressPolicy policy,
                                           const FamilyQueryState &v4,
                                           const FamilyQueryState &v6) noexcept {
    const FamilyQueryState *first = preferred_family(policy, v4, v6);
    const FamilyQueryState *second = fallback_family(policy, v4, v6);
    if (first->err == common::IoErr::None && second->err == common::IoErr::None) {
        if (status_rank(first->status) >= status_rank(second->status)) {
            return first;
        }
        return second;
    }
    if (first->err == common::IoErr::None) {
        return first;
    }
    return second;
}

} // namespace

bool AddressResolveResult::init(Options options) noexcept {
    release();
    options_ = options;
    if (options.max_name_storage == 0) {
        return false;
    }
    if (options.max_records != 0) {
        records_ = std::make_unique<net::IpAddress[]>(options.max_records);
        if (!records_) {
            release();
            return false;
        }
    }
    name_storage_ = std::make_unique<char[]>(options.max_name_storage);
    if (!name_storage_) {
        release();
        return false;
    }
    clear();
    return true;
}

void AddressResolveResult::release() noexcept {
    records_.reset();
    name_storage_.reset();
    options_ = {};
    clear();
}

void AddressResolveResult::clear() noexcept {
    canonical_name_ = {};
    record_count_ = 0;
    v4_count_ = 0;
    v6_count_ = 0;
    expire_at_ = {};
}

bool AddressResolveResult::valid() const noexcept {
    return name_storage_ != nullptr && (options_.max_records == 0 || records_ != nullptr);
}

common::IoErr AddressResolveResult::assign_positive(std::string_view canonical_name,
                                                    const net::IpAddress *records,
                                                    std::uint16_t count,
                                                    std::uint16_t v4_count,
                                                    std::uint16_t v6_count,
                                                    std::chrono::steady_clock::time_point expire_at) noexcept {
    if ((count != 0 && records == nullptr) || count > options_.max_records || count != v4_count + v6_count) {
        return count > options_.max_records ? common::IoErr::NoMem : common::IoErr::Invalid;
    }
    common::IoErr err = assign_canonical(canonical_name);
    if (err != common::IoErr::None) {
        return err;
    }
    for (std::uint16_t i = 0; i < count; ++i) {
        records_[i] = records[i];
    }
    record_count_ = count;
    v4_count_ = v4_count;
    v6_count_ = v6_count;
    expire_at_ = expire_at;
    return common::IoErr::None;
}

common::IoErr AddressResolveResult::assign_canonical(std::string_view canonical_name) noexcept {
    if (canonical_name.size() > options_.max_name_storage) {
        return common::IoErr::NoMem;
    }
    std::copy(canonical_name.begin(), canonical_name.end(), name_storage_.get());
    canonical_name_ = std::string_view(name_storage_.get(), canonical_name.size());
    return common::IoErr::None;
}

bool EndpointResolveResult::init(Options options) noexcept {
    release();
    options_ = options;
    if (options.max_name_storage == 0) {
        return false;
    }
    if (options.max_records != 0) {
        records_ = std::make_unique<net::SocketAddress[]>(options.max_records);
        if (!records_) {
            release();
            return false;
        }
    }
    name_storage_ = std::make_unique<char[]>(options.max_name_storage);
    if (!name_storage_) {
        release();
        return false;
    }
    clear();
    return true;
}

void EndpointResolveResult::release() noexcept {
    records_.reset();
    name_storage_.reset();
    options_ = {};
    clear();
}

void EndpointResolveResult::clear() noexcept {
    canonical_name_ = {};
    record_count_ = 0;
    expire_at_ = {};
}

bool EndpointResolveResult::valid() const noexcept {
    return name_storage_ != nullptr && (options_.max_records == 0 || records_ != nullptr);
}

common::IoErr EndpointResolveResult::assign_positive(std::string_view canonical_name,
                                                     const net::SocketAddress *records,
                                                     std::uint16_t count,
                                                     std::chrono::steady_clock::time_point expire_at) noexcept {
    if ((count != 0 && records == nullptr) || count > options_.max_records) {
        return count > options_.max_records ? common::IoErr::NoMem : common::IoErr::Invalid;
    }
    common::IoErr err = assign_canonical(canonical_name);
    if (err != common::IoErr::None) {
        return err;
    }
    for (std::uint16_t i = 0; i < count; ++i) {
        records_[i] = records[i];
    }
    record_count_ = count;
    expire_at_ = expire_at;
    return common::IoErr::None;
}

common::IoErr EndpointResolveResult::assign_canonical(std::string_view canonical_name) noexcept {
    if (canonical_name.size() > options_.max_name_storage) {
        return common::IoErr::NoMem;
    }
    std::copy(canonical_name.begin(), canonical_name.end(), name_storage_.get());
    canonical_name_ = std::string_view(name_storage_.get(), canonical_name.size());
    return common::IoErr::None;
}

bool DnsResolver::init(DnsResolverLocal &local, Options options) noexcept {
    release();
    if (!local.valid() || options.family_result_options.max_name_storage == 0) {
        return false;
    }
    local_ = &local;
    options_ = options;
    return true;
}

void DnsResolver::release() noexcept {
    local_ = nullptr;
    options_ = {};
}

bool DnsResolver::valid() const noexcept {
    return local_ != nullptr && local_->valid();
}

event::EventLoop &DnsResolver::loop() const noexcept {
    FIBER_ASSERT(local_ != nullptr);
    return local_->loop();
}

async::Task<common::IoResult<ResolveStatus>> DnsResolver::resolve_host(std::string_view host,
                                                                       AddressResolveResult &out) noexcept {
    co_return co_await resolve_host(host, options_.default_policy, out);
}

async::Task<common::IoResult<ResolveStatus>> DnsResolver::resolve_host(std::string_view host,
                                                                       AddressPolicy policy,
                                                                       AddressResolveResult &out) noexcept {
    FIBER_ASSERT(local_ != nullptr);
    FIBER_ASSERT(local_->loop().in_loop());

    out.clear();
    if (!valid()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }
    if (!out.valid()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    net::IpAddress literal{};
    if (net::IpAddress::parse(host, literal)) {
        if (!is_literal_allowed(literal, policy)) {
            common::IoErr err = out.assign_canonical(host);
            if (err != common::IoErr::None) {
                co_return std::unexpected(err);
            }
            co_return ResolveStatus::NoData;
        }
        common::IoErr err = out.assign_positive(host, &literal, 1, literal.is_v4() ? 1 : 0, literal.is_v6() ? 1 : 0,
                                                std::chrono::steady_clock::time_point::max());
        if (err != common::IoErr::None) {
            co_return std::unexpected(err);
        }
        co_return ResolveStatus::Success;
    }

    auto run_one = [this, host](std::uint16_t qtype, FamilyQueryState &state, async::WaitGroup &wait_group)
        -> async::DetachedTask {
        QuestionSpec question{};
        question.name = host;
        question.type = qtype;
        question.dns_class = static_cast<std::uint16_t>(RecordClass::IN);

        auto result = co_await local_->resolve(question, state.result);
        if (result) {
            state.status = *result;
        } else {
            state.err = result.error();
        }
        wait_group.done();
    };

    auto finish_single = [&out](const FamilyQueryState &state) -> common::IoResult<ResolveStatus> {
        if (state.err != common::IoErr::None) {
            return std::unexpected(state.err);
        }
        if (state.status != ResolveStatus::Success) {
            common::IoErr err = out.assign_canonical(state.result.canonical_name());
            if (err != common::IoErr::None) {
                return std::unexpected(err);
            }
            return state.status;
        }

        std::uint16_t v4_count = 0;
        std::uint16_t v6_count = 0;
        for (std::uint16_t i = 0; i < state.result.record_count(); ++i) {
            v4_count += state.result.records()[i].is_v4() ? 1 : 0;
            v6_count += state.result.records()[i].is_v6() ? 1 : 0;
        }
        common::IoErr err = out.assign_positive(state.result.canonical_name(),
                                                state.result.records(),
                                                state.result.record_count(),
                                                v4_count,
                                                v6_count,
                                                state.result.expire_at());
        if (err != common::IoErr::None) {
            return std::unexpected(err);
        }
        return ResolveStatus::Success;
    };

    FamilyQueryState v4{};
    FamilyQueryState v6{};

    if ((policy == AddressPolicy::V4Only || policy == AddressPolicy::V4First) &&
        !v4.result.init(options_.family_result_options)) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    if ((policy == AddressPolicy::V6Only || policy == AddressPolicy::V6First) &&
        !v6.result.init(options_.family_result_options)) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    if ((policy == AddressPolicy::V4First || policy == AddressPolicy::V6First) &&
        (!v4.result.valid() || !v6.result.valid())) {
        if (!v4.result.valid() && !v4.result.init(options_.family_result_options)) {
            co_return std::unexpected(common::IoErr::NoMem);
        }
        if (!v6.result.valid() && !v6.result.init(options_.family_result_options)) {
            co_return std::unexpected(common::IoErr::NoMem);
        }
    }

    if (policy == AddressPolicy::V4Only) {
        QuestionSpec question{};
        question.name = host;
        question.type = static_cast<std::uint16_t>(RecordType::A);
        question.dns_class = static_cast<std::uint16_t>(RecordClass::IN);
        auto result = co_await local_->resolve(question, v4.result);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        v4.status = *result;
        co_return finish_single(v4);
    }

    if (policy == AddressPolicy::V6Only) {
        QuestionSpec question{};
        question.name = host;
        question.type = static_cast<std::uint16_t>(RecordType::AAAA);
        question.dns_class = static_cast<std::uint16_t>(RecordClass::IN);
        auto result = co_await local_->resolve(question, v6.result);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        v6.status = *result;
        co_return finish_single(v6);
    }

    async::WaitGroup wait_group;
    wait_group.add(2);
    async::spawn([&]() { return run_one(static_cast<std::uint16_t>(RecordType::A), v4, wait_group); });
    async::spawn([&]() { return run_one(static_cast<std::uint16_t>(RecordType::AAAA), v6, wait_group); });
    co_await wait_group.join();

    const bool v4_success = v4.err == common::IoErr::None && v4.status == ResolveStatus::Success;
    const bool v6_success = v6.err == common::IoErr::None && v6.status == ResolveStatus::Success;

    if (v4_success || v6_success) {
        const FamilyQueryState *canonical_source = preferred_family(policy, v4, v6);
        if (!(canonical_source->err == common::IoErr::None && canonical_source->status == ResolveStatus::Success)) {
            canonical_source = fallback_family(policy, v4, v6);
        }

        std::array<net::IpAddress, 32> stack_records{};
        std::unique_ptr<net::IpAddress[]> heap_records{};
        const std::uint16_t total_count =
            (v4_success ? v4.result.record_count() : 0) + (v6_success ? v6.result.record_count() : 0);
        net::IpAddress *merged = nullptr;
        if (total_count <= stack_records.size()) {
            merged = stack_records.data();
        } else {
            heap_records = std::make_unique<net::IpAddress[]>(total_count);
            if (!heap_records) {
                co_return std::unexpected(common::IoErr::NoMem);
            }
            merged = heap_records.get();
        }

        std::uint16_t index = 0;
        auto append_records = [&](const FamilyQueryState &state) noexcept {
            for (std::uint16_t i = 0; i < state.result.record_count(); ++i) {
                merged[index++] = state.result.records()[i];
            }
        };

        if (policy == AddressPolicy::V6First) {
            if (v6_success) {
                append_records(v6);
            }
            if (v4_success) {
                append_records(v4);
            }
        } else {
            if (v4_success) {
                append_records(v4);
            }
            if (v6_success) {
                append_records(v6);
            }
        }

        std::chrono::steady_clock::time_point expire_at = std::chrono::steady_clock::time_point::max();
        if (v4_success) {
            expire_at = std::min(expire_at, v4.result.expire_at());
        }
        if (v6_success) {
            expire_at = std::min(expire_at, v6.result.expire_at());
        }

        common::IoErr err = out.assign_positive(canonical_source->result.canonical_name(),
                                                merged,
                                                total_count,
                                                v4_success ? v4.result.record_count() : 0,
                                                v6_success ? v6.result.record_count() : 0,
                                                expire_at);
        if (err != common::IoErr::None) {
            co_return std::unexpected(err);
        }
        co_return ResolveStatus::Success;
    }

    const FamilyQueryState *status_source = pick_status_family(policy, v4, v6);
    if (status_source->err != common::IoErr::None) {
        co_return std::unexpected(status_source->err);
    }
    common::IoErr err = out.assign_canonical(status_source->result.canonical_name());
    if (err != common::IoErr::None) {
        co_return std::unexpected(err);
    }
    co_return status_source->status;
}

bool AddressResolver::init(DnsResolver &resolver, Options options) noexcept {
    release();
    if (!resolver.valid() || options.address_result_options.max_name_storage == 0) {
        return false;
    }
    resolver_ = &resolver;
    options_ = options;
    return true;
}

void AddressResolver::release() noexcept {
    resolver_ = nullptr;
    options_ = {};
}

bool AddressResolver::valid() const noexcept {
    return resolver_ != nullptr && resolver_->valid();
}

event::EventLoop &AddressResolver::loop() const noexcept {
    FIBER_ASSERT(resolver_ != nullptr);
    return resolver_->loop();
}

async::Task<common::IoResult<ResolveStatus>> AddressResolver::resolve(std::string_view host,
                                                                      std::uint16_t port,
                                                                      EndpointResolveResult &out) noexcept {
    co_return co_await resolve(host, port, options_.default_policy, out);
}

async::Task<common::IoResult<ResolveStatus>> AddressResolver::resolve(std::string_view host,
                                                                      std::uint16_t port,
                                                                      AddressPolicy policy,
                                                                      EndpointResolveResult &out) noexcept {
    FIBER_ASSERT(resolver_ != nullptr);
    FIBER_ASSERT(resolver_->loop().in_loop());

    out.clear();
    if (!valid()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }
    if (!out.valid()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    AddressResolveResult addresses;
    if (!addresses.init(options_.address_result_options)) {
        co_return std::unexpected(common::IoErr::NoMem);
    }

    auto resolved = co_await resolver_->resolve_host(host, policy, addresses);
    if (!resolved) {
        co_return std::unexpected(resolved.error());
    }
    if (*resolved != ResolveStatus::Success) {
        common::IoErr err = out.assign_canonical(addresses.canonical_name());
        if (err != common::IoErr::None) {
            co_return std::unexpected(err);
        }
        co_return *resolved;
    }

    std::array<net::SocketAddress, 32> stack_records{};
    std::unique_ptr<net::SocketAddress[]> heap_records{};
    net::SocketAddress *records = nullptr;
    if (addresses.record_count() <= stack_records.size()) {
        records = stack_records.data();
    } else {
        heap_records = std::make_unique<net::SocketAddress[]>(addresses.record_count());
        if (!heap_records) {
            co_return std::unexpected(common::IoErr::NoMem);
        }
        records = heap_records.get();
    }

    for (std::uint16_t i = 0; i < addresses.record_count(); ++i) {
        records[i] = net::SocketAddress(addresses.records()[i], port);
    }

    common::IoErr err = out.assign_positive(addresses.canonical_name(), records, addresses.record_count(),
                                            addresses.expire_at());
    if (err != common::IoErr::None) {
        co_return std::unexpected(err);
    }
    co_return ResolveStatus::Success;
}

} // namespace fiber::dns
