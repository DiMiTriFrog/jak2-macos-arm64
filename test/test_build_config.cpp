#include <string>

#include <gtest/gtest.h>

#include "common/platform/BuildConfig.h"

TEST(BuildConfig, NamesMatchNumericSelection) {
  const std::string execution_mode = OG_EXECUTION_MODE_NAME;
  const std::string graphics_backend = OG_GRAPHICS_BACKEND_NAME;

  EXPECT_TRUE(execution_mode == "DYNAMIC" || execution_mode == "AOT");
  EXPECT_TRUE(graphics_backend == "OPENGL" || graphics_backend == "METAL");

  EXPECT_EQ((execution_mode == "AOT"), static_cast<bool>(OG_EXECUTION_MODE_AOT));
  EXPECT_EQ((execution_mode == "DYNAMIC"), static_cast<bool>(OG_EXECUTION_MODE_DYNAMIC));
  EXPECT_EQ((graphics_backend == "METAL"), static_cast<bool>(OG_GRAPHICS_BACKEND_METAL));
  EXPECT_EQ((graphics_backend == "OPENGL"), static_cast<bool>(OG_GRAPHICS_BACKEND_OPENGL));
}

TEST(BuildConfig, ExecutionModesAreExclusive) {
  EXPECT_NE(OG_EXECUTION_MODE_AOT, OG_EXECUTION_MODE_DYNAMIC);
  EXPECT_NE(OG_GRAPHICS_BACKEND_METAL, OG_GRAPHICS_BACKEND_OPENGL);
  EXPECT_EQ(OG_EXECUTION_MODE_AOT + OG_EXECUTION_MODE_DYNAMIC, 1);
  EXPECT_EQ(OG_GRAPHICS_BACKEND_METAL + OG_GRAPHICS_BACKEND_OPENGL, 1);
}

TEST(BuildConfig, ConstexprCapabilitiesMatchGeneratedSelection) {
  EXPECT_EQ(goal_platform::kAotCode, OG_EXECUTION_MODE_AOT != 0);
  EXPECT_EQ(goal_platform::kDynamicCode, OG_EXECUTION_MODE_DYNAMIC != 0);
  EXPECT_EQ(goal_platform::kMetal, OG_GRAPHICS_BACKEND_METAL != 0);
  EXPECT_EQ(goal_platform::kIos, OG_TARGET_IOS != 0);
  EXPECT_EQ(goal_platform::kMacos, OG_TARGET_MACOS != 0);
}
