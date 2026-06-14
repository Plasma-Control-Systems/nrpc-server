#include <gtest/gtest.h>

#include <cstdint>

#include "nrpc/arena.hpp"

TEST(Arena, AllocatesAlignedMemoryAndResets) {
    nrpc::Arena arena(64);
    const auto first = arena.allocate(3, 1);
    const auto second = arena.allocate(8, 8);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(second->data()) % 8U, 0U);
    EXPECT_GT(arena.used(), 3U);

    arena.reset();
    EXPECT_EQ(arena.used(), 0U);
    EXPECT_TRUE(arena.allocate(64, 1).has_value());
}

TEST(Arena, ReportsInvalidAlignmentAndExhaustion) {
    nrpc::Arena arena(8);
    EXPECT_EQ(arena.allocate(1, 3).error(), nrpc::ArenaError::invalid_alignment);
    ASSERT_TRUE(arena.allocate(8, 1).has_value());
    EXPECT_EQ(arena.allocate(1, 1).error(), nrpc::ArenaError::out_of_memory);
}

