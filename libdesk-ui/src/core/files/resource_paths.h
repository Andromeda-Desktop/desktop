#pragma once

#include <filesystem>
#include <string_view>

namespace paths {

  [[nodiscard]] const std::filesystem::path& assetsRoot();
  [[nodiscard]] std::filesystem::path assetPath(std::string_view relativePath);
  // The directory with a module's translation catalogs (<lang>.json): `sourceDir` in an uninstalled build, else
  // <datadir>/andromeda/translations/<domain>, else Resources/translations/<domain> in a macOS bundle.
  [[nodiscard]] std::filesystem::path translationsDir(std::string_view domain, const std::filesystem::path& sourceDir);

} // namespace paths
