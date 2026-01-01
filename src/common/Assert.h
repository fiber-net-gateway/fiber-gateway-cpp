#ifndef FIBER_COMMON_ASSERT_H
#define FIBER_COMMON_ASSERT_H

#include <cstdlib>
#include <cstdio>

#if defined(__cpp_lib_stacktrace)
#include <iostream>
#include <stacktrace>
#endif

namespace fiber::common {

[[noreturn]] inline void panic_assert(const char *expr, const char *file, int line, const char *func) {
    std::fprintf(stderr, "FIBER_ASSERT failed: %s\n  at %s:%d (%s)\n", expr, file, line, func);
#if defined(__cpp_lib_stacktrace)
    std::fprintf(stderr, "stacktrace:\n");
    std::cerr << std::stacktrace::current() << '\n';
#else
    std::fprintf(stderr, "stacktrace: unavailable\n");
#endif
    std::fflush(stderr);
    std::abort();
}

[[noreturn]] inline void panic_assert_msg(const char *expr,
                                          const char *message,
                                          const char *file,
                                          int line,
                                          const char *func) {
    std::fprintf(stderr,
                 "FIBER_ASSERT failed: %s\n  message: %s\n  at %s:%d (%s)\n",
                 expr,
                 message,
                 file,
                 line,
                 func);
#if defined(__cpp_lib_stacktrace)
    std::fprintf(stderr, "stacktrace:\n");
    std::cerr << std::stacktrace::current() << '\n';
#else
    std::fprintf(stderr, "stacktrace: unavailable\n");
#endif
    std::fflush(stderr);
    std::abort();
}

[[noreturn]] inline void panic_message(const char *message, const char *file, int line, const char *func) {
    std::fprintf(stderr, "FIBER_PANIC: %s\n  at %s:%d (%s)\n", message, file, line, func);
#if defined(__cpp_lib_stacktrace)
    std::fprintf(stderr, "stacktrace:\n");
    std::cerr << std::stacktrace::current() << '\n';
#else
    std::fprintf(stderr, "stacktrace: unavailable\n");
#endif
    std::fflush(stderr);
    std::abort();
}

} // namespace fiber::common

#define FIBER_ASSERT(expr) \
    do { \
        if (!(expr)) { \
            ::fiber::common::panic_assert(#expr, __FILE__, __LINE__, __func__); \
        } \
    } while (false)

#define FIBER_ASSERT_MSG(expr, message) \
    do { \
        if (!(expr)) { \
            ::fiber::common::panic_assert_msg(#expr, (message), __FILE__, __LINE__, __func__); \
        } \
    } while (false)

#define FIBER_PANIC(message) \
    do { \
        ::fiber::common::panic_message((message), __FILE__, __LINE__, __func__); \
    } while (false)

#endif // FIBER_COMMON_ASSERT_H
