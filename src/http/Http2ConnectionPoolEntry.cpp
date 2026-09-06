#include <fiber/http/Http2ConnectionPoolEntry.h>

namespace fiber::http {
Http2ConnectionPoolEntry::~Http2ConnectionPoolEntry() {
    FIBER_ASSERT(!maintenance_posted_);
    FIBER_ASSERT(!ready_hook_.linked() && !group_hook_.linked() && !idle_hook_.linked());
    destroy_connection();
}
void Http2ConnectionPoolEntry::construct_connection(event::EventLoop &loop, Http2Connection::Options options) noexcept {
    FIBER_ASSERT(!has_connection_);
    std::construct_at(reinterpret_cast<Http2ClientConnection *>(conn_storage_), loop, options);
    has_connection_ = true;
}
void Http2ConnectionPoolEntry::destroy_connection() noexcept {
    if (has_connection_) {
        if (closed_observer_.linked) {
            connection().close_gate().remove_observer(closed_observer_);
        }
        std::destroy_at(&connection());
        has_connection_ = false;
    }
}
Http2ClientConnection &Http2ConnectionPoolEntry::connection() noexcept {
    FIBER_ASSERT(has_connection_);
    return *std::launder(reinterpret_cast<Http2ClientConnection *>(conn_storage_));
}
} // namespace fiber::http
