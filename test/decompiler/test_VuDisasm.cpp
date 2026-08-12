#include "common/util/FileUtil.h"

#include "decompiler/VuDisasm/VuDisassembler.h"
#include "decompiler/util/DataParser.h"
#include "gtest/gtest.h"

#include "fmt/format.h"

using namespace decompiler;

namespace {
std::vector<u32> get_test_data(const std::string& name) {
  auto text = file_util::read_text_file(
      file_util::get_file_path({fmt::format("test/decompiler/vu_reference/jak2/{}.txt", name)}));
  auto parsed = parse_data(text);

  std::vector<u32> data;
  for (auto& word : parsed.words) {
    EXPECT_EQ(word.kind(), LinkedWord::Kind::PLAIN_DATA);
    data.push_back(word.data);
  }
  return data;
}

std::string get_expected(const std::string& name) {
  return file_util::read_text_file(file_util::get_file_path(
      {fmt::format("test/decompiler/vu_reference/jak2/{}-result.txt", name)}));
}

void check_disassembly(const std::string& name, VuDisassembler::VuKind kind) {
  auto data = get_test_data(name);
  VuDisassembler disasm(kind);
  auto program = disasm.disassemble(data.data(), data.size() * 4, false);
  EXPECT_EQ(disasm.to_string(program), get_expected(name));
}
}  // namespace

TEST(VuDisasmJak2, ShadowVu0) {
  check_disassembly("shadow-vu0", VuDisassembler::VuKind::VU0);
}

TEST(VuDisasmJak2, ShadowVu1) {
  check_disassembly("shadow-vu1", VuDisassembler::VuKind::VU1);
}

TEST(VuDisasmJak2, OceanTextureVu1) {
  check_disassembly("ocean-texture-vu1", VuDisassembler::VuKind::VU1);
}

TEST(VuDisasmJak2, MercVu1) {
  check_disassembly("merc-vu1", VuDisassembler::VuKind::VU1);
}

TEST(VuDisasmJak2, SpriteVu1) {
  check_disassembly("sprite-vu1", VuDisassembler::VuKind::VU1);
}
