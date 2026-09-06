#include <fiber/http/HttpAlpnHintTable.h>

namespace fiber::http {

void HttpAlpnHintTable::note(const HttpConnectionGroupKey &key, HttpProtocol protocol,
                             std::chrono::steady_clock::time_point now) noexcept {
    Slot &slot = slots_[slot_index(key.hash())];
    slot.hash = key.hash();
    slot.expire_at = now + ttl_;
    slot.protocol = protocol;
    slot.occupied = true;
}

std::optional<HttpProtocol> HttpAlpnHintTable::lookup(const HttpConnectionGroupKey &key,
                                                      std::chrono::steady_clock::time_point now) const noexcept {
    const Slot &slot = slots_[slot_index(key.hash())];
    if (!slot.occupied || slot.hash != key.hash() || slot.expire_at <= now) {
        return std::nullopt;
    }
    return slot.protocol;
}

void HttpAlpnHintTable::forget(const HttpConnectionGroupKey &key) noexcept {
    Slot &slot = slots_[slot_index(key.hash())];
    if (slot.occupied && slot.hash == key.hash()) {
        slot = Slot{};
    }
}

void HttpAlpnHintTable::clear() noexcept { slots_.fill(Slot{}); }

} // namespace fiber::http
