#include "goalc/emitter/Relocation.h"

#include "gtest/gtest.h"

using namespace emitter;

namespace {

u32 read_word(const std::vector<u8>& code, size_t off) {
  u32 w;
  memcpy(&w, code.data() + off, 4);
  return w;
}

// Build a code buffer with one instruction word set to `base`.
std::vector<u8> make_code(u32 base) {
  std::vector<u8> code(8, 0);
  memcpy(code.data(), &base, 4);
  return code;
}

}  // namespace

TEST(RelocationModel, SerializeRoundTrip) {
  for (int t = 1; t <= 8; t++) {
    Relocation r{static_cast<RelocationType>(t), 12345, -0x12345678};
    auto data = serialize_relocation(r);
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(data->size(), 13);
    auto back = deserialize_relocation(*data);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->type, r.type);
    EXPECT_EQ(back->position, r.position);
    EXPECT_EQ(back->addend, r.addend);
  }
}

TEST(RelocationModel, UnknownTypeRejected) {
  EXPECT_FALSE(serialize_relocation({static_cast<RelocationType>(0), 0, 0}).has_value());
  EXPECT_FALSE(serialize_relocation({static_cast<RelocationType>(9), 0, 0}).has_value());
  EXPECT_FALSE(serialize_relocation({static_cast<RelocationType>(255), 0, 0}).has_value());
  // deserialize: unknown tag and truncated payloads rejected.
  std::vector<u8> bad = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_FALSE(deserialize_relocation(bad).has_value());
  std::vector<u8> bad2 = {9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_FALSE(deserialize_relocation(bad2).has_value());
  std::vector<u8> short_data = {1, 2, 3};
  EXPECT_FALSE(deserialize_relocation(short_data).has_value());
  // empty payload (e.g. an unresolved symbol produced nothing) is rejected.
  EXPECT_FALSE(deserialize_relocation({}).has_value());
}

TEST(RelocationModel, Branch26) {
  auto code = make_code(0x14000000);
  s64 pc = 0x1000;
  // forward +8 bytes -> imm26 = 2
  EXPECT_TRUE(apply_relocation(RelocationType::Arm64Branch26, code.data(), 0, pc, pc + 8, 0, nullptr));
  EXPECT_EQ(read_word(code, 0), 0x14000000u | 2u);
  // backward -8 bytes -> imm26 = -2 (two's complement in 26 bits)
  EXPECT_TRUE(apply_relocation(RelocationType::Arm64Branch26, code.data(), 0, pc, pc - 8, 0, nullptr));
  EXPECT_EQ(read_word(code, 0), 0x14000000u | (0x3FFFFFFu - 1));  // 0x3FFFFFE
  // max positive: imm26 = 2^25 - 1
  s64 max_target = pc + 4 * ((1LL << 25) - 1);
  std::string err;
  EXPECT_TRUE(apply_relocation(RelocationType::Arm64Branch26, code.data(), 0, pc, max_target, 0, &err));
  EXPECT_EQ(read_word(code, 0) & 0x3FFFFFF, (1u << 25) - 1);
  // one past max -> rejected
  EXPECT_FALSE(apply_relocation(RelocationType::Arm64Branch26, code.data(), 0, pc, max_target + 4, 0, &err));
  EXPECT_NE(err.find("out of range"), std::string::npos);
  // min negative: imm26 = -2^25
  s64 min_target = pc + 4 * (-(1LL << 25));
  EXPECT_TRUE(apply_relocation(RelocationType::Arm64Branch26, code.data(), 0, pc, min_target, 0, nullptr));
  // one below min -> rejected
  EXPECT_FALSE(apply_relocation(RelocationType::Arm64Branch26, code.data(), 0, pc, min_target - 4, 0, nullptr));
  // misaligned target -> rejected
  EXPECT_FALSE(apply_relocation(RelocationType::Arm64Branch26, code.data(), 0, pc, pc + 6, 0, &err));
  EXPECT_NE(err.find("aligned"), std::string::npos);
}

TEST(RelocationModel, CondBranch19) {
  auto code = make_code(0x54000000);
  s64 pc = 0x2000;
  EXPECT_TRUE(apply_relocation(RelocationType::Arm64CondBranch19, code.data(), 0, pc, pc + 4, 0, nullptr));
  EXPECT_EQ((read_word(code, 0) >> 5) & 0x7FFFF, 1u);
  s64 max_target = pc + 4 * ((1LL << 18) - 1);
  EXPECT_TRUE(apply_relocation(RelocationType::Arm64CondBranch19, code.data(), 0, pc, max_target, 0, nullptr));
  EXPECT_FALSE(apply_relocation(RelocationType::Arm64CondBranch19, code.data(), 0, pc, max_target + 4, 0, nullptr));
  EXPECT_FALSE(apply_relocation(RelocationType::Arm64CondBranch19, code.data(), 0, pc, pc + 2, 0, nullptr));
}

TEST(RelocationModel, AdrpPage21) {
  auto code = make_code(0x90000000);
  s64 pc = 0x1000;
  // target 4 pages ahead -> imm21 = 4
  s64 target = pc + 4 * 0x1000;
  EXPECT_TRUE(apply_relocation(RelocationType::Arm64AdrpPage21, code.data(), 0, pc, target, 0, nullptr));
  u32 w = read_word(code, 0);
  u32 immhi = (w >> 5) & 0x7FFFF;
  u32 immlo = (w >> 29) & 0b11;
  EXPECT_EQ((immhi << 2) | immlo, 4u);
  // max: 2^20 - 1 pages
  s64 max_target = (pc & ~0xFFFull) + ((1LL << 20) - 1) * 0x1000;
  std::string err;
  EXPECT_TRUE(apply_relocation(RelocationType::Arm64AdrpPage21, code.data(), 0, pc, max_target, 0, &err));
  // one past -> rejected
  EXPECT_FALSE(apply_relocation(RelocationType::Arm64AdrpPage21, code.data(), 0, pc, max_target + 0x1000, 0, &err));
  EXPECT_NE(err.find("out of range"), std::string::npos);
}

TEST(RelocationModel, AddLo12) {
  auto code = make_code(0x91000000);
  s64 pc = 0x1000;
  // low 12 bits of the page-relative address
  EXPECT_TRUE(apply_relocation(RelocationType::Arm64AddLo12, code.data(), 0, pc, 0x1234, 0, nullptr));
  EXPECT_EQ((read_word(code, 0) >> 10) & 0xFFF, 0x234u);
  EXPECT_TRUE(apply_relocation(RelocationType::Arm64AddLo12, code.data(), 0, pc, 0x1FFF, 0, nullptr));
  EXPECT_EQ((read_word(code, 0) >> 10) & 0xFFF, 0xFFFu);
}

TEST(RelocationModel, LdrLiteral19) {
  auto code = make_code(0x58000000);
  s64 pc = 0x3000;
  EXPECT_TRUE(apply_relocation(RelocationType::Arm64LdrLiteral19, code.data(), 0, pc, pc + 12, 0, nullptr));
  EXPECT_EQ((read_word(code, 0) >> 5) & 0x7FFFF, 3u);
  EXPECT_FALSE(apply_relocation(RelocationType::Arm64LdrLiteral19, code.data(), 0, pc, pc + 2, 0, nullptr));
}

TEST(RelocationModel, MovWide16) {
  // movz x0, #0, lsl #16 (hw = 1)
  auto code = make_code(0xD2800000u | (1u << 21));
  s64 pc = 0x4000;
  // target 0x0001_1234: with hw=1 the imm16 must be 0x11 (bits [32:16])
  EXPECT_TRUE(apply_relocation(RelocationType::Arm64MovWide16, code.data(), 0, pc, 0x111234ull, 0, nullptr));
  EXPECT_EQ((read_word(code, 0) >> 5) & 0xFFFF, 0x11u);
  // value that does not fit in 16 bits for hw=1 -> rejected
  EXPECT_FALSE(apply_relocation(RelocationType::Arm64MovWide16, code.data(), 0, pc, 0x100000000ull, 0, nullptr));
  // hw = 3 (lsl #48): only bits [63:48] fit
  auto code2 = make_code(0xD2800000u | (3u << 21));
  EXPECT_TRUE(apply_relocation(RelocationType::Arm64MovWide16, code2.data(), 0, pc, 0x1234000000000000ull, 0, nullptr));
  EXPECT_EQ((read_word(code2, 0) >> 5) & 0xFFFF, 0x1234u);
}

TEST(RelocationModel, X86Rel32) {
  auto code = make_code(0);
  s64 pc = 0x5000;
  // disp = target - (pc + 4)
  EXPECT_TRUE(apply_relocation(RelocationType::X86Rel32, code.data(), 0, pc, pc + 4 + 42, 0, nullptr));
  EXPECT_EQ(read_word(code, 0), 42u);
  EXPECT_TRUE(apply_relocation(RelocationType::X86Rel32, code.data(), 0, pc, pc + 4 - 42, 0, nullptr));
  EXPECT_EQ(read_word(code, 0), (u32)-42);
  // addend shifts the target
  EXPECT_TRUE(apply_relocation(RelocationType::X86Rel32, code.data(), 0, pc, pc + 4, 7, nullptr));
  EXPECT_EQ(read_word(code, 0), 7u);
  // out of 32-bit range -> rejected
  std::string err;
  EXPECT_FALSE(apply_relocation(RelocationType::X86Rel32, code.data(), 0, pc, pc + 4 + (s64)INT32_MAX + 1, 0, &err));
  EXPECT_NE(err.find("out of range"), std::string::npos);
}

TEST(RelocationModel, X86Imm32) {
  auto code = make_code(0);
  EXPECT_TRUE(apply_relocation(RelocationType::X86Imm32, code.data(), 0, 0, 0xDEADBEEF, 0, nullptr));
  EXPECT_EQ(read_word(code, 0), 0xDEADBEEFu);
  EXPECT_TRUE(apply_relocation(RelocationType::X86Imm32, code.data(), 0, 0, 0x1000, -0x1000, nullptr));
  EXPECT_EQ(read_word(code, 0), 0u);
}

TEST(RelocationModel, PositionValidation) {
  auto code = make_code(0x14000000);
  std::string err;
  // unaligned instruction position -> rejected
  EXPECT_FALSE(apply_relocation(RelocationType::Arm64Branch26, code.data(), 2, 0x1000, 0x1008, 0, &err));
  EXPECT_NE(err.find("aligned"), std::string::npos);
  // negative position -> rejected
  EXPECT_FALSE(apply_relocation(RelocationType::Arm64Branch26, code.data(), -4, 0x1000, 0x1008, 0, &err));
}
