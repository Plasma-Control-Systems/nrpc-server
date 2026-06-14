#include <gtest/gtest.h>

#include "nrpc/version.hpp"

TEST(Project, HasExpectedVersion) {
    EXPECT_EQ(nrpc::version, "0.1.0");
}
