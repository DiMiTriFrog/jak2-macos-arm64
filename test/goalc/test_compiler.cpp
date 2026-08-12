#include "goalc/compiler/Compiler.h"
#include "gtest/gtest.h"

TEST(CompilerAndRuntime, ConstructCompiler) {
  Compiler compiler(GameVersion::Jak2, emitter::InstructionSet::X86);
}
