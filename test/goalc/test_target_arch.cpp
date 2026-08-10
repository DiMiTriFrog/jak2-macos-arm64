#include <fstream>

#include "common/util/FileUtil.h"

#include "goalc/emitter/InstructionSet.h"
#include "goalc/make/MakeSystem.h"

#include "gtest/gtest.h"

namespace {

std::string write_test_project(const std::string& prefix) {
  const auto path = file_util::get_file_path({"test", "target_arch_tmp.gp"});
  std::ofstream out(path, std::ios::trunc);
  out << "(set-output-prefix \"" << prefix << "\")\n";
  out.close();
  return path;
}

std::string write_test_project_with_out_mapping() {
  const auto path = file_util::get_file_path({"test", "target_arch_tmp.gp"});
  std::ofstream out(path, std::ios::trunc);
  out << "(map-path! \"$OUT\" \"out/targetarch/\")\n";
  out << "(set-output-prefix \"targetarch/\")\n";
  out << "(defstep :in \"test/target_arch_input\" :tool 'copy :out '(\"$OUT/probe\"))\n";
  out.close();
  return path;
}

}  // namespace

TEST(InstructionSet, ToString) {
  EXPECT_STREQ(emitter::to_string(emitter::InstructionSet::X86), "x86_64");
  EXPECT_STREQ(emitter::to_string(emitter::InstructionSet::ARM64), "arm64");
}

TEST(InstructionSet, ParseValid) {
  EXPECT_EQ(emitter::parse_instruction_set("x86_64"), emitter::InstructionSet::X86);
  EXPECT_EQ(emitter::parse_instruction_set("arm64"), emitter::InstructionSet::ARM64);
}

TEST(InstructionSet, ParseInvalidThrows) {
  EXPECT_THROW(emitter::parse_instruction_set(""), std::runtime_error);
  EXPECT_THROW(emitter::parse_instruction_set("x86"), std::runtime_error);
  EXPECT_THROW(emitter::parse_instruction_set("arm"), std::runtime_error);
  EXPECT_THROW(emitter::parse_instruction_set("riscv64"), std::runtime_error);
}

TEST(InstructionSet, OutputSuffix) {
  EXPECT_STREQ(emitter::output_suffix(emitter::InstructionSet::X86), "");
  EXPECT_STREQ(emitter::output_suffix(emitter::InstructionSet::ARM64), "-arm64");
}

TEST(TargetArch, X86OutputPrefixUnchanged) {
  const auto path = write_test_project("targetarch/");
  MakeSystem make(std::nullopt, "#f", emitter::InstructionSet::X86);
  make.load_project_file(path);
  EXPECT_EQ(make.compiler_output_prefix(), "targetarch/");
  fs::remove(path);
}

TEST(TargetArch, Arm64OutputPrefixSeparated) {
  const auto path = write_test_project("targetarch/");
  MakeSystem make(std::nullopt, "#f", emitter::InstructionSet::ARM64);
  make.load_project_file(path);
  EXPECT_EQ(make.compiler_output_prefix(), "targetarch-arm64/");
  fs::remove(path);
}

TEST(TargetArch, Arm64RemapsProjectOutWithPrefix) {
  const auto path = write_test_project_with_out_mapping();
  MakeSystem make(std::nullopt, "#f", emitter::InstructionSet::ARM64);
  make.load_project_file(path);
  EXPECT_EQ(make.get_dependencies("out/targetarch-arm64/probe").size(), 1);
  fs::remove(path);
}

TEST(TargetArch, X86ProjectOutMappingUnchanged) {
  const auto path = write_test_project_with_out_mapping();
  MakeSystem make(std::nullopt, "#f", emitter::InstructionSet::X86);
  make.load_project_file(path);
  EXPECT_EQ(make.get_dependencies("out/targetarch/probe").size(), 1);
  fs::remove(path);
}
