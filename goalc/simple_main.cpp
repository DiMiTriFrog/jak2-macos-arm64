#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/versions/versions.h"

#include "goalc/compiler/Compiler.h"
#include "goalc/emitter/InstructionSet.h"

int main(int argc, char** argv) {
  // logging
  lg::set_stdout_level(lg::level::info);
  lg::set_flush_level(lg::level::info);
  lg::initialize();

  // game version
  std::string game = "jak1";
  std::string target_arch = "x86_64";
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--target-arch" && i + 1 < argc) {
      target_arch = argv[++i];
    } else {
      game = arg;
    }
  }
  GameVersion game_version = game_name_to_version(game);

  emitter::InstructionSet instr_set;
  try {
    instr_set = emitter::parse_instruction_set(target_arch);
  } catch (const std::exception& e) {
    lg::error("Error: {}", e.what());
    return 1;
  }

  // path
  if (!file_util::setup_project_path(std::nullopt)) {
    return 1;
  }

  lg::info("OpenGOAL Compiler {}.{}", versions::GOAL_VERSION_MAJOR, versions::GOAL_VERSION_MINOR);

  std::unique_ptr<Compiler> compiler;
  ReplStatus status = ReplStatus::OK;
  try {
    compiler = std::make_unique<Compiler>(game_version, instr_set, std::nullopt,
                                          "", std::make_unique<REPL::Wrapper>(game_version));
    while (status != ReplStatus::WANT_EXIT) {
      if (status == ReplStatus::WANT_RELOAD) {
        lg::info("Reloading compiler...");
        if (compiler) {
          compiler->save_repl_history();
        }
        compiler =
            std::make_unique<Compiler>(game_version, instr_set, std::nullopt, "",
                                       std::make_unique<REPL::Wrapper>(game_version));
        status = ReplStatus::OK;
      }
      std::string input_from_stdin = compiler->get_repl_input();
      if (!input_from_stdin.empty()) {
        status = compiler->handle_repl_string(input_from_stdin);
      }
    }
  } catch (std::exception& e) {
    lg::error("Compiler Fatal Error: {}", e.what());
  }

  return 0;
}
