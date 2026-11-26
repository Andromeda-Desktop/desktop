#include "system/system_font.h"

#include "protocols/xdg_dirs.h"
#include "system/dconf_read.h"
#include "util/string_utils.h"

#include <cstdlib>
#include <glib.h>
#include <pango/pango.h>
#include <system_error>

namespace system_font {

  namespace {

    constexpr std::string_view kFontNameKey = "/org/gnome/desktop/interface/font-name";
    constexpr std::string_view kFallbackFamily = "Inter";

    std::filesystem::path configDir() {
      return xdg::configHome();
    }

    std::vector<std::filesystem::path> settingsIniFiles() {
      const std::filesystem::path dir = configDir();
      return {dir / "gtk-4.0" / "settings.ini", dir / "gtk-3.0" / "settings.ini"};
    }

    std::optional<std::string> readSettingsIniFontName(const std::filesystem::path& path) {
      GKeyFile* keyFile = g_key_file_new();
      std::optional<std::string> out;
      if (g_key_file_load_from_file(keyFile, path.c_str(), G_KEY_FILE_NONE, nullptr) != FALSE) {
        if (gchar* raw = g_key_file_get_string(keyFile, "Settings", "gtk-font-name", nullptr); raw != nullptr) {
          std::string value = StringUtils::trim(raw);
          g_free(raw);
          if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
          }
          if (!value.empty()) {
            out = std::move(value);
          }
        }
      }
      g_key_file_free(keyFile);
      return out;
    }

  } // namespace

  std::optional<std::string> familyOfFontName(std::string_view fontName) {
    const std::string name(fontName);
    PangoFontDescription* desc = pango_font_description_from_string(name.c_str());
    if (desc == nullptr) {
      return std::nullopt;
    }
    std::optional<std::string> out;
    if (const char* family = pango_font_description_get_family(desc); family != nullptr) {
      if (std::string trimmed = StringUtils::trim(family); !trimmed.empty()) {
        out = std::move(trimmed);
      }
    }
    pango_font_description_free(desc);
    return out;
  }

  std::optional<std::string> readFamily() {
    if (auto name = dconf::readString(kFontNameKey)) {
      if (auto family = familyOfFontName(*name)) {
        return family;
      }
    }
    for (const auto& path : settingsIniFiles()) {
      if (auto name = readSettingsIniFontName(path)) {
        if (auto family = familyOfFontName(*name)) {
          return family;
        }
      }
    }
    return std::nullopt;
  }

  std::vector<std::filesystem::path> watchFiles() {
    std::vector<std::filesystem::path> candidates = settingsIniFiles();
    if (auto database = dconf::userDatabasePath()) {
      // dconf-service replaces this database on every write.
      candidates.push_back(*database);
    }
    return candidates;
  }

  std::string resolveFamily(std::string_view configured, const std::optional<std::string>& system) {
    if (std::string family = StringUtils::trim(configured); !family.empty()) {
      return family;
    }
    return system.value_or(std::string(kFallbackFamily));
  }

} // namespace system_font
