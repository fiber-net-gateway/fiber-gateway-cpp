#ifndef FIBER_COMMON_SPECIAL_MEMBER_H
#define FIBER_COMMON_SPECIAL_MEMBER_H

#define FIBER_NON_COPYABLE(Type)                                                                                       \
    Type(const Type &) = delete;                                                                                       \
    Type &operator=(const Type &) = delete

#define FIBER_NON_MOVABLE(Type)                                                                                        \
    Type(Type &&) = delete;                                                                                            \
    Type &operator=(Type &&) = delete

#define FIBER_NON_COPYABLE_NON_MOVABLE(Type)                                                                           \
    FIBER_NON_COPYABLE(Type);                                                                                          \
    FIBER_NON_MOVABLE(Type)

#endif // FIBER_COMMON_SPECIAL_MEMBER_H
