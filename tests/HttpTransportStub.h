#ifndef FIBER_TESTS_HTTP_TRANSPORT_STUB_H
#define FIBER_TESTS_HTTP_TRANSPORT_STUB_H

#include <fiber/http/HttpTransport.h>

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

    // Loop handover is a real-fd concept; fakes never move loops.
    common::IoErr detach_for_handover() noexcept override { return common::IoErr::NotSupported; }
    common::IoErr adopt_loop(event::EventLoop &) noexcept override { return common::IoErr::None; }

    [[nodiscard]] bool terminal() const noexcept override { return terminal_; }

    // Fakes announce reads through notify_read_ready; one holding data it
    // reads without an announcement reports it here.
    [[nodiscard]] bool read_ready() const noexcept override { return false; }

    // Fakes are synchronous: one try_* operation per call, never WouldBlock.
    [[nodiscard]] common::IoResult<size_t> try_readv(size_t, mem::IoBufChain &) noexcept override {
        return std::unexpected(common::IoErr::NotSupported);
    }

    [[nodiscard]] common::IoResult<size_t> try_writev(mem::IoBufChain &) noexcept override {
        return std::unexpected(common::IoErr::NotSupported);
    }

    fiber::async::Task<common::IoResult<size_t>> readv(size_t size, mem::IoBufChain &out,
                                                       std::chrono::milliseconds) override {
        co_return try_readv(size, out);
    }

    fiber::async::Task<common::IoResult<size_t>> writev(mem::IoBufChain &buf, std::chrono::milliseconds) override {
        co_return try_writev(buf);
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
