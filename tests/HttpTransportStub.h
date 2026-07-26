#ifndef FIBER_TESTS_HTTP_TRANSPORT_STUB_H
#define FIBER_TESTS_HTTP_TRANSPORT_STUB_H

#include "http/HttpTransport.h"

namespace fiber::test {

// Keeps protocol-focused transport fakes small. Tests that exercise callback
// or poll I/O override the relevant method explicitly.
class HttpTransportStub : public http::HttpTransport {
public:
    common::IoErr set_read_callback(ReadyCallback callback, void *ctx) noexcept override {
        return set_callback(read_callback_, read_callback_ctx_, callback, ctx);
    }

    common::IoErr set_write_callback(ReadyCallback callback, void *ctx) noexcept override {
        return set_callback(write_callback_, write_callback_ctx_, callback, ctx);
    }

    common::IoErr set_terminal_callback(ReadyCallback callback, void *ctx) noexcept override {
        if (!callback) {
            return common::IoErr::Invalid;
        }
        if (terminal_) {
            callback(ctx, terminal_error_);
            return common::IoErr::None;
        }
        return set_callback(terminal_callback_, terminal_callback_ctx_, callback, ctx);
    }

    common::IoErr clear_read_callback(ReadyCallback callback, void *ctx) noexcept override {
        return clear_callback(read_callback_, read_callback_ctx_, callback, ctx);
    }

    common::IoErr clear_write_callback(ReadyCallback callback, void *ctx) noexcept override {
        return clear_callback(write_callback_, write_callback_ctx_, callback, ctx);
    }

    common::IoErr clear_terminal_callback(ReadyCallback callback, void *ctx) noexcept override {
        return clear_callback(terminal_callback_, terminal_callback_ctx_, callback, ctx);
    }

    [[nodiscard]] bool terminal() const noexcept override { return terminal_; }

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

protected:
    [[nodiscard]] bool terminal_callback_registered() const noexcept { return terminal_callback_ != nullptr; }

    void notify_read_ready(common::IoErr err = common::IoErr::None) noexcept {
        if (read_callback_) {
            read_callback_(read_callback_ctx_, err);
        }
    }

    void notify_write_ready(common::IoErr err = common::IoErr::None) noexcept {
        if (write_callback_) {
            write_callback_(write_callback_ctx_, err);
        }
    }

    void notify_terminal(common::IoErr err = common::IoErr::Unknown) noexcept {
        if (terminal_) {
            return;
        }
        terminal_ = true;
        terminal_error_ = err;
        ReadyCallback callback = terminal_callback_;
        void *ctx = terminal_callback_ctx_;
        terminal_callback_ = nullptr;
        terminal_callback_ctx_ = nullptr;
        if (callback) {
            callback(ctx, err);
        }
    }

private:
    static common::IoErr set_callback(ReadyCallback &slot, void *&slot_ctx, ReadyCallback callback,
                                      void *ctx) noexcept {
        if (!callback) {
            return common::IoErr::Invalid;
        }
        if (slot) {
            return common::IoErr::Busy;
        }
        slot = callback;
        slot_ctx = ctx;
        return common::IoErr::None;
    }

    static common::IoErr clear_callback(ReadyCallback &slot, void *&slot_ctx, ReadyCallback callback,
                                        void *ctx) noexcept {
        if (!callback) {
            return common::IoErr::Invalid;
        }
        if (slot == callback && slot_ctx == ctx) {
            slot = nullptr;
            slot_ctx = nullptr;
        }
        return common::IoErr::None;
    }

    static common::IoErr not_supported(std::size_t &out, event::IoEvent &wait_event) noexcept {
        out = 0;
        wait_event = event::IoEvent::None;
        return common::IoErr::NotSupported;
    }

    ReadyCallback read_callback_ = nullptr;
    void *read_callback_ctx_ = nullptr;
    ReadyCallback write_callback_ = nullptr;
    void *write_callback_ctx_ = nullptr;
    ReadyCallback terminal_callback_ = nullptr;
    void *terminal_callback_ctx_ = nullptr;
    common::IoErr terminal_error_ = common::IoErr::None;
    bool terminal_ = false;
};

} // namespace fiber::test

#endif // FIBER_TESTS_HTTP_TRANSPORT_STUB_H
