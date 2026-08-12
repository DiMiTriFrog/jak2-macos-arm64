#include "common/aot/AotManifest.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace goal_aot {
namespace {

uint64_t fnv1a_append(uint64_t hash, const std::string& value) {
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  // Length separators make concatenations unambiguous.
  hash ^= 0xff;
  hash *= 1099511628211ull;
  return hash;
}

uint64_t function_key_hash(const FunctionKey& key) {
  uint64_t hash = 1469598103934665603ull;
  hash = fnv1a_append(hash, key.game);
  hash = fnv1a_append(hash, key.object);
  hash = fnv1a_append(hash, key.segment);
  for (unsigned shift = 0; shift < 32; shift += 8) {
    hash ^= static_cast<uint8_t>((key.ordinal >> shift) & 0xff);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool contains_path_parent(const std::string& path) {
  size_t start = 0;
  while (start <= path.size()) {
    const size_t end = path.find_first_of("/\\", start);
    const std::string component = path.substr(start, end - start);
    if (component == "..") {
      return true;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return false;
}

void append_json_string(std::string& output, const std::string& value) {
  output.push_back('"');
  for (const unsigned char byte : value) {
    switch (byte) {
      case '"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        if (byte < 0x20) {
          std::ostringstream escaped;
          escaped << "\\u" << std::hex << std::setfill('0') << std::setw(4)
                  << static_cast<unsigned>(byte);
          output += escaped.str();
        } else {
          output.push_back(static_cast<char>(byte));
        }
        break;
    }
  }
  output.push_back('"');
}

void append_key(std::string& output, const char* key, const std::string& value) {
  append_json_string(output, key);
  output.push_back(':');
  append_json_string(output, value);
}

void append_key(std::string& output, const char* key, uint32_t value) {
  append_json_string(output, key);
  output.push_back(':');
  output += std::to_string(value);
}

std::string sanitize_symbol_component(const std::string& component) {
  std::string result;
  result.reserve(component.size());
  for (const unsigned char byte : component) {
    result.push_back(std::isalnum(byte) ? static_cast<char>(byte) : '_');
  }
  return result;
}

}  // namespace

bool FunctionKey::operator==(const FunctionKey& other) const {
  return game == other.game && object == other.object && segment == other.segment &&
         ordinal == other.ordinal;
}

bool FunctionKey::operator<(const FunctionKey& other) const {
  if (game != other.game) {
    return game < other.game;
  }
  if (object != other.object) {
    return object < other.object;
  }
  if (segment != other.segment) {
    return segment < other.segment;
  }
  return ordinal < other.ordinal;
}

void validate_schema(uint32_t schema_version, uint32_t aot_abi_version) {
  if (schema_version != kManifestSchemaVersion) {
    throw std::invalid_argument("unknown AOT manifest schema version");
  }
  if (aot_abi_version != kManifestAotAbiVersion) {
    throw std::invalid_argument("unsupported AOT manifest ABI version");
  }
}

void validate_function_id(uint32_t id) {
  if (id == 0) {
    throw std::invalid_argument("AOT function ID 0 is reserved");
  }
}

uint32_t checked_u32(uint64_t value, const char* field_name) {
  if (value > std::numeric_limits<uint32_t>::max()) {
    throw std::overflow_error(std::string("AOT field exceeds u32: ") + field_name);
  }
  return static_cast<uint32_t>(value);
}

void validate_relative_manifest_path(const std::string& path) {
  if (path.empty() || path.front() == '/' || path.front() == '\\' ||
      (path.size() >= 2 && path[1] == ':') || contains_path_parent(path)) {
    throw std::invalid_argument("AOT manifest paths must be relative and cannot contain ..");
  }
}

std::string stable_function_symbol(const FunctionKey& key) {
  if (key.game.empty() || key.object.empty() || key.segment.empty()) {
    throw std::invalid_argument("AOT function key components cannot be empty");
  }
  const uint64_t hash = function_key_hash(key);
  std::ostringstream output;
  output << "og_aot_" << sanitize_symbol_component(key.game) << "_"
         << sanitize_symbol_component(key.object) << "_" << sanitize_symbol_component(key.segment)
         << "_f"
         << key.ordinal << "_h" << std::hex << std::setfill('0') << std::setw(16) << hash;
  const std::string symbol = output.str();
  for (const unsigned char byte : symbol) {
    if (byte >= 0x80 || !(std::isalnum(byte) || byte == '_')) {
      throw std::invalid_argument("AOT symbol contains an unsafe character");
    }
  }
  return symbol;
}

ManifestBuilder::ManifestBuilder(ManifestMetadata metadata) : m_metadata(std::move(metadata)) {
  validate_schema(m_metadata.schema_version, m_metadata.aot_abi_version);
  if (m_metadata.game_version.empty() || m_metadata.target_triple.empty() ||
      m_metadata.source_hash.empty()) {
    throw std::invalid_argument("AOT manifest metadata is incomplete");
  }
}

void ManifestBuilder::add_function(FunctionRecord function) {
  if (function.key.game.empty() || function.key.object.empty() || function.key.segment.empty()) {
    throw std::invalid_argument("AOT function key components cannot be empty");
  }
  if (function.code_alignment == 0 || (function.code_alignment & (function.code_alignment - 1))) {
    throw std::invalid_argument("AOT code alignment must be a non-zero power of two");
  }
  // Names are metadata, but rejecting control characters keeps the canonical
  // file human-readable and prevents ambiguous diagnostics.
  for (const unsigned char byte : function.goal_name) {
    if (byte < 0x20) {
      throw std::invalid_argument("AOT GOAL names cannot contain control characters");
    }
  }
  m_functions.push_back(std::move(function));
}

AotManifest ManifestBuilder::build() const {
  AotManifest manifest{m_metadata, m_functions};
  std::sort(manifest.functions.begin(), manifest.functions.end(),
            [](const FunctionRecord& left, const FunctionRecord& right) {
              return left.key < right.key;
            });

  uint32_t id = 1;
  std::string previous_symbol;
  for (auto& function : manifest.functions) {
    if (id == 0) {
      throw std::overflow_error("AOT manifest has too many functions");
    }
    if (&function != manifest.functions.data() && function.key ==
            (&function - 1)->key) {
      throw std::invalid_argument("duplicate AOT function key");
    }
    function.id = id++;
    function.symbol = stable_function_symbol(function.key);
    if (!previous_symbol.empty() && function.symbol == previous_symbol) {
      throw std::invalid_argument("AOT function symbol collision");
    }
    previous_symbol = function.symbol;
  }
  return manifest;
}

std::string AotManifest::serialize_canonical() const {
  validate_schema(metadata.schema_version, metadata.aot_abi_version);
  std::string output = "{\n  ";
  append_key(output, "schema_version", metadata.schema_version);
  output += ",\n  ";
  append_key(output, "aot_abi_version", metadata.aot_abi_version);
  output += ",\n  ";
  append_key(output, "game_version", metadata.game_version);
  output += ",\n  ";
  append_key(output, "target_triple", metadata.target_triple);
  output += ",\n  ";
  append_key(output, "source_hash", metadata.source_hash);
  output += ",\n  \"functions\":[";

  for (size_t i = 0; i < functions.size(); ++i) {
    const auto& function = functions[i];
    if (i != 0) {
      output.push_back(',');
    }
    output += "\n    {";
    append_key(output, "id", function.id);
    output += ",";
    append_key(output, "game", function.key.game);
    output += ",";
    append_key(output, "object", function.key.object);
    output += ",";
    append_key(output, "segment", function.key.segment);
    output += ",";
    append_key(output, "ordinal", function.key.ordinal);
    output += ",";
    append_key(output, "goal_name", function.goal_name);
    output += ",";
    append_key(output, "code_size", function.code_size);
    output += ",";
    append_key(output, "code_alignment", function.code_alignment);
    output += ",";
    append_key(output, "symbol", function.symbol);
    output += "}";
  }
  if (!functions.empty()) {
    output.push_back('\n');
    output += "  ";
  }
  output += "]\n}\n";
  return output;
}

}  // namespace goal_aot
