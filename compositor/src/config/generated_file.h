#pragma once

#include "protocols/toml.h"

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>

namespace andromeda {

  // A TOML file Andromeda writes on the user's behalf (displays.toml, settings.toml).

  // Parses `existing` (a damaged document starts empty), applies `edit`, and serializes it under `header`.
  [[nodiscard]] std::string
  editGeneratedToml(std::string_view existing, std::string_view header, const std::function<void(toml::table&)>& edit);

  // `file` as the next read will find it: the text a pending async write leaves there, else the disk; empty when
  // missing.
  [[nodiscard]] std::string readGeneratedFile(const std::filesystem::path& file);

  // editGeneratedToml on readGeneratedFile(file), written by async_file_writer: the main thread never waits on
  // a write. A failed write is logged by the writer.
  void rewriteGeneratedTomlAsync(
      const std::filesystem::path& file, std::string_view header, const std::function<void(toml::table&)>& edit
  );

  // The table at `path` below `root`, created along the way. A non-table value in the way is replaced.
  toml::table& generatedTable(toml::table& root, std::initializer_list<std::string_view> path);

} // namespace andromeda
