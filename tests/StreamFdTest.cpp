#include <gtest/gtest.h>

#include <csignal>
#include <cstdlib>
#include <future>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

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
        fiber::async::spawn(group.at(1), [&]() -> fiber::async::DetachedTask {
            const char payload[] = "ping";
            auto result = stream->try_write(payload, sizeof(payload) - 1U);
            result_promise.set_value(result ? fiber::common::IoErr::None : result.error());
            co_return;
        });

        const fiber::common::IoErr err = result_future.get();
        std::promise<bool> close_promise;
        auto close_future = close_promise.get_future();
        fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
            const bool terminal = stream->terminal();
            stream->close();
            delete stream;
            close_promise.set_value(terminal);
            co_return;
        });
        const bool terminal = close_future.get();
        group.stop();
        group.join();
        _exit(err == fiber::common::IoErr::BrokenPipe && !terminal ? 0 : 32);
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

TEST(StreamFdTest, CrossLoopTryWriteReturnsBrokenPipeWithoutTouchingOwnerPoller) {
    EXPECT_EQ(run_cross_loop_broken_pipe_child(), 0);
}
