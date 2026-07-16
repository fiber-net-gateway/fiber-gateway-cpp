#include <gtest/gtest.h>

#include <fiber/nacos/NacosClient.h>

TEST(NacosClientTest, HelloPrintsMessage) {
    testing::internal::CaptureStdout();
    fiber::nacos::hello();
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "hello\n");
}
