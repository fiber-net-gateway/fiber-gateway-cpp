#include "IoError.h"

#include <cerrno>

namespace fiber::common {

IoErr io_err_from_errno(int err) noexcept {
    switch (err) {
    case 0:
        return IoErr::None;
    case EAGAIN:
#if EAGAIN != EWOULDBLOCK
    case EWOULDBLOCK:
#endif
        return IoErr::WouldBlock;
    case EINTR:
        return IoErr::Interrupted;
    case EINVAL:
        return IoErr::Invalid;
    case EBADF:
        return IoErr::BadFd;
    case EBUSY:
        return IoErr::Busy;
    case ENOENT:
        return IoErr::NotFound;
    case EADDRINUSE:
        return IoErr::AddrInUse;
    case EADDRNOTAVAIL:
        return IoErr::AddrNotAvailable;
    case ECONNABORTED:
        return IoErr::ConnAborted;
    case ECONNRESET:
        return IoErr::ConnReset;
    case ECONNREFUSED:
        return IoErr::ConnRefused;
    case ETIMEDOUT:
        return IoErr::TimedOut;
    case ENOTCONN:
        return IoErr::NotConnected;
    case EALREADY:
        return IoErr::Already;
    case EACCES:
    case EPERM:
        return IoErr::Permission;
    case EPIPE:
        return IoErr::BrokenPipe;
    case ENOMEM:
        return IoErr::NoMem;
#ifdef ENOTSUP
    case ENOTSUP:
        return IoErr::NotSupported;
#endif
#if defined(EOPNOTSUPP) && (!defined(ENOTSUP) || EOPNOTSUPP != ENOTSUP)
    case EOPNOTSUPP:
        return IoErr::NotSupported;
#endif
    case ECANCELED:
        return IoErr::Canceled;
    default:
        return IoErr::Unknown;
    }
}

int io_err_to_errno(IoErr err) noexcept {
    switch (err) {
    case IoErr::None:
        return 0;
    case IoErr::WouldBlock:
        return EAGAIN;
    case IoErr::Interrupted:
        return EINTR;
    case IoErr::Invalid:
        return EINVAL;
    case IoErr::BadFd:
        return EBADF;
    case IoErr::Busy:
        return EBUSY;
    case IoErr::NotFound:
        return ENOENT;
    case IoErr::AddrInUse:
        return EADDRINUSE;
    case IoErr::AddrNotAvailable:
        return EADDRNOTAVAIL;
    case IoErr::ConnAborted:
        return ECONNABORTED;
    case IoErr::ConnReset:
        return ECONNRESET;
    case IoErr::ConnRefused:
        return ECONNREFUSED;
    case IoErr::TimedOut:
        return ETIMEDOUT;
    case IoErr::NotConnected:
        return ENOTCONN;
    case IoErr::Already:
        return EALREADY;
    case IoErr::Permission:
        return EACCES;
    case IoErr::BrokenPipe:
        return EPIPE;
    case IoErr::NoMem:
        return ENOMEM;
    case IoErr::NotSupported:
#ifdef ENOTSUP
        return ENOTSUP;
#else
        return EOPNOTSUPP;
#endif
    case IoErr::Canceled:
        return ECANCELED;
    case IoErr::Unknown:
    default:
        return EINVAL;
    }
}

std::string_view io_err_name(IoErr err) noexcept {
    switch (err) {
    case IoErr::None:
        return "none";
    case IoErr::WouldBlock:
        return "would_block";
    case IoErr::Interrupted:
        return "interrupted";
    case IoErr::Invalid:
        return "invalid";
    case IoErr::BadFd:
        return "bad_fd";
    case IoErr::Busy:
        return "busy";
    case IoErr::NotFound:
        return "not_found";
    case IoErr::AddrInUse:
        return "addr_in_use";
    case IoErr::AddrNotAvailable:
        return "addr_not_available";
    case IoErr::ConnAborted:
        return "conn_aborted";
    case IoErr::ConnReset:
        return "conn_reset";
    case IoErr::ConnRefused:
        return "conn_refused";
    case IoErr::TimedOut:
        return "timed_out";
    case IoErr::NotConnected:
        return "not_connected";
    case IoErr::Already:
        return "already";
    case IoErr::Permission:
        return "permission";
    case IoErr::BrokenPipe:
        return "broken_pipe";
    case IoErr::NoMem:
        return "no_mem";
    case IoErr::NotSupported:
        return "not_supported";
    case IoErr::Canceled:
        return "canceled";
    case IoErr::Unknown:
    default:
        return "unknown";
    }
}

} // namespace fiber::common
