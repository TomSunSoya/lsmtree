#include <gtest/gtest.h>

import lsm.smoke;

TEST(ModulesSmokeTest, ImportedFunctionIsUsable) { EXPECT_EQ(42, lsm::smokeAnswer()); }
