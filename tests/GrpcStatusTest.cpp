#include <gtest/gtest.h>

#include <string_view>

#include "grpc/GrpcStatus.h"

namespace {
using fiber::grpc::parse_grpc_status;

TEST(GrpcStatusTest, ParsesOkAndCodes) {
    EXPECT_EQ(parse_grpc_status("0"), 0);
    EXPECT_EQ(parse_grpc_status("5"), 5);
    EXPECT_EQ(parse_grpc_status("16"), 16);
}

TEST(GrpcStatusTest, EmptyYieldsZero) { EXPECT_EQ(parse_grpc_status(""), 0); }

TEST(GrpcStatusTest, RejectsNonNumericAsMinusOne) {
    // A non-numeric grpc-status must NOT be silently treated as OK (0); it is a
    // malformed value and should surface as a failure (-1) per the contract.
    EXPECT_EQ(parse_grpc_status("OK"), -1);
    EXPECT_EQ(parse_grpc_status(" 5"), -1);
    EXPECT_EQ(parse_grpc_status("+5"), -1);
    EXPECT_EQ(parse_grpc_status("5x"), -1);
    EXPECT_EQ(parse_grpc_status("5.0"), -1);
}

TEST(GrpcStatusTest, MaxIntIsAccepted) { EXPECT_EQ(parse_grpc_status("2147483647"), 2147483647); }

TEST(GrpcStatusTest, OverflowReturnsMinusOne) {
    // INT_MAX + 1 must not wrap (no signed-overflow UB): it is rejected as -1.
    EXPECT_EQ(parse_grpc_status("2147483648"), -1);
    EXPECT_EQ(parse_grpc_status("9999999999"), -1);
}
} // namespace
