#include "scripting/plugin_i18n.h"

#include "core/log.h"
#include "i18n/i18n.h"

#include <vector>

namespace scripting {

  namespace {
    constexpr Logger kLog("plugin-i18n");
  } // namespace

  void PluginTranslationCatalog::load(const std::filesystem::path& pluginDir) {
    m_values.clear();
    if (pluginDir.empty()) {
      return;
    }

    const std::filesystem::path dir = pluginDir / "translations";
    i18n::mergeCatalogFile(dir / "en.json", m_values);
    if (const std::string_view lang = i18n::Service::instance().language(); !lang.empty() && lang != "en") {
      i18n::mergeCatalogFile(dir / (std::string(lang) + ".json"), m_values);
    }
  }

  bool PluginTranslationCatalog::has(std::string_view key) const { return m_values.contains(key); }

  std::string PluginTranslationCatalog::translate(
      std::string_view key, const std::unordered_map<std::string, std::string>& subst
  ) const {
    const auto it = m_values.find(key);
    if (it == m_values.end()) {
      kLog.warn("plugin translation key '{}' not found", key);
      return std::string(key);
    }
    std::vector<i18n::detail::Pair> pairs;
    pairs.reserve(subst.size());
    for (const auto& [name, value] : subst) {
      pairs.emplace_back(name, value);
    }
    return i18n::detail::interpolate(it->second, pairs);
  }

} // namespace scripting
