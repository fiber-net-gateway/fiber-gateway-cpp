#ifndef FIBER_TESTS_HTTP_TRANSPORT_STUB_H
#define FIBER_TESTS_HTTP_TRANSPORT_STUB_H

#include "http/HttpTransport.h"

namespace fiber::test {

// Keeps protocol-focused transport fakes small. Tests that exercise callback
// or poll I/O override the relevant method explicitly.
class HttpTransportStub : public http::HttpTransport {
public:
    common::IoErr set_read_callback(ReadyCallback, void *) noexcept override { return common::IoErr::NotSupported; }

    common::IoErr set_write_callback(ReadyCallback, void *) noexcept override { return common::IoErr::NotSupported; }

    common::IoErr clear_read_callback(ReadyCallback, void *) noexcept override { return common::IoErr::NotSupported; }

    common::IoErr clear_write_callback(ReadyCallback, void *) noexcept override { return common::IoErr::NotSupported; }

    common::IoErr poll_read(void *, std::size_t, std::size_t &out, event::IoEvent &wait_event) noexcept override {
        return not_supported(out, wait_event);
    }

    common::IoErr poll_read_into(mem::IoBuf &, std::size_t &out, event::IoEvent &wait_event) noexcept override {
        return not_supported(out, wait_event);
    }

    common::IoErr poll_readv_into(mem::IoBufChain &, std::size_t &out, event::IoEvent &wait_event) noexcept override {
        return not_supported(out, wait_event);
    }

    common::IoErr poll_write(const void *, std::size_t, std::size_t &out,
                             event::IoEvent &wait_event) noexcept override {
        return not_supported(out, wait_event);
    }

    common::IoErr poll_write(mem::IoBuf &, std::size_t &out, event::IoEvent &wait_event) noexcept override {
        return not_supported(out, wait_event);
    }

    common::IoErr poll_writev(mem::IoBufChain &, std::size_t &out, event::IoEvent &wait_event) noexcept override {
        return not_supported(out, wait_event);
    }

private:
    static common::IoErr not_supported(std::size_t &out, event::IoEvent &wait_event) noexcept {
        out = 0;
        wait_event = event::IoEvent::None;
        return common::IoErr::NotSupported;
    }
};

} // namespace fiber::test

#endif // FIBER_TESTS_HTTP_TRANSPORT_STUB_H
