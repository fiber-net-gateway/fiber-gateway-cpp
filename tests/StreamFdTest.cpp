#include <gtest/gtest.h>

#include <csignal>
#include <cstdlib>
#include <future>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/net/detail/StreamFd.h>

namespace {

int run_broken_pipe_child(bool use_writev) {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
        return 10;
    }
    ::close(fds[1]);
    fds[1] = -1;

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        return 11;
    }
    if (pid == 0) {
        fiber::event::EventLoopGroup group(1);
        group.start();

        // Restore SIG_DFL *after* the loop is constructed: the framework ignores
        // SIGPIPE globally (EventLoop ctor), so this child must opt back into the
        // default disposition to keep the SIGPIPE tripwire (StreamFd must suppress
        // it via MSG_NOSIGNAL and return BrokenPipe instead).
        (void) ::signal(SIGPIPE, SIG_DFL);

        std::promise<fiber::common::IoErr> result_promise;
        auto result_future = result_promise.get_future();
        fiber::async::spawn(group.at(0), [&, fd = fds[0]]() mutable -> fiber::async::DetachedTask {
            fiber::net::detail::StreamFd stream(group.at(0), fd);
            fiber::common::IoErr err = fiber::common::IoErr::Unknown;
            if (use_writev) {
                const char left[] = "pi";
                const char right[] = "ng";
                struct iovec iov[2]{};
                iov[0].iov_base = const_cast<char *>(left);
                iov[0].iov_len = sizeof(left) - 1U;
                iov[1].iov_base = const_cast<char *>(right);
                iov[1].iov_len = sizeof(right) - 1U;
                auto result = stream.try_writev(iov, 2);
                err = result ? fiber::common::IoErr::None : result.error();
            } else {
                const char payload[] = "ping";
                auto result = stream.try_write(payload, sizeof(payload) - 1U);
                err = result ? fiber::common::IoErr::None : result.error();
            }
            result_promise.set_value(err);
            stream.close();
            fiber::event::EventLoop::current().stop();
            co_return;
        });

        const fiber::common::IoErr err = result_future.get();
        group.join();
        int exit_code = err == fiber::common::IoErr::BrokenPipe ? 0 : 20;
        _exit(exit_code);
    }

    ::close(fds[0]);
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        return 12;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 13;
}

int run_cross_loop_broken_pipe_child() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
        return 30;
    }
    ::close(fds[1]);
    fds[1] = -1;

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        return 31;
    }
    if (pid == 0) {
        fiber::event::EventLoopGroup group(2);
        group.start();
        (void) ::signal(SIGPIPE, SIG_DFL);

        auto *stream = new fiber::net::detail::StreamFd(group.at(0), fds[0]);
        std::promise<fiber::common::IoErr> result_promise;
        auto result_future = result_promise.get_future();
        fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
            // Cross-loop I/O goes through the handover protocol: detach on the
            // owning loop, adopt on the target loop, then operate there.
            if (stream->detach_for_handover() != fiber::common::IoErr::None) {
                result_promise.set_value(fiber::common::IoErr::Invalid);
                co_return;
            }
            fiber::async::spawn(group.at(1), [&]() -> fiber::async::DetachedTask {
                stream->adopt_loop(group.at(1));
                const char payload[] = "ping";
                auto result = stream->try_write(payload, sizeof(payload) - 1U);
                result_promise.set_value(result ? fiber::common::IoErr::None : result.error());
                co_return;
            });
            co_return;
        });

        const fiber::common::IoErr err = result_future.get();
        std::promise<bool> close_promise;
        auto close_future = close_promise.get_future();
        fiber::async::spawn(group.at(1), [&]() -> fiber::async::DetachedTask {
            const bool terminal = stream->terminal();
            stream->close();
            delete stream;
            close_promise.set_value(terminal);
            co_return;
        });
        const bool terminal = close_future.get();
        group.stop();
        group.join();
        // A fatal write error marks the stream terminal on the operating loop.
        _exit(err == fiber::common::IoErr::BrokenPipe && terminal ? 0 : 32);
    }

    ::close(fds[0]);
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        return 33;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 34;
}

} // namespace

TEST(StreamFdTest, TryWriteReturnsBrokenPipeInsteadOfSigpipe) { EXPECT_EQ(run_broken_pipe_child(false), 0); }

TEST(StreamFdTest, TryWritevReturnsBrokenPipeInsteadOfSigpipe) { EXPECT_EQ(run_broken_pipe_child(true), 0); }

TEST(StreamFdTest, CrossLoopWriteAfterHandoverReturnsBrokenPipe) { EXPECT_EQ(run_cross_loop_broken_pipe_child(), 0); }

namespace {
using namespace std::chrono_literals;
using fiber::common::IoErr;
using fiber::event::IoEvent;
struct EtWatchdog {
    fiber::event::EventLoop &loop;
    fiber::event::EventLoop::TimerEntry timer{};
    static void expire(EtWatchdog *self) noexcept {
        ADD_FAILURE() << "ET operation failed to make progress";
        self->loop.stop();
    }
    void arm() { loop.post_at<EtWatchdog, &EtWatchdog::timer, &EtWatchdog::expire>(loop.now() + 1s, *this); }
    void finish() {
        if (timer.is_in_heap()) {
            loop.cancel<EtWatchdog, &EtWatchdog::timer>(*this);
        }
        loop.stop();
    }
};
} // namespace

TEST(StreamFdTest, ShortReadSuppressesEmptyProbeAndNextPacketWakesReader) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
    fiber::event::EventLoop loop;
    EtWatchdog watchdog{loop};
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        watchdog.arm();
        fiber::net::detail::StreamFd stream(loop, fds[0]);
        EXPECT_EQ(::send(fds[1], "a", 1, 0), 1);
        char data[16]{};
        auto first = stream.try_read(data, sizeof(data));
        EXPECT_TRUE(first && *first == 1);
        // The short read proved the receive side drained; no probe needed.
        EXPECT_FALSE(stream.read_ready());
        EXPECT_EQ(::send(fds[1], "b", 1, 0), 1);
        auto second = co_await stream.read(data, sizeof(data), 500ms);
        EXPECT_TRUE(second && *second == 1);
        EXPECT_EQ(data[0], 'b');
        stream.close();
        ::close(fds[1]);
        watchdog.finish();
    });
    loop.run();
}

TEST(StreamFdTest, FullReadKeepsRemainingBytesRunnableWithoutAnotherEdge) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
    fiber::event::EventLoop loop;
    EtWatchdog watchdog{loop};
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        watchdog.arm();
        fiber::net::detail::StreamFd stream(loop, fds[0]);
        EXPECT_EQ(::send(fds[1], "abcd", 4, 0), 4);
        auto ready = co_await stream.wait_readable(500ms);
        EXPECT_TRUE(ready);
        char data[2]{};
        auto first = stream.try_read(data, 2);
        EXPECT_TRUE(first && *first == 2);
        auto still_ready = co_await stream.wait_readable(500ms);
        EXPECT_TRUE(still_ready);
        auto rest = stream.try_read(data, 2);
        EXPECT_TRUE(rest && *rest == 2);
        EXPECT_EQ(data[0], 'c');
        stream.close();
        ::close(fds[1]);
        watchdog.finish();
    });
    loop.run();
}

TEST(StreamFdTest, DataThenHalfCloseDrainsEofAndStillAllowsReply) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
    fiber::event::EventLoop loop;
    EtWatchdog watchdog{loop};
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        watchdog.arm();
        fiber::net::detail::StreamFd stream(loop, fds[0]);
        EXPECT_EQ(::send(fds[1], "tail", 4, 0), 4);
        EXPECT_EQ(::shutdown(fds[1], SHUT_WR), 0);
        auto ready = co_await stream.wait_readable(500ms);
        EXPECT_TRUE(ready);
        char data[16]{};
        auto tail = co_await stream.read(data, sizeof(data), 500ms);
        EXPECT_TRUE(tail && *tail == 4);
        auto eof = co_await stream.read(data, sizeof(data), 500ms);
        EXPECT_TRUE(eof && *eof == 0);
        EXPECT_FALSE(stream.terminal());
        auto written = co_await stream.write("ok", 2, 500ms);
        EXPECT_TRUE(written && *written == 2);
        EXPECT_EQ(::recv(fds[1], data, sizeof(data), 0), 2);
        stream.close();
        ::close(fds[1]);
        watchdog.finish();
    });
    loop.run();
}

TEST(StreamFdTest, HalfCloseShortReadThenWaitCompletesWithoutNewEdge) {
    // RDHUP was consumed together with the data edge. A short read parks the
    // readiness at Blocked, and no further edge can arrive — the known
    // half-close must complete the next read wait (reads can no longer block)
    // instead of timing out, all the way to EOF.
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
    fiber::event::EventLoop loop;
    EtWatchdog watchdog{loop};
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        watchdog.arm();
        fiber::net::detail::StreamFd stream(loop, fds[0]);
        EXPECT_EQ(stream.ensure_state_observation(), fiber::common::IoErr::None);
        EXPECT_EQ(::send(fds[1], "abcdefgh", 8, 0), 8);
        EXPECT_EQ(::shutdown(fds[1], SHUT_WR), 0);
        auto ready = co_await stream.wait_readable(500ms);
        EXPECT_TRUE(ready);
        char data[100]{};
        auto short_read = stream.try_read(data, sizeof(data));
        EXPECT_TRUE(short_read && *short_read == 8);
        EXPECT_TRUE(stream.peer_hangup());
        EXPECT_FALSE(stream.read_ready());
        auto still_readable = co_await stream.wait_readable(500ms);
        EXPECT_TRUE(still_readable);
        auto eof_read = stream.try_read(data, sizeof(data));
        EXPECT_TRUE(eof_read && *eof_read == 0);
        EXPECT_TRUE(stream.eof());
        EXPECT_FALSE(stream.terminal());
        stream.close();
        ::close(fds[1]);
        watchdog.finish();
    });
    loop.run();
}

TEST(StreamFdTest, HupWithBufferedDataLetsWaitsDrainRemainingThenEof) {
    // A full peer close reports a Terminal hint alongside the data edge. The
    // hint must not turn read waits into an error while buffered bytes are
    // still readable: data first, then EOF.
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
    fiber::event::EventLoop loop;
    EtWatchdog watchdog{loop};
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        watchdog.arm();
        fiber::net::detail::StreamFd stream(loop, fds[0]);
        EXPECT_EQ(stream.ensure_state_observation(), fiber::common::IoErr::None);
        EXPECT_EQ(::send(fds[1], "abcdefgh", 8, 0), 8);
        EXPECT_EQ(::close(fds[1]), 0);
        fds[1] = -1;
        auto ready = co_await stream.wait_readable(500ms);
        EXPECT_TRUE(ready);
        char data[16]{};
        auto first = stream.try_read(data, 4);
        EXPECT_TRUE(first && *first == 4);
        EXPECT_TRUE(stream.terminal());
        // The terminal hint must not fail this wait: the direction is still
        // readable with buffered bytes.
        auto drain_wait = co_await stream.wait_readable(500ms);
        EXPECT_TRUE(drain_wait);
        auto rest = stream.try_read(data, 8);
        EXPECT_TRUE(rest && *rest == 4);
        // Now the short read parked the readiness at Blocked with the hint
        // still recorded; the wait must complete anyway.
        auto after = co_await stream.wait_readable(500ms);
        EXPECT_TRUE(after);
        auto eof_read = stream.try_read(data, sizeof(data));
        EXPECT_TRUE(eof_read && *eof_read == 0);
        EXPECT_TRUE(stream.eof());
        stream.close();
        watchdog.finish();
    });
    loop.run();
}

TEST(StreamFdTest, ZeroLengthReadDoesNotConsumeReadiness) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
    fiber::event::EventLoop loop;
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        fiber::net::detail::StreamFd stream(loop, fds[0]);
        EXPECT_EQ(::send(fds[1], "x", 1, 0), 1);
        auto empty = stream.try_read(nullptr, 0);
        EXPECT_TRUE(empty && *empty == 0);
        char byte{};
        auto read = stream.try_read(&byte, 1);
        EXPECT_TRUE(read && *read == 1);
        EXPECT_EQ(byte, 'x');
        stream.close();
        ::close(fds[1]);
        loop.stop();
        co_return;
    });
    loop.run();
}

namespace {

bool create_connected_tcp_sockets(int &server_fd, int &client_fd) {
    server_fd = -1;
    client_fd = -1;
    int listener_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener_fd < 0) {
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
        ::listen(listener_fd, 1) != 0) {
        (void) ::close(listener_fd);
        return false;
    }

    socklen_t address_len = sizeof(address);
    if (::getsockname(listener_fd, reinterpret_cast<sockaddr *>(&address), &address_len) != 0) {
        (void) ::close(listener_fd);
        return false;
    }

    client_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (client_fd < 0 || ::connect(client_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        if (client_fd >= 0) {
            (void) ::close(client_fd);
            client_fd = -1;
        }
        (void) ::close(listener_fd);
        return false;
    }

    server_fd = ::accept4(listener_fd, nullptr, nullptr, SOCK_CLOEXEC);
    (void) ::close(listener_fd);
    if (server_fd < 0) {
        (void) ::close(client_fd);
        client_fd = -1;
        return false;
    }
    return true;
}

struct TerminalCallbackResult {
    int calls = 0;
    IoErr err = IoErr::Invalid;
};

void record_terminal_callback(void *raw_ctx, IoErr err) noexcept {
    auto *ctx = static_cast<TerminalCallbackResult *>(raw_ctx);
    ++ctx->calls;
    ctx->err = err;
}

} // namespace

TEST(StreamFdTest, PeerWriteHalfCloseIsReadableButNotTerminal) {
    int server_fd = -1;
    int client_fd = -1;
    ASSERT_TRUE(create_connected_tcp_sockets(server_fd, client_fd));

    fiber::event::EventLoop loop;
    fiber::net::detail::StreamFd stream(loop, server_fd);
    TerminalCallbackResult terminal_result;
    bool terminal_before_close = true;
    bool peer_closed_observed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        // The terminal subscription also arms the read-hangup observation.
        EXPECT_EQ(stream.set_terminal_callback(&record_terminal_callback, &terminal_result), IoErr::None);
        EXPECT_EQ(::shutdown(client_fd, SHUT_WR), 0);
        for (int i = 0; i < 100 && !stream.peer_closed(); ++i) {
            co_await fiber::async::sleep(1ms);
        }
        peer_closed_observed = stream.peer_closed();
        char data[8]{};
        auto eof = co_await stream.read(data, sizeof(data), 500ms);
        EXPECT_TRUE(eof && *eof == 0);
        terminal_before_close = stream.terminal();
        EXPECT_EQ(terminal_result.calls, 0);
        stream.close();
        ::close(client_fd);
        client_fd = -1;
        loop.stop();
        co_return;
    });

    loop.run();
    if (client_fd >= 0) {
        (void) ::close(client_fd);
    }
    EXPECT_TRUE(peer_closed_observed);
    EXPECT_FALSE(terminal_before_close);
    // close() completes the still-registered terminal subscription.
    EXPECT_EQ(terminal_result.calls, 1);
    EXPECT_EQ(terminal_result.err, IoErr::Canceled);
}

TEST(StreamFdTest, PeerResetMarksTerminalAndNotifiesOnce) {
    int server_fd = -1;
    int client_fd = -1;
    ASSERT_TRUE(create_connected_tcp_sockets(server_fd, client_fd));

    fiber::event::EventLoop loop;
    fiber::net::detail::StreamFd stream(loop, server_fd);
    TerminalCallbackResult terminal_result;
    bool reset_sent = false;
    bool terminal_before_close = false;
    int calls_before_close = 0;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        EXPECT_EQ(stream.set_terminal_callback(&record_terminal_callback, &terminal_result), IoErr::None);
        linger reset_linger{1, 0};
        if (::setsockopt(client_fd, SOL_SOCKET, SO_LINGER, &reset_linger, sizeof(reset_linger)) == 0) {
            reset_sent = true;
            (void) ::close(client_fd);
            client_fd = -1;
        }
        for (int i = 0; i < 100 && terminal_result.calls == 0; ++i) {
            co_await fiber::async::sleep(1ms);
        }
        terminal_before_close = stream.terminal();
        calls_before_close = terminal_result.calls;
        stream.close();
        if (client_fd >= 0) {
            (void) ::close(client_fd);
            client_fd = -1;
        }
        loop.stop();
        co_return;
    });

    loop.run();
    if (client_fd >= 0) {
        (void) ::close(client_fd);
    }
    EXPECT_TRUE(reset_sent);
    EXPECT_EQ(calls_before_close, 1);
    EXPECT_TRUE(terminal_before_close);
    EXPECT_EQ(terminal_result.calls, 1);
}
