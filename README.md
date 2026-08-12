# OpenGOAL Jak 2 — native macOS ARM64

<p align="center">
  <img src="docs/img/jak2-arm64-preview.png" alt="Jak 2 running in the native ARM64 macOS snapshot" width="960">
</p>

This repository is a focused development snapshot for running **Jak 2 from OpenGOAL natively on Apple Silicon**, without Rosetta. It is not a general OpenGOAL trilogy mirror or a Jak 1/Jak 3 project.

## Scope

- Native macOS ARM64 runtime and compiler path for Jak 2.
- Existing x86-64 compatibility for shared infrastructure.
- SDL/OpenGL renderer compatibility; graphics rewrites and mobile ports are out of scope.
- Tests, build presets and documentation target Jak 2 unless explicitly marked as shared infrastructure.

The repository does not include game ISOs or proprietary game assets. Use your own legally obtained Jak 2 copy for local extraction and testing.

## Build on Apple Silicon

```sh
cmake -B build-arm64-release --preset=Release-macos-arm64-clang
cmake --build build-arm64-release --parallel "$(sysctl -n hw.logicalcpu)"
```

Select the only supported game profile before extracting or compiling game data:

```sh
task set-game-jak2
task set-decomp-ntscv1
task extract
```

Then start the runtime from a second terminal:

```sh
task boot-game
```

The ARM64 path must be launched natively. A translated process under Rosetta is rejected by the runtime.

## Project layout

- `goal_src/jak2/` — Jak 2 GOAL sources and build description.
- `decompiler/config/jak2/` — Jak 2 decompiler configuration.
- `game/`, `common/`, `goalc/`, `decompiler/` — shared runtime, compiler and tooling required by the Jak 2 port.
- `test/` — compiler, runtime and Jak 2 regression tests.
- `docs/` — ARM64 implementation and QA notes.

OpenGOAL is a decompilation project. It does not replace the requirement to own and extract the original game legally.
