#include "goalc/compiler/Compiler.h"
#include "gtest/gtest.h"

void add_common_expected_type_mismatches(Compiler& c) {
  c.add_ignored_define_extern_symbol("draw-drawable-tree-tfrag");
  c.add_ignored_define_extern_symbol("draw-drawable-tree-trans-tfrag");
  c.add_ignored_define_extern_symbol("draw-drawable-tree-dirt-tfrag");
  c.add_ignored_define_extern_symbol("draw-drawable-tree-ice-tfrag");
  c.add_ignored_define_extern_symbol("tfrag-init-buffer");
}

void add_jak2_expected_type_mismatches(Compiler& c) {
  c.add_ignored_type_definition("editable-plane");
}

TEST(Jak2TypeConsistency, MANUAL_TEST_TypeConsistencyWithBuildFirst) {
  Compiler compiler(GameVersion::Jak2, emitter::InstructionSet::X86);
  compiler.enable_throw_on_redefines();
  add_common_expected_type_mismatches(compiler);
  add_jak2_expected_type_mismatches(compiler);
  compiler.run_test_no_load("test/goalc/source_templates/with_game/test-build-all-code.gc");
  compiler.run_test_no_load("decompiler/config/jak2/all-types.gc");
}

TEST(Jak2TypeConsistency, TypeConsistency) {
  Compiler compiler(GameVersion::Jak2, emitter::InstructionSet::X86);
  compiler.enable_throw_on_redefines();
  add_common_expected_type_mismatches(compiler);
  add_jak2_expected_type_mismatches(compiler);
  compiler.run_test_no_load("decompiler/config/jak2/all-types.gc");
  compiler.run_test_no_load("test/goalc/source_templates/with_game/test-build-all-code.gc");
}
