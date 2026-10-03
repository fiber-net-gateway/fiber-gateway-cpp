#ifndef FIBER_COMMON_SPECIAL_MEMBER_H
#define FIBER_COMMON_SPECIAL_MEMBER_H

// NOLINTBEGIN(bugprone-macro-parentheses)
// Type is used in declaration position (as a type name), where parenthesizing
// the argument is syntactically invalid — not an expression-context omission.
#define FIBER_NON_COPYABLE(Type)                                                                                       \
    Type(const Type &) = delete;                                                                                       \
    Type &operator=(const Type &) = delete

#define FIBER_NON_MOVABLE(Type)                                                                                        \
    Type(Type &&) = delete;                                                                                            \
    Type &operator=(Type &&) = delete

#define FIBER_NON_COPYABLE_NON_MOVABLE(Type)                                                                           \
    FIBER_NON_COPYABLE(Type);                                                                                          \
    FIBER_NON_MOVABLE(Type)
// NOLINTEND(bugprone-macro-parentheses)

#endif // FIBER_COMMON_SPECIAL_MEMBER_H
