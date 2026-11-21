#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The desktop's interface font family, as GTK apps see it, without GSettings or a session-bus connection.
namespace system_font {

  // org.gnome.desktop.interface font-name from dconf, else gtk-font-name in the GTK 4 or GTK 3 settings.ini;
  // nullopt when none names a family.
  std::optional<std::string> readFamily();

  // The family of a Pango font name ("Noto Sans Bold 11" -> "Noto Sans"); nullopt when it names none.
  std::optional<std::string> familyOfFontName(std::string_view fontName);

  // Files whose change can change readFamily(), including the targets of symlinked settings.ini files.
  std::vector<std::filesystem::path> watchFiles();

  // The family the shell draws with: `configured`, else `system`, else Inter (the design-token face).
  std::string resolveFamily(std::string_view configured, const std::optional<std::string>& system);

} // namespace system_font
