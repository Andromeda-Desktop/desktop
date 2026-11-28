#include "theme/theme_controller.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <utility>

namespace noctalia::theme {

  namespace {

    bool sameTable(const ResolvedTheme& a, const ResolvedTheme& b) {
      return a.chain == b.chain && a.schemes == b.schemes && a.values == b.values && a.origin == b.origin;
    }

    // The colour roles theme.accent_source picks: the overrides set them only with the custom source.
    bool accentRole(std::string_view key) {
      if (!key.starts_with("scheme.")) {
        return false;
      }
      const std::string_view role = key.substr(key.rfind('.') + 1);
      return role == "accent" || role == "on_accent" || role == "accent_text";
    }

    std::string accentKey(std::string_view scheme) { return std::format("scheme.{}.accent", scheme); }

    std::optional<Color> colorAt(const std::map<std::string, ThemeValue>& values, const std::string& key) {
      const auto it = values.find(key);
      const auto* color = it == values.end() ? nullptr : std::get_if<Color>(&it->second);
      return color == nullptr ? std::nullopt : std::optional(*color);
    }

  } // namespace

  ThemeController::ThemeController(Disk disk, ThemeVocabulary vocabulary, BuiltInNumber builtIn, Apply apply)
      : m_disk(std::move(disk)), m_vocabulary(std::move(vocabulary)), m_builtIn(std::move(builtIn)),
        m_apply(std::move(apply)) {}

  void ThemeController::themeFilesChanged() { refresh(); }

  void ThemeController::update(std::string_view id, bool userAccent, bool overridesEdited) {
    const bool loaded = overridesEdited && loadOverrides();
    if (!loaded && id == m_selected && userAccent == m_userAccent) {
      return;
    }
    m_selected = id;
    m_userAccent = userAccent;
    refresh();
  }

  void ThemeController::overridesChanged() {
    if (loadOverrides()) {
      refresh();
    }
  }

  bool ThemeController::loadOverrides() {
    auto loaded = m_disk.loadOverrides();
    if (!loaded) {
      loaded.error().source = "overrides";
      m_overridesError = std::move(loaded.error());
      return false;
    }
    m_overridesError.reset();
    m_overrides = std::move(*loaded);
    return true;
  }

  std::string_view ThemeController::shownId() const noexcept {
    return m_shown.chain.empty() ? std::string_view{} : std::string_view(m_shown.chain.front());
  }

  std::vector<std::string> ThemeController::watchedIds() const {
    std::vector<std::string> ids = m_shown.chain;
    if (!m_selected.empty() && !std::ranges::contains(ids, m_selected)) {
      ids.push_back(m_selected);
    }
    return ids;
  }

  std::vector<ThemeIssue> ThemeController::errors() const {
    std::vector<ThemeIssue> out;
    for (const auto* error : {&m_themeError, &m_overridesError, &m_applyError}) {
      if (error->has_value()) {
        out.push_back(**error);
      }
    }
    return out;
  }

  std::optional<Color> ThemeController::themeAccent(std::string_view scheme) const {
    return colorAt(m_themeAccents, accentKey(scheme));
  }

  std::optional<Color> ThemeController::userAccent(std::string_view scheme) const {
    return m_overrides ? colorAt(m_overrides->values, accentKey(scheme)) : std::nullopt;
  }

  // Resolves without the overrides' accent roles, so the theme's accent stays known, then lays them on top with the
  // custom source. No other key references a colour role, so the result equals a resolve with them.
  void ThemeController::refresh() {
    const ThemeLookup lookup = [this](std::string_view id) { return m_disk.loadTheme(id); };
    std::optional<ThemePackage> overrides = m_overrides;
    std::map<std::string, ThemeValue> userRoles;
    if (overrides) {
      for (auto it = overrides->values.begin(); it != overrides->values.end();) {
        const auto next = std::next(it);
        if (accentRole(it->first)) {
          userRoles.insert(overrides->values.extract(it));
        }
        it = next;
      }
    }
    auto resolved = resolveTheme(m_selected, lookup, overrides ? &*overrides : nullptr, m_vocabulary, m_builtIn);
    if (!resolved) {
      m_themeError = std::move(resolved.error());
      return;
    }
    m_themeError.reset();
    std::map<std::string, ThemeValue> themeAccents;
    for (const std::string_view scheme : {"dark", "light"}) {
      if (const auto it = resolved->values.find(accentKey(scheme)); it != resolved->values.end()) {
        themeAccents.insert(*it);
      }
    }
    if (m_userAccent) {
      for (const auto& [key, value] : userRoles) {
        if (std::holds_alternative<Color>(value)) {
          resolved->values.insert_or_assign(key, value);
          resolved->origin.insert_or_assign(key, "overrides");
        }
      }
    }
    show(std::move(*resolved), std::move(themeAccents));
  }

  void ThemeController::show(ResolvedTheme table, std::map<std::string, ThemeValue> themeAccents) {
    if (sameTable(m_shown, table)) {
      m_applyError.reset();
      m_themeAccents = std::move(themeAccents);
      return;
    }
    if (auto applied = m_apply(table, m_generation + 1); !applied) {
      m_applyError = ThemeIssue{.key = "theme", .message = std::move(applied.error()),
                                .source = table.chain.empty() ? std::string("overrides") : table.chain.front()};
      return;
    }
    m_applyError.reset();
    m_shown = std::move(table);
    m_themeAccents = std::move(themeAccents);
    ++m_generation;
  }

} // namespace noctalia::theme
