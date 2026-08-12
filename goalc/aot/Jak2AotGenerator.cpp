#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/aot/AotManifest.h"

namespace fs = std::filesystem;

namespace {

struct Options {
  fs::path source_root;
  fs::path output_dir;
  std::string target_triple;
};

struct SourceFile {
  fs::path absolute;
  std::string relative;
  std::string contents;
};

[[noreturn]] void usage_error(const std::string& message) {
  throw std::invalid_argument(message +
                              "\nusage: generate-jak2-aot --source-root DIR --output-dir DIR "
                              "--target-triple TRIPLE");
}

Options parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    auto value = [&](const char* name) {
      if (i + 1 >= argc) {
        usage_error(std::string("missing value for ") + name);
      }
      return std::string(argv[++i]);
    };
    if (argument == "--source-root") {
      options.source_root = value("--source-root");
    } else if (argument == "--output-dir") {
      options.output_dir = value("--output-dir");
    } else if (argument == "--target-triple") {
      options.target_triple = value("--target-triple");
    } else if (argument == "--help") {
      usage_error("");
    } else {
      usage_error("unknown argument: " + argument);
    }
  }
  if (options.source_root.empty() || options.output_dir.empty() || options.target_triple.empty()) {
    usage_error("all options are required");
  }
  return options;
}

std::string read_text(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read GOAL source: " + path.string());
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

uint64_t fnv1a_append(uint64_t hash, const std::string& value) {
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  hash ^= 0xff;
  hash *= 1099511628211ull;
  return hash;
}

std::string source_hash(const std::vector<SourceFile>& files) {
  uint64_t hash = 1469598103934665603ull;
  for (const auto& file : files) {
    hash = fnv1a_append(hash, file.relative);
    hash = fnv1a_append(hash, file.contents);
  }
  hash = fnv1a_append(hash, "jak2-aot-source-v1");
  std::ostringstream output;
  output << std::hex << std::setfill('0') << std::setw(16) << hash;
  return output.str();
}

std::vector<SourceFile> collect_sources(const fs::path& source_root) {
  const fs::path jak2_root = source_root / "goal_src" / "jak2";
  if (!fs::is_directory(jak2_root)) {
    throw std::runtime_error("Jak II GOAL source directory does not exist: " +
                             jak2_root.string());
  }

  std::vector<SourceFile> files;
  for (const auto& entry : fs::recursive_directory_iterator(jak2_root)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".gc") {
      continue;
    }
    const auto relative = fs::relative(entry.path(), source_root).generic_string();
    files.push_back({entry.path(), relative, read_text(entry.path())});
  }
  std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
    return left.relative < right.relative;
  });
  return files;
}

struct Inventory {
  std::vector<goal_aot::FunctionRecord> functions;
  uint32_t mips2c_declarations = 0;
};

std::vector<std::string> find_named_forms(const std::string& contents,
                                          const std::string& keyword) {
  std::vector<std::string> names;
  size_t cursor = 0;
  while ((cursor = contents.find('(', cursor)) != std::string::npos) {
    size_t token = cursor + 1;
    while (token < contents.size() &&
           (contents[token] == ' ' || contents[token] == '\t' || contents[token] == '\n' ||
            contents[token] == '\r')) {
      ++token;
    }
    if (contents.compare(token, keyword.size(), keyword) == 0 &&
        (token + keyword.size() == contents.size() ||
         contents[token + keyword.size()] == ' ' || contents[token + keyword.size()] == '\t' ||
         contents[token + keyword.size()] == '\n' || contents[token + keyword.size()] == '\r')) {
      token += keyword.size();
      while (token < contents.size() &&
             (contents[token] == ' ' || contents[token] == '\t' || contents[token] == '\n' ||
              contents[token] == '\r')) {
        ++token;
      }
      const size_t name_start = token;
      while (token < contents.size() && contents[token] != ' ' && contents[token] != '\t' &&
             contents[token] != '\n' && contents[token] != '\r' && contents[token] != '(' &&
             contents[token] != ')') {
        ++token;
      }
      if (token != name_start) {
        names.push_back(contents.substr(name_start, token - name_start));
      }
    }
    cursor = token == cursor + 1 ? cursor + 1 : token;
  }
  return names;
}

Inventory build_inventory(const std::vector<SourceFile>& files) {

  Inventory inventory;
  uint32_t id = 1;
  for (const auto& file : files) {
    auto function_names = find_named_forms(file.contents, "defun");
    auto method_names = find_named_forms(file.contents, "defmethod");
    function_names.insert(function_names.end(), method_names.begin(), method_names.end());
    uint32_t ordinal = 0;
    for (const auto& function_name : function_names) {
      goal_aot::FunctionKey key{"jak2", file.relative, "main", ordinal};
      goal_aot::FunctionRecord record;
      record.key = key;
      record.goal_name = function_name;
      record.code_size = 0;
      record.code_alignment = 4;
      record.id = id++;
      record.symbol = goal_aot::stable_function_symbol(key);
      inventory.functions.push_back(std::move(record));
      ++ordinal;
    }
    inventory.mips2c_declarations +=
        static_cast<uint32_t>(find_named_forms(file.contents, "def-mips2c").size());
    inventory.mips2c_declarations +=
        static_cast<uint32_t>(find_named_forms(file.contents, "defmethod-mips2c").size());
  }
  if (inventory.functions.empty()) {
    throw std::runtime_error("Jak II source inventory contains no defun/defmethod forms");
  }
  return inventory;
}

void write_text_file(const fs::path& path, const std::string& contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot write generated file: " + path.string());
  }
  output << contents;
  if (!output) {
    throw std::runtime_error("failed while writing generated file: " + path.string());
  }
}

void write_report(const fs::path& path,
                  const Options& options,
                  const std::vector<SourceFile>& files,
                  const Inventory& inventory,
                  const std::string& hash) {
  std::ostringstream report;
  report << "{\n"
         << "  \"schema\": 1,\n"
         << "  \"game\": \"jak2\",\n"
         << "  \"target_triple\": \"" << options.target_triple << "\",\n"
         << "  \"source_hash\": \"" << hash << "\",\n"
         << "  \"source_file_count\": " << files.size() << ",\n"
         << "  \"function_inventory_count\": " << inventory.functions.size() << ",\n"
         << "  \"mips2c_declaration_count\": " << inventory.mips2c_declarations << ",\n"
         << "  \"aot_code_generation\": \"not-yet-linked\",\n"
         << "  \"complete\": false\n"
         << "}\n";
  write_text_file(path, report.str());
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse_options(argc, argv);
    const auto files = collect_sources(fs::absolute(options.source_root));
    const auto hash = source_hash(files);
    const auto inventory = build_inventory(files);

    goal_aot::ManifestMetadata metadata;
    metadata.game_version = "jak2";
    metadata.target_triple = options.target_triple;
    metadata.source_hash = hash;
    goal_aot::ManifestBuilder builder(metadata);
    for (const auto& function : inventory.functions) {
      builder.add_function(function);
    }
    const auto manifest = builder.build();

    fs::create_directories(options.output_dir);
    write_text_file(options.output_dir / "manifest.json", manifest.serialize_canonical());
    write_report(options.output_dir / "build-report.json", options, files, inventory, hash);
    std::cout << "generated Jak II AOT inventory: " << inventory.functions.size()
              << " functions, " << inventory.mips2c_declarations << " MIPS2C declarations\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "generate-jak2-aot: " << error.what() << '\n';
    return 1;
  }
}
