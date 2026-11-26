#include "system/default_apps.h"

#include "config/atomic_file.h"
#include "protocols/async_file_writer.h"
#include "protocols/xdg_dirs.h"
#include "system/desktop_entry.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>

namespace default_apps {

  namespace {


    constexpr std::array<CategorySpec, 9> kCategories{{
        {Category::WebBrowser, "x-scheme-handler/http", "WebBrowser"},
        {Category::Mail, "x-scheme-handler/mailto", "Email"},
        {Category::FileManager, "inode/directory", "FileManager"},
        {Category::Terminal, "x-scheme-handler/terminal", "TerminalEmulator"},
        {Category::TextEditor, "text/plain", "TextEditor"},
        {Category::ImageViewer, "image/png", "Viewer"},
        {Category::VideoPlayer, "video/mp4", "Player"},
        {Category::MusicPlayer, "audio/mpeg", "Player"},
        {Category::PdfViewer, "application/pdf", "Viewer"},
    }};

    std::filesystem::path configHome() {
      const std::string config = xdg::configHome();
      return config.empty() ? std::filesystem::path("/tmp") : std::filesystem::path(config);
    }

    std::vector<std::string> splitMimeList(std::string_view value) {
      std::vector<std::string> parts;
      for (std::size_t start = 0; start <= value.size();) {
        const std::size_t sep = value.find(';', start);
        std::string_view part = value.substr(start, sep - start);
        if (!part.empty()) {
          parts.emplace_back(part);
        }
        if (sep == std::string_view::npos) {
          break;
        }
        start = sep + 1;
      }
      return parts;
    }

  } // namespace

  std::span<const CategorySpec> categories() { return kCategories; }

  std::filesystem::path mimeAppsListPath() { return configHome() / "mimeapps.list"; }

  std::vector<std::string> defaultIds(const std::filesystem::path& path, Category category);

  namespace {
    std::vector<std::string> currentDesktopPrefixes() {
      std::vector<std::string> prefixes;
      const char* value = std::getenv("XDG_CURRENT_DESKTOP");
      std::string_view list = value != nullptr ? value : "";
      while (!list.empty()) {
        const std::size_t sep = list.find(':');
        std::string desktop(list.substr(0, sep));
        list = sep == std::string_view::npos ? std::string_view() : list.substr(sep + 1);
        if (!desktop.empty()) {
          std::ranges::transform(desktop, desktop.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
          prefixes.push_back(desktop + "-");
        }
      }
      return prefixes;
    }
  } // namespace

  bool setUserDefault(Category category, const std::string& desktopId) {
    bool ok = setDefault(mimeAppsListPath(), category, desktopId);
    for (const std::string& prefix : currentDesktopPrefixes()) {
      const std::filesystem::path list = configHome() / (prefix + "mimeapps.list");
      if (!currentDefault(list, category).empty()) {
        ok = setDefault(list, category, desktopId) && ok;
      }
    }
    return ok;
  }

  std::string effectiveDefault(Category category) {
    std::vector<std::filesystem::path> appDirs{std::filesystem::path(xdg::dataHome()) / "applications"};
    for (const std::string& dir : xdg::dataDirs()) {
      appDirs.push_back(std::filesystem::path(dir) / "applications");
    }
    std::vector<std::filesystem::path> dirs{configHome()};
    for (const std::string& dir : xdg::configDirs()) {
      dirs.emplace_back(dir);
    }
    dirs.insert(dirs.end(), appDirs.begin(), appDirs.end());
    std::vector<std::string> prefixes = currentDesktopPrefixes();
    prefixes.emplace_back();
    // An id whose desktop file is not installed is skipped, as xdg-mime and GIO skip it.
    const auto installed = [&appDirs](const std::string& id) {
      std::error_code ec;
      return std::ranges::any_of(appDirs, [&](const auto& dir) { return std::filesystem::is_regular_file(dir / id, ec); });
    };
    for (const auto& dir : dirs) {
      for (const std::string& prefix : prefixes) {
        for (const std::string& id : defaultIds(dir / (prefix + "mimeapps.list"), category)) {
          if (installed(id)) {
            return id;
          }
        }
      }
    }
    return {};
  }

  // The file as the next read must see it: a queued write wins over the disk.
  std::istringstream readList(const std::filesystem::path& path) {
    if (std::optional<std::string> pending = andromeda::pendingFileText(path)) {
      return std::istringstream(std::move(*pending));
    }
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return std::istringstream(std::move(text).str());
  }

  std::vector<std::string> defaultIds(const std::filesystem::path& path, Category category) {
    const auto it = std::ranges::find(kCategories, category, &CategorySpec::category);
    if (it == kCategories.end()) {
      return {};
    }
    std::istringstream in = readList(path);
    std::string line;
    bool inSection = false;
    while (std::getline(in, line)) {
      if (!line.empty() && line.front() == '[') {
        inSection = line == "[Default Applications]";
        continue;
      }
      if (!inSection) {
        continue;
      }
      const std::size_t eq = line.find('=');
      if (eq == std::string::npos || line.compare(0, eq, it->mimeType) != 0) {
        continue;
      }
      return splitMimeList(std::string_view(line).substr(eq + 1));
    }
    return {};
  }

  std::string currentDefault(const std::filesystem::path& path, Category category) {
    const std::vector<std::string> ids = defaultIds(path, category);
    return ids.empty() ? std::string() : ids.front();
  }

  std::vector<const DesktopEntry*> candidatesForCategory(std::span<const DesktopEntry> entries, Category category) {
    const auto it = std::ranges::find(kCategories, category, &CategorySpec::category);
    if (it == kCategories.end()) {
      return {};
    }
    std::vector<const DesktopEntry*> byMime;
    std::vector<const DesktopEntry*> byCategory;
    for (const DesktopEntry& entry : entries) {
      if (entry.exec.empty()) {
        continue;
      }
      if (!entry.mimeTypes.empty()) {
        for (const std::string& mime : splitMimeList(entry.mimeTypes)) {
          if (mime == it->mimeType) {
            byMime.push_back(&entry);
            break;
          }
        }
      } else if (entry.categories.find(std::string(it->mainCategory)) != std::string::npos) {
        byCategory.push_back(&entry);
      }
    }
    return byMime.empty() ? byCategory : byMime;
  }

  bool setDefault(const std::filesystem::path& path, Category category, const std::string& desktopId) {
    const auto it = std::ranges::find(kCategories, category, &CategorySpec::category);
    if (it == kCategories.end()) {
      return false;
    }
    std::vector<std::string> lines;
    {
      std::istringstream in = readList(path);
      std::string line;
      while (std::getline(in, line)) {
        lines.push_back(line);
      }
    }

    std::optional<std::size_t> sectionStart;
    std::optional<std::size_t> sectionEnd; // exclusive
    std::optional<std::size_t> keyLine;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (!lines[i].empty() && lines[i].front() == '[') {
        if (sectionStart.has_value() && !sectionEnd.has_value()) {
          sectionEnd = i;
        }
        if (lines[i] == "[Default Applications]") {
          sectionStart = i;
        }
        continue;
      }
      if (sectionStart.has_value() && !sectionEnd.has_value()) {
        const std::size_t eq = lines[i].find('=');
        if (eq != std::string::npos && lines[i].compare(0, eq, it->mimeType) == 0) {
          keyLine = i;
        }
      }
    }
    if (sectionStart.has_value() && !sectionEnd.has_value()) {
      sectionEnd = lines.size();
    }

    const std::string newLine = std::string(it->mimeType) + "=" + desktopId + ";";
    if (keyLine.has_value()) {
      lines[*keyLine] = newLine;
    } else if (sectionStart.has_value()) {
      lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(*sectionEnd), newLine);
    } else {
      if (!lines.empty()) {
        lines.emplace_back();
      }
      lines.emplace_back("[Default Applications]");
      lines.push_back(newLine);
    }

    std::string text;
    for (const std::string& line : lines) {
      text += line + '\n';
    }
    writeTextFileAtomicAsync(path, std::move(text));
    return true;
  }

} // namespace default_apps
