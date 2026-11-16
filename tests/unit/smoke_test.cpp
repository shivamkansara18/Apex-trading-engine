#include "apex/core/version.hpp"

#include <gtest/gtest.h>

TEST(ProjectSmoke, ReportsInitialVersion) {
    EXPECT_EQ(apex::version(), "0.5.0");
}
