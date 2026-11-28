#include "theme/theme_controller.h"

#include <algorithm>
#include <utility>

namespace noctalia::theme {

  namespace {

    const ResolvedTheme kBuiltIn;

    bool sameTable(const ResolvedTheme& a, const ResolvedTheme& b) {
      return a.chain == b.chain && a.schemes == b.schemes && a.values == b.values && a.origin == b.origin;
    }

  } // namespace

  ThemeController::ThemeController(Disk disk, ThemeVocabulary vocabulary, BuiltInNumber builtIn, Apply apply)
      : m_disk(std::move(disk)), m_vocabulary(std::move(vocabulary)), m_builtIn(std::move(builtIn)),
        m_apply(std::move(apply)) {}

  void ThemeController::select(std::string_view id) {
    m_selected = id;
    refresh();
  }

  void ThemeController::themeFilesChanged() { refresh(); }

  void ThemeController::overridesChanged() {
    auto loaded = m_disk.loadOverrides();
    if (!loaded) {
      loaded.error().source = "overrides";
      m_overridesError = std::move(loaded.error());
      return;
    }
    m_overridesError.reset();
    m_overrides = std::move(*loaded);
    refresh();
  }

  std::string_view ThemeController::shownId() const noexcept {
    const auto* shown = std::get_if<Shown>(&m_shown);
    return shown == nullptr ? std::string_view{} : std::string_view(shown->table.chain.front());
  }

  std::vector<std::string> ThemeController::watchedIds() const {
    std::vector<std::string> ids;
    if (const auto* shown = std::get_if<Shown>(&m_shown)) {
      ids = shown->table.chain;
    }
    if (!m_selected.empty() && !std::ranges::contains(ids, m_selected)) {
      ids.push_back(m_selected);
    }
    return ids;
  }

  std::vector<ThemeIssue> ThemeController::errors() const {
    std::vector<ThemeIssue> out;
    for (const auto* error : {&m_themeError, &m_overridesError}) {
      if (error->has_value()) {
        out.push_back(**error);
      }
    }
    return out;
  }

  void ThemeController::refresh() {
    if (m_selected.empty()) {
      m_themeError.reset();
      show(ResolvedTheme{});
      return;
    }
    const ThemeLookup lookup = [this](std::string_view id) { return m_disk.loadTheme(id); };
    auto resolved = resolveTheme(m_selected, lookup, m_overrides ? &*m_overrides : nullptr, m_vocabulary, m_builtIn);
    if (!resolved) {
      m_themeError = std::move(resolved.error());
      return;
    }
    m_themeError.reset();
    show(std::move(*resolved));
  }

  void ThemeController::show(ResolvedTheme table) {
    const auto* shown = std::get_if<Shown>(&m_shown);
    if (sameTable(shown == nullptr ? kBuiltIn : shown->table, table)) {
      return;
    }
    if (table.chain.empty()) {
      m_shown = BuiltIn{};
    } else {
      m_shown = Shown{.table = std::move(table)};
    }
    ++m_generation;
    const auto* now = std::get_if<Shown>(&m_shown);
    m_apply(now == nullptr ? kBuiltIn : now->table, m_generation);
  }

} // namespace noctalia::theme
