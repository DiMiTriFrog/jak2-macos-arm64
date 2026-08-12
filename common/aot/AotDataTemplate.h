#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace goal_aot {

inline constexpr std::uint32_t kDataObjectMagic = 0x44544f41;  // "AOTD"
inline constexpr std::uint32_t kDataObjectSchemaVersion = 1;
inline constexpr std::uint32_t kDataObjectAotAbiVersion = 1;
inline constexpr std::uint32_t kDataObjectHeaderSize = 72;
inline constexpr std::size_t kMaxDataObjectBytes = 256u * 1024u * 1024u;
inline constexpr std::size_t kMaxDataObjectRelocations = 1u << 20;

enum class RelocationKind : std::uint32_t {
  DataOffset32 = 1,
  SymbolValue32 = 2,
  TypePointer32 = 3,
  FunctionDescriptor32 = 4,
  StringOffset32 = 5,
  // These values are intentionally part of the rejected wire vocabulary. A
  // producer cannot accidentally turn a native address into importable data.
  CodePointer = 0x100,
  NativePointer = 0x101,
};

struct DataRelocation {
  RelocationKind kind = RelocationKind::DataOffset32;
  std::uint32_t source_offset = 0;
  std::uint32_t target_object = 0;
  std::uint32_t target_offset = 0;
};

struct AotDataTemplate {
  std::uint32_t abi_version = 1;
  std::string game;
  std::string object;
  std::vector<std::uint8_t> data;
  std::vector<DataRelocation> relocations;
  // Required even for a local fixture: an all-zero hash is the explicit
  // "not supplied" value, while an absent field is malformed.
  std::vector<std::uint8_t> source_hash;

  void validate() const;
  std::vector<std::uint8_t> serialize() const;
  static AotDataTemplate parse(std::span<const std::uint8_t> bytes);
};

using RelocationResolver =
    std::function<std::uint32_t(RelocationKind kind,
                                 std::uint32_t target_object,
                                 std::uint32_t target_offset)>;

// Build the instance in a private copy and publish it only after every
// relocation succeeds. A failed resolver leaves destination unchanged.
void instantiate_data_template(const AotDataTemplate& object,
                               std::span<std::uint8_t> destination,
                               const RelocationResolver& resolve);

}  // namespace goal_aot
