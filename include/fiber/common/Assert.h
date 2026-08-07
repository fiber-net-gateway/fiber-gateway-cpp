#ifndef FIBER_COMMON_ASSERT_H
#define FIBER_COMMON_ASSERT_H

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#if defined(__cpp_lib_stacktrace)
#include <iostream>
#include <stacktrace>
#endif

#if defined(__has_include)
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define FIBER_HAS_EXECINFO 1
#endif
#endif

#if !defined(FIBER_HAS_EXECINFO)
#define FIBER_HAS_EXECINFO 0
#endif

namespace fiber::common {

inline void print_stacktrace() noexcept {
#if defined(__cpp_lib_stacktrace) && !defined(__GLIBCXX__)
    std::fprintf(stderr, "stacktrace:\n");
    std::cerr << std::stacktrace::current() << '\n';
#elif FIBER_HAS_EXECINFO
    std::fprintf(stderr, "stacktrace:\n");
    void *frames[64];
    const int count = ::backtrace(frames, static_cast<int>(sizeof(frames) / sizeof(frames[0])));
    if (count > 0) {
        ::backtrace_symbols_fd(frames, count, STDERR_FILENO);
        std::fprintf(stderr, "\n");
    } else {
        std::fprintf(stderr, "  <empty>\n");
    }
#else
    std::fprintf(stderr, "stacktrace: unavailable\n");
#endif
}

[[noreturn]] inline void panic_assert(const char *expr, const char *file, int line, const char *func) {
    std::fprintf(stderr, "FIBER_ASSERT failed: %s\n  at %s:%d (%s)\n", expr, file, line, func);
    print_stacktrace();
    std::fflush(stderr);
    std::abort();
}

[[noreturn]] inline void panic_assert_msg(const char *expr, const char *message, const char *file, int line,
                                          const char *func) {
    std::fprintf(stderr, "FIBER_ASSERT failed: %s\n  message: %s\n  at %s:%d (%s)\n", expr, message, file, line, func);
    print_stacktrace();
    std::fflush(stderr);
    std::abort();
}

[[noreturn]] inline void panic_message(const char *message, const char *file, int line, const char *func) {
    std::fprintf(stderr, "FIBER_PANIC: %s\n  at %s:%d (%s)\n", message, file, line, func);
    print_stacktrace();
    std::fflush(stderr);
    std::abort();
}

} // namespace fiber::common

#define FIBER_ASSERT(expr)                                                                                             \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            ::fiber::common::panic_assert(#expr, __FILE__, __LINE__, __func__);                                        \
        }                                                                                                              \
    } while (false)

#define FIBER_ASSERT_MSG(expr, message)                                                                                \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            ::fiber::common::panic_assert_msg(#expr, (message), __FILE__, __LINE__, __func__);                         \
        }                                                                                                              \
    } while (false)

#define FIBER_PANIC(message)                                                                                           \
    do {                                                                                                               \
        ::fiber::common::panic_message((message), __FILE__, __LINE__, __func__);                                       \
    } while (false)

#endif // FIBER_COMMON_ASSERT_H
