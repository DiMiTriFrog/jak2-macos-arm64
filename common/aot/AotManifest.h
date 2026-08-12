#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace goal_aot {

constexpr uint32_t kManifestSchemaVersion = 1;
constexpr uint32_t kManifestAotAbiVersion = 1;

struct FunctionKey {
  std::string game;
  std::string object;
  std::string segment;
  uint32_t ordinal = 0;

  bool operator==(const FunctionKey& other) const;
  bool operator<(const FunctionKey& other) const;
};

struct FunctionRecord {
  FunctionKey key;
  std::string goal_name;
  uint32_t code_size = 0;
  uint32_t code_alignment = 4;
  uint32_t id = 0;
  std::string symbol;
};

struct ManifestMetadata {
  std::string game_version;
  std::string target_triple;
  std::string source_hash;
  uint32_t schema_version = kManifestSchemaVersion;
  uint32_t aot_abi_version = kManifestAotAbiVersion;
};

struct AotManifest {
  ManifestMetadata metadata;
  std::vector<FunctionRecord> functions;

  // The representation is deliberately canonical: fixed key order, sorted
  // functions, decimal integers, and a trailing newline.
  std::string serialize_canonical() const;
};

class ManifestBuilder {
 public:
  explicit ManifestBuilder(ManifestMetadata metadata);

  void add_function(FunctionRecord function);
  AotManifest build() const;

 private:
  ManifestMetadata m_metadata;
  std::vector<FunctionRecord> m_functions;
};

// These validators are shared by the host writer and the runtime loader.
void validate_schema(uint32_t schema_version, uint32_t aot_abi_version);
void validate_function_id(uint32_t id);
uint32_t checked_u32(uint64_t value, const char* field_name);
void validate_relative_manifest_path(const std::string& path);

// Stable, ASCII-only symbol derived from the complete function key. The GOAL
// display name is metadata and is intentionally not part of identity.
std::string stable_function_symbol(const FunctionKey& key);

}  // namespace goal_aot
