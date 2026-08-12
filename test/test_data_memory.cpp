#include <cstring>

#include <gtest/gtest.h>

#include "common/data_memory.h"

TEST(DataMemory, AllocatesWritableNonExecutableRegion) {
  auto region = data_memory::DataRegion::allocate(4096);
  ASSERT_NE(region.data(), nullptr);
  ASSERT_GE(region.size(), 4096u);

  auto* bytes = static_cast<unsigned char*>(region.data());
  bytes[0] = 0x5a;
  bytes[region.size() - 1] = 0xa5;
  EXPECT_EQ(bytes[0], 0x5a);
  EXPECT_EQ(bytes[region.size() - 1], 0xa5);
}

TEST(DataMemory, RejectsZeroSizedRegion) {
  EXPECT_THROW(data_memory::DataRegion::allocate(0), std::invalid_argument);
}
