#include "common/aot/AotDataTemplate.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace goal_aot {
namespace {

constexpr std::size_t kRelocationWireSize = 16;

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
  }
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t& cursor) {
  if (bytes.size() - cursor < sizeof(std::uint32_t)) {
    throw std::invalid_argument("truncated AOT-DATA integer");
  }
  std::uint32_t result = 0;
  for (unsigned shift = 0; shift < 32; shift += 8) {
    result |= static_cast<std::uint32_t>(bytes[cursor++]) << shift;
  }
  return result;
}

void append_bytes(std::vector<std::uint8_t>& output, std::span<const std::uint8_t> bytes) {
  output.insert(output.end(), bytes.begin(), bytes.end());
}

void validate_name(const std::string& name, const char* field) {
  if (name.empty() || name.size() > 255 || name.find('\0') != std::string::npos ||
      name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
      name.find("..") != std::string::npos) {
    throw std::invalid_argument(std::string("invalid AOT-DATA ") + field);
  }
  for (const unsigned char byte : name) {
    if (byte < 0x20 || byte >= 0x80) {
      throw std::invalid_argument(std::string("AOT-DATA ") + field + " is not ASCII-safe");
    }
  }
}

void validate_kind(RelocationKind kind) {
  switch (kind) {
    case RelocationKind::DataOffset32:
    case RelocationKind::SymbolValue32:
    case RelocationKind::TypePointer32:
    case RelocationKind::FunctionDescriptor32:
    case RelocationKind::StringOffset32:
      return;
    case RelocationKind::CodePointer:
    case RelocationKind::NativePointer:
      throw std::invalid_argument("AOT-DATA contains a forbidden executable relocation");
  }
  throw std::invalid_argument("unknown AOT-DATA relocation kind");
}

}  // namespace

void AotDataTemplate::validate() const {
  if (abi_version != kDataObjectAotAbiVersion) {
    throw std::invalid_argument("unsupported AOT-DATA ABI version");
  }
  if (data.size() > kMaxDataObjectBytes) {
    throw std::length_error("AOT-DATA data section exceeds the maximum size");
  }
  if (relocations.size() > kMaxDataObjectRelocations) {
    throw std::length_error("AOT-DATA relocation table exceeds the maximum size");
  }
  validate_name(game, "game name");
  validate_name(object, "object name");
  if (source_hash.size() != 32) {
    throw std::invalid_argument("AOT-DATA source hash must have SHA-256 length");
  }
  std::vector<std::uint32_t> relocation_sources;
  relocation_sources.reserve(relocations.size());
  for (const auto& relocation : relocations) {
    validate_kind(relocation.kind);
    if (relocation.source_offset > data.size() ||
        data.size() - relocation.source_offset < sizeof(std::uint32_t) ||
        (relocation.source_offset & 3) != 0) {
      throw std::out_of_range("AOT-DATA relocation source is outside an aligned u32");
    }
    relocation_sources.push_back(relocation.source_offset);
  }
  std::sort(relocation_sources.begin(), relocation_sources.end());
  if (std::adjacent_find(relocation_sources.begin(), relocation_sources.end()) !=
      relocation_sources.end()) {
    throw std::invalid_argument("AOT-DATA contains duplicate relocation sources");
  }
}

std::vector<std::uint8_t> AotDataTemplate::serialize() const {
  validate();
  if (game.size() > std::numeric_limits<std::uint32_t>::max() ||
      object.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::overflow_error("AOT-DATA name is too large");
  }

  const auto checked_size = [](std::size_t value, const char* field) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
      throw std::overflow_error(std::string("AOT-DATA ") + field + " exceeds u32");
    }
    return static_cast<std::uint32_t>(value);
  };

  std::vector<std::uint8_t> output;
  output.reserve(kDataObjectHeaderSize + game.size() + object.size() + data.size() +
                 relocations.size() * kRelocationWireSize);
  append_u32(output, kDataObjectMagic);
  append_u32(output, kDataObjectSchemaVersion);
  append_u32(output, abi_version);
  append_u32(output, kDataObjectHeaderSize);
  append_u32(output, checked_size(game.size(), "game name"));
  append_u32(output, checked_size(object.size(), "object name"));
  append_u32(output, checked_size(data.size(), "data"));
  append_u32(output, checked_size(relocations.size(), "relocation count"));
  append_u32(output, 0);  // code section count: must stay zero for AOT-DATA
  append_u32(output, 0);  // native pointer count: must stay zero for AOT-DATA
  std::array<std::uint8_t, 32> hash{};
  std::copy(source_hash.begin(), source_hash.end(), hash.begin());
  append_bytes(output, hash);

  output.insert(output.end(), game.begin(), game.end());
  output.insert(output.end(), object.begin(), object.end());
  append_bytes(output, data);
  for (const auto& relocation : relocations) {
    append_u32(output, static_cast<std::uint32_t>(relocation.kind));
    append_u32(output, relocation.source_offset);
    append_u32(output, relocation.target_object);
    append_u32(output, relocation.target_offset);
  }
  return output;
}

AotDataTemplate AotDataTemplate::parse(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < kDataObjectHeaderSize) {
    throw std::invalid_argument("truncated AOT-DATA header");
  }
  std::size_t cursor = 0;
  const auto magic = read_u32(bytes, cursor);
  const auto schema = read_u32(bytes, cursor);
  const auto abi = read_u32(bytes, cursor);
  const auto header_size = read_u32(bytes, cursor);
  const auto game_size = read_u32(bytes, cursor);
  const auto object_size = read_u32(bytes, cursor);
  const auto data_size = read_u32(bytes, cursor);
  const auto relocation_count = read_u32(bytes, cursor);
  const auto code_count = read_u32(bytes, cursor);
  const auto native_pointer_count = read_u32(bytes, cursor);
  if (magic != kDataObjectMagic || schema != kDataObjectSchemaVersion ||
      header_size != kDataObjectHeaderSize) {
    throw std::invalid_argument("unsupported AOT-DATA header");
  }
  if (code_count != 0 || native_pointer_count != 0) {
    throw std::invalid_argument("AOT-DATA declares executable content");
  }
  if (game_size > 255 || object_size > 255 || data_size > kMaxDataObjectBytes ||
      relocation_count > kMaxDataObjectRelocations) {
    throw std::length_error("AOT-DATA declares an oversized field");
  }
  if (bytes.size() - cursor < 32) {
    throw std::invalid_argument("truncated AOT-DATA source hash");
  }
  std::vector<std::uint8_t> source_hash(bytes.begin() + cursor, bytes.begin() + cursor + 32);
  cursor += 32;

  const std::size_t fixed_size = static_cast<std::size_t>(game_size) + object_size + data_size;
  const std::size_t relocation_bytes = static_cast<std::size_t>(relocation_count) *
                                       kRelocationWireSize;
  if (fixed_size > bytes.size() - cursor || relocation_bytes > bytes.size() - cursor - fixed_size ||
      bytes.size() != cursor + fixed_size + relocation_bytes) {
    throw std::invalid_argument("AOT-DATA sizes do not match the file");
  }

  AotDataTemplate result;
  result.abi_version = abi;
  result.game.assign(reinterpret_cast<const char*>(bytes.data() + cursor), game_size);
  cursor += game_size;
  result.object.assign(reinterpret_cast<const char*>(bytes.data() + cursor), object_size);
  cursor += object_size;
  result.data.assign(bytes.begin() + cursor, bytes.begin() + cursor + data_size);
  cursor += data_size;
  result.source_hash = std::move(source_hash);
  result.relocations.reserve(relocation_count);
  for (std::uint32_t i = 0; i < relocation_count; ++i) {
    DataRelocation relocation;
    relocation.kind = static_cast<RelocationKind>(read_u32(bytes, cursor));
    relocation.source_offset = read_u32(bytes, cursor);
    relocation.target_object = read_u32(bytes, cursor);
    relocation.target_offset = read_u32(bytes, cursor);
    result.relocations.push_back(relocation);
  }
  result.validate();
  return result;
}

void instantiate_data_template(const AotDataTemplate& object,
                               std::span<std::uint8_t> destination,
                               const RelocationResolver& resolve) {
  object.validate();
  if (destination.size() != object.data.size()) {
    throw std::invalid_argument("AOT-DATA destination size does not match the template");
  }
  if (!resolve && !object.relocations.empty()) {
    throw std::invalid_argument("AOT-DATA relocations require a resolver");
  }

  std::vector<std::uint8_t> candidate(object.data);
  for (const auto& relocation : object.relocations) {
    const auto value = resolve(relocation.kind, relocation.target_object, relocation.target_offset);
    candidate[relocation.source_offset + 0] = static_cast<std::uint8_t>(value & 0xff);
    candidate[relocation.source_offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    candidate[relocation.source_offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    candidate[relocation.source_offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
  }
  std::copy(candidate.begin(), candidate.end(), destination.begin());
}

}  // namespace goal_aot
