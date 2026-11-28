#include "theme/desktop_theme.h"

#include "core/files/file_watcher.h"
#include "core/log.h"
#include "design_tokens.h"
#include "theme/builtin_palettes.h"
#include "theme/theme_store.h"

#include <algorithm>
#include <format>
#include <utility>

namespace noctalia::theme {

  namespace {

    constexpr Logger kLog("desktop-theme");

    // The base value of a number key: its first token in the dev token file, else its compiled value.
    std::optional<double> builtInNumber(const ThemeVocabulary& vocabulary, const desk::tokens::SlotValues& devBase,
                                        std::string_view key) {
      const auto entry = vocabulary.keys.find(key);
      if (entry == vocabulary.keys.end() || entry->second.tokens.empty()) {
        return std::nullopt;
      }
      if (const auto dev = devBase.find(entry->second.tokens.front()); dev != devBase.end()) {
        if (const auto* number = std::get_if<double>(&dev->second)) {
          return *number;
        }
      }
      for (const auto& slot : Tokens::slots()) {
        if (slot.path == entry->second.tokens.front() && (slot.kind == 'f' || slot.kind == 'i')) {
          return slot.number;
        }
      }
      return std::nullopt;
    }

    std::string describe(const std::vector<ThemeIssue>& issues) {
      std::string text;
      for (const auto& issue : issues) {
        text += std::format("{}: {}: {}\n", issue.source, issue.key, issue.message);
      }
      return text;
    }

    // The dev token file when it parses and every expression resolves.
    std::optional<std::pair<dev_tokens::File, desk::tokens::SlotValues>> readDevFile(const std::filesystem::path& path) {
      auto file = dev_tokens::readFile(path);
      if (!file) {
        kLog.warn("{}: {}", path.string(), file.error());
        return std::nullopt;
      }
      auto base = dev_tokens::resolve(*file, {});
      if (!base) {
        kLog.warn("{}: {}", path.string(), base.error());
        return std::nullopt;
      }
      return std::pair{std::move(*file), std::move(*base)};
    }

  } // namespace

  DesktopTheme::DesktopTheme(FileWatcher& watcher)
      : m_vocabulary(shippedVocabulary()),
        m_controller(
            {.loadTheme = [this](std::string_view id) { return loadTheme(defaultThemeRoots(), id, m_vocabulary); },
             .loadOverrides = [this]() -> std::expected<std::optional<ThemePackage>, ThemeIssue> {
               auto text = readThemeOverridesText(themeOverridesPath());
               if (!text) {
                 return std::unexpected(std::move(text.error()));
               }
               m_overridesText = std::move(*text);
               return parseThemeOverridesText(m_overridesText, m_vocabulary);
             }},
            m_vocabulary, [this](std::string_view key) { return builtInNumber(m_vocabulary, m_devBase, key); },
            [this](const ResolvedTheme& table, std::uint64_t) { return show(table); }
        ),
        m_watcher(watcher) {
    const auto devPath = dev_tokens::envPath();
    if (!devPath.empty()) {
      if (auto dev = readDevFile(devPath); dev && write(dev->first, {})) {
        m_devFile = std::move(dev->first);
        m_devBase = std::move(dev->second);
        kLog.info("loaded design tokens from {}", devPath.string());
      } else {
        kLog.warn("{}: using built-in tokens", devPath.string());
      }
    }
    m_controller.overridesChanged();
    logErrors();

    m_fileWatches.push_back(m_watcher.watch(
        themeOverridesPath(), [this] { watchedChange([this] { m_controller.overridesChanged(); }); },
        FileWatcher::WatchTrigger::WriteCompleted
    ));
    if (!devPath.empty()) {
      m_fileWatches.push_back(m_watcher.watch(
          devPath,
          [this, devPath] {
            watchedChange([this, &devPath] {
              auto dev = readDevFile(devPath);
              if (!dev || !write(dev->first, m_themeValues)) {
                kLog.warn("{}: keeping the last good tokens", devPath.string());
                return;
              }
              m_devFile = std::move(dev->first);
              m_devBase = std::move(dev->second);
              m_controller.themeFilesChanged(); // relative theme values resolve over the new base
            });
          },
          FileWatcher::WatchTrigger::WriteCompleted
      ));
    }
  }

  DesktopTheme::~DesktopTheme() {
    for (const int watch : m_fileWatches) {
      m_watcher.unwatch(watch);
    }
    for (const auto& [id, watches] : m_themeWatches) {
      for (const int watch : watches) {
        m_watcher.unwatch(watch);
      }
    }
  }

  void DesktopTheme::setChangeCallback(std::function<void()> callback) { m_onChanged = std::move(callback); }

  bool DesktopTheme::sync(std::string_view id, const std::optional<std::string>& overridesText) {
    const std::uint64_t written = m_written;
    if (overridesText != m_overridesText) {
      m_controller.overridesChanged();
    }
    if (id != m_controller.selected()) {
      m_controller.select(id);
    }
    logErrors();
    rewatchThemes();
    return m_written != written;
  }

  std::optional<std::string_view> DesktopTheme::pinnedScheme() const noexcept {
    if (m_schemes.size() != 1) {
      return std::nullopt;
    }
    return m_schemes.front();
  }

  std::expected<void, std::string> DesktopTheme::show(const ResolvedTheme& table) {
    desk::tokens::SlotValues values = slotValues(table, m_vocabulary);
    if (auto written = write(m_devFile, values); !written) {
      return written;
    }
    m_themeValues = std::move(values);
    m_schemes = table.schemes;
    if (const std::string issues = describe(table.issues); !issues.empty()) {
      kLog.warn("theme report:\n{}", issues);
    }
    return {};
  }

  std::expected<void, std::string> DesktopTheme::write(const std::optional<dev_tokens::File>& dev,
                                                       const desk::tokens::SlotValues& theme) {
    auto values = dev ? dev_tokens::resolve(*dev, theme) : theme;
    if (!values) {
      return std::unexpected(std::move(values.error()));
    }
    if (auto applied = desk::tokens::applySlotValues(*values); !applied) {
      return applied;
    }
    refreshBuiltinPalettes();
    ++m_written;
    return {};
  }

  void DesktopTheme::logErrors() {
    std::string errors = describe(m_controller.errors());
    if (errors != m_loggedErrors && !errors.empty()) {
      kLog.warn("theme not shown as is:\n{}", errors);
    }
    m_loggedErrors = std::move(errors);
  }

  void DesktopTheme::watchedChange(const std::function<void()>& step) {
    const std::uint64_t written = m_written;
    step();
    logErrors();
    rewatchThemes();
    if (m_written != written && m_onChanged) {
      m_onChanged();
    }
  }

  void DesktopTheme::rewatchThemes() {
    const std::vector<std::string> ids = m_controller.watchedIds();
    for (auto it = m_themeWatches.begin(); it != m_themeWatches.end();) {
      if (std::ranges::contains(ids, it->first)) {
        ++it;
        continue;
      }
      for (const int watch : it->second) {
        m_watcher.unwatch(watch);
      }
      it = m_themeWatches.erase(it);
    }
    const ThemeRoots roots = defaultThemeRoots();
    for (const std::string& id : ids) {
      if (m_themeWatches.contains(id)) {
        continue;
      }
      auto& watches = m_themeWatches[id];
      for (const auto& root : {roots.user, roots.system}) {
        watches.push_back(m_watcher.watch(
            root / id / "theme.toml", [this] { watchedChange([this] { m_controller.themeFilesChanged(); }); },
            FileWatcher::WatchTrigger::WriteCompleted
        ));
      }
    }
  }

} // namespace noctalia::theme
