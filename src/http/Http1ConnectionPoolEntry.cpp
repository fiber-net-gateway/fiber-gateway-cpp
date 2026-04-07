#include "Http1ConnectionPoolEntry.h"

#include "Http1ConnectionPoolCore.h"

namespace fiber::http {

void Http1ConnectionPoolEntry::post_remote_return(Http1ConnectionPoolCore &home_core,
                                                  const Http1ConnectionGroupKey &key) noexcept {
    arm_remote_return(home_core, key);
    home_core.loop().post<Http1ConnectionPoolEntry,
                          &Http1ConnectionPoolEntry::return_notify_,
                          &Http1ConnectionPoolEntry::run_remote_return>(*this);
}

void Http1ConnectionPoolEntry::run_remote_return(Http1ConnectionPoolEntry *entry) {
    FIBER_ASSERT(entry != nullptr);
    Http1ConnectionPoolCore &home_core = entry->remote_return_home_core();
    Http1ConnectionGroupKey key = entry->remote_return_key();
    entry->clear_remote_return_state();
    home_core.accept_returned_entry(*entry, key);
}

} // namespace fiber::http
