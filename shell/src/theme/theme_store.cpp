#include "theme/theme_store.h"

#include "core/log.h"
#include "protocols/async_file_writer.h"
#include "protocols/xdg_dirs.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

namespace noctalia::theme {

  namespace fs = std::filesystem;

  namespace {

    constexpr Logger kLog("theme-store");

#pragma GCC diagnostic push
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wc23-extensions"
#else
#pragma GCC diagnostic ignored "-Wc++26-extensions"
#endif
    constexpr char kVocabularyText[] = {
#embed "../../protocols/theme-vocabulary.toml"
        , '\0'};
#pragma GCC diagnostic pop

    std::string fromEnv(const char* name) {
      const char* value = std::getenv(name);
      return value != nullptr && value[0] != '\0' ? std::string(value) : std::string{};
    }

    ThemeIssue folderIssue(const fs::path& path, std::string message) {
      return {.key = path.filename().string(), .message = std::move(message)};
    }

    std::optional<std::string> readText(const fs::path& path) {
      std::ifstream file(path, std::ios::binary);
      if (!file) {
        return std::nullopt;
      }
      return std::string(std::istreambuf_iterator<char>(file), {});
    }

  } // namespace

  const ThemeVocabulary& shippedVocabulary() {
    static const ThemeVocabulary vocabulary = [] {
      auto parsed = parseThemeVocabulary(kVocabularyText);
      if (!parsed) {
        kLog.error("theme vocabulary: {}: {}", parsed.error().key, parsed.error().message);
        return ThemeVocabulary{};
      }
      return std::move(*parsed);
    }();
    return vocabulary;
  }

  ThemeRoots defaultThemeRoots() {
    std::string data = fromEnv("ANDROMEDA_DATA_HOME");
    if (data.empty()) {
      data = xdg::dataHome();
    }
    return {.user = fs::path(data) / "andromeda/desktop-themes", .system = "/usr/share/andromeda/desktop-themes"};
  }

  fs::path themeOverridesPath() {
    std::string config = fromEnv("ANDROMEDA_CONFIG_HOME");
    if (config.empty()) {
      config = xdg::configHome();
    }
    return fs::path(config) / "andromeda/theme-overrides.toml";
  }

  std::expected<void, ThemeIssue> checkThemeFolder(const fs::path& folder, const ThemeFolderLimits& limits) {
    std::error_code ec;
    const fs::path root = fs::canonical(folder, ec);
    if (ec) {
      return std::unexpected(folderIssue(folder, std::format("cannot open: {}", ec.message())));
    }
    std::uintmax_t bytes = 0;
    std::size_t files = 0;
    fs::recursive_directory_iterator it(root, fs::directory_options::none, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      const fs::path relative = fs::relative(it->path(), root);
      if (++files > limits.maxFiles) {
        return std::unexpected(folderIssue(folder, std::format("more than {} files", limits.maxFiles)));
      }
      const fs::file_status link = it->symlink_status(ec);
      if (ec) {
        break;
      }
      if (fs::is_symlink(link)) {
        const fs::path target = fs::weakly_canonical(it->path(), ec);
        if (ec || std::ranges::mismatch(root, target).in1 != root.end()) {
          return std::unexpected(folderIssue(folder, std::format("{} links outside the folder", relative.string())));
        }
        continue; // its target is checked where it lies inside the folder
      }
      if (fs::is_directory(link)) {
        continue;
      }
      if (!fs::is_regular_file(link)) {
        return std::unexpected(folderIssue(folder, std::format("{} is not a regular file", relative.string())));
      }
      bytes += it->file_size(ec);
      if (bytes > limits.maxBytes) {
        return std::unexpected(folderIssue(folder, std::format("larger than {} bytes", limits.maxBytes)));
      }
    }
    if (ec) {
      return std::unexpected(folderIssue(folder, std::format("cannot read: {}", ec.message())));
    }
    return {};
  }

  std::expected<ThemePackage, ThemeIssue> loadTheme(const ThemeRoots& roots, std::string_view id,
                                                    const ThemeVocabulary& vocabulary, const ThemeFolderLimits& limits) {
    if (!isThemeId(id)) {
      return std::unexpected(ThemeIssue{.key = "theme.id", .message = std::format("'{}' is not a theme id", id)});
    }
    for (const fs::path& root : {roots.user, roots.system}) {
      const fs::path folder = root / id;
      std::error_code ec;
      if (root.empty() || !fs::exists(folder, ec)) {
        continue;
      }
      if (auto checked = checkThemeFolder(folder, limits); !checked) {
        return std::unexpected(std::move(checked.error()));
      }
      const auto text = readText(folder / "theme.toml");
      if (!text) {
        return std::unexpected(ThemeIssue{.key = "theme", .message = "no theme.toml"});
      }
      return parseThemePackage(*text, id, vocabulary);
    }
    return std::unexpected(ThemeIssue{.key = "theme", .message = std::format("'{}' is not installed", id)});
  }

  std::expected<std::optional<std::string>, ThemeIssue> readThemeOverridesText(const fs::path& path) {
    if (auto pending = andromeda::pendingFileText(path)) {
      return std::optional(std::move(*pending));
    }
    std::error_code ec;
    if (!fs::exists(path, ec)) {
      return std::optional<std::string>{};
    }
    auto text = readText(path);
    if (!text) {
      return std::unexpected(ThemeIssue{.key = "overrides", .message = "cannot read theme-overrides.toml"});
    }
    return text;
  }

  std::expected<std::optional<ThemePackage>, ThemeIssue> loadThemeOverrides(const fs::path& path,
                                                                            const ThemeVocabulary& vocabulary) {
    auto text = readThemeOverridesText(path);
    if (!text) {
      return std::unexpected(std::move(text.error()));
    }
    return parseThemeOverridesText(*text, vocabulary);
  }

  std::expected<std::optional<ThemePackage>, ThemeIssue> parseThemeOverridesText(const std::optional<std::string>& text,
                                                                                 const ThemeVocabulary& vocabulary) {
    if (!text) {
      return std::optional<ThemePackage>{};
    }
    auto parsed = parseThemeOverrides(*text, vocabulary);
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    return std::optional(std::move(*parsed));
  }

} // namespace noctalia::theme
