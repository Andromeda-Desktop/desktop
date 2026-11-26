#pragma once

#include <cstdint>
#include "protocols/async_file_writer.h"

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct DesktopEntry;

// Reads and writes $XDG_CONFIG_HOME/mimeapps.list's [Default Applications] section (XDG MIME
// Applications spec), the one owner of default-app associations.
namespace default_apps {

  enum class Category : std::uint8_t {
    WebBrowser,
    Mail,
    FileManager,
    Terminal,
    TextEditor,
    ImageViewer,
    VideoPlayer,
    MusicPlayer,
    PdfViewer,
  };

  struct CategorySpec {
    Category category;
    std::string_view mimeType;     // the [Default Applications] key this category is stored under
    std::string_view mainCategory; // Categories= fallback when no app declares this MimeType
  };

  [[nodiscard]] std::span<const CategorySpec> categories();

  [[nodiscard]] std::filesystem::path mimeAppsListPath();

  // Desktop file id ("org.mozilla.firefox.desktop") currently set for a category, empty when unset.
  [[nodiscard]] std::string currentDefault(const std::filesystem::path& path, Category category);
  // The default the desktop uses: the first entry over every mimeapps.list in XDG order (user, desktop-specific
  // such as /usr/share/applications/andromeda-mimeapps.list, system), as xdg-mime and GIO resolve it.
  [[nodiscard]] std::string effectiveDefault(Category category);
  // The user's pick: written to ~/.config/mimeapps.list and to each ~/.config/<desktop>-mimeapps.list that sets the
  // type, since those are read first; so the pick is what effectiveDefault() returns. `done` gets one result for all
  // files on the main loop, with the first failed path.
  using SetDefaultDone = std::function<void(bool ok, const std::filesystem::path& failedPath)>;
  void setUserDefault(Category category, const std::string& desktopId, SetDefaultDone done = {});

  // Desktop entries that either declare the category's MimeType or, failing that, fall back to its
  // main Categories= group (many file managers and terminals declare neither MimeType).
  [[nodiscard]] std::vector<const DesktopEntry*>
  candidatesForCategory(std::span<const DesktopEntry> entries, Category category);

  // Preserves every other line and section in the file; inserts [Default Applications] if missing. False when the
  // category is unknown: nothing is written and `done` is not called.
  [[nodiscard]] bool setDefault(
      const std::filesystem::path& path, Category category, const std::string& desktopId, andromeda::WriteDone done = {}
  );

} // namespace default_apps
