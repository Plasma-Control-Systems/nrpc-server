#include <gtest/gtest.h>

#include <memory>

#include "nrpc/resource_manager.hpp"

namespace {

class MockHandle {
public:
    explicit MockHandle(int& close_count) : close_count_(close_count) {}
    MockHandle(const MockHandle&) = delete;
    MockHandle& operator=(const MockHandle&) = delete;
    void close() noexcept { ++close_count_; }

private:
    int& close_count_;
};

}  // namespace

TEST(ResourceManager, FindsAndCleansClientHandles) {
    int close_count = 0;
    nrpc::ResourceManager manager;
    EXPECT_TRUE(manager.register_handle(10, 1, std::make_unique<MockHandle>(close_count)));
    EXPECT_TRUE(manager.register_handle(10, 2, std::make_unique<MockHandle>(close_count)));
    EXPECT_TRUE(manager.register_handle(11, 1, std::make_unique<MockHandle>(close_count)));
    auto retained = manager.find<MockHandle>(10, 1);
    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(manager.size(), 3U);

    EXPECT_EQ(manager.cleanup_client(10), 2U);
    EXPECT_EQ(close_count, 1);
    retained.reset();
    EXPECT_EQ(close_count, 2);
    EXPECT_EQ(manager.size(), 1U);
}

