#include <fiber/nacos/NamingService.h>

#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::nacos {

InstanceRegistration::InstanceRegistration(std::shared_ptr<void> owner, void *context, UpdateFn update,
                                           SubscribeFn subscribe, CloseFn close) noexcept :
    owner_(std::move(owner)), context_(context), update_(update), subscribe_(subscribe), close_(close) {
    FIBER_ASSERT(owner_ != nullptr);
    FIBER_ASSERT(context_ != nullptr);
    FIBER_ASSERT(update_ != nullptr);
    FIBER_ASSERT(subscribe_ != nullptr);
    FIBER_ASSERT(close_ != nullptr);
}

InstanceRegistration::InstanceRegistration(InstanceRegistration &&other) noexcept :
    owner_(std::move(other.owner_)), context_(other.context_), update_(other.update_), subscribe_(other.subscribe_),
    close_(other.close_) {
    other.context_ = nullptr;
    other.update_ = nullptr;
    other.subscribe_ = nullptr;
    other.close_ = nullptr;
}

InstanceRegistration &InstanceRegistration::operator=(InstanceRegistration &&other) noexcept {
    if (this != &other) {
        close();
        owner_ = std::move(other.owner_);
        context_ = other.context_;
        update_ = other.update_;
        subscribe_ = other.subscribe_;
        close_ = other.close_;
        other.context_ = nullptr;
        other.update_ = nullptr;
        other.subscribe_ = nullptr;
        other.close_ = nullptr;
    }
    return *this;
}

InstanceRegistration::~InstanceRegistration() { close(); }

std::expected<void, NamingServiceError> InstanceRegistration::update(Instance instance) noexcept {
    FIBER_ASSERT(context_ != nullptr);
    return update_(context_, std::move(instance));
}

InstanceRegistration::StatusSubscriber InstanceRegistration::subscribe_status() const {
    FIBER_ASSERT(context_ != nullptr);
    return subscribe_(context_);
}

void InstanceRegistration::close() noexcept {
    if (!context_) {
        return;
    }
    close_(context_);
    context_ = nullptr;
    update_ = nullptr;
    subscribe_ = nullptr;
    close_ = nullptr;
    owner_.reset();
}

} // namespace fiber::nacos
