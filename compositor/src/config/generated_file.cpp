#include "config/generated_file.h"

#include "core/async_file_writer.h"

#include <fstream>
#include <sstream>

namespace andromeda {

  std::string
  editGeneratedToml(std::string_view existing, std::string_view header, const std::function<void(toml::table&)>& edit) {
    toml::table root;
    if (!existing.empty()) {
      try {
        root = toml::parse(existing);
      } catch (const toml::parse_error&) {
        // A hand-damaged file is replaced rather than kept half-parsed.
      }
    }
    edit(root);
    std::ostringstream out;
    out << header << root << '\n';
    return std::move(out).str();
  }

  std::string readGeneratedFile(const std::filesystem::path& file) {
    if (std::optional<std::string> pending = pendingFileText(file)) {
      return std::move(*pending);
    }
    std::string text;
    if (std::ifstream in(file); in) {
      text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return text;
  }

  void rewriteGeneratedTomlAsync(
      const std::filesystem::path& file, std::string_view header, const std::function<void(toml::table&)>& edit
  ) {
    writeFileAsync(file, editGeneratedToml(readGeneratedFile(file), header, edit));
  }

  toml::table& generatedTable(toml::table& root, std::initializer_list<std::string_view> path) {
    toml::table* table = &root;
    for (const std::string_view key : path) {
      if ((*table)[key].as_table() == nullptr) {
        table->insert_or_assign(key, toml::table{});
      }
      table = (*table)[key].as_table();
    }
    return *table;
  }

} // namespace andromeda
