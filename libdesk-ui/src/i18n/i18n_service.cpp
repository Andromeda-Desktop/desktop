#include "i18n/i18n_service.h"

#include "core/files/resource_paths.h"
#include "core/log.h"
#include "i18n/language_tag.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

namespace i18n {

  namespace {

    constexpr Logger kLog("i18n");

    // Reads $LC_ALL / $LC_MESSAGES / $LANG. Returns empty string if unset.
    std::string detectSystemLanguage() {
      for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(var);
        if (value != nullptr && value[0] != '\0') {
          return detail::normalizeLanguageTag(value);
        }
      }
      return {};
    }

    std::string resolveRequestedLanguage(std::string_view preferredLang) {
      return preferredLang.empty() ? detectSystemLanguage() : detail::normalizeLanguageTag(preferredLang);
    }

  } // namespace

  Service& Service::instance() {
    static Service s_instance;
    return s_instance;
  }

  bool mergeCatalogFile(const std::filesystem::path& path, Catalog& out, std::string_view prefix) {
    std::string error;
    const CatalogRead read = readCatalogFile(path, out, prefix, &error);
    if (read == CatalogRead::Invalid) {
      kLog.warn("catalog {}: {}", path.string(), error);
    }
    return read == CatalogRead::Ok;
  }

  bool Service::loadCatalog(std::string_view lang, Catalog& out) const {
    Catalog fresh;
    if (!mergeCatalogFile(paths::assetPath("translations/" + std::string(lang) + ".json"), fresh)) {
      return false;
    }
    out = std::move(fresh);
    return true;
  }

  void Service::init(std::string_view preferredLang) {
    m_requestedLanguage = resolveRequestedLanguage(preferredLang);
    m_loaded = true;
    loadShellCatalogs(m_requestedLanguage);
    for (const Domain& domain : m_domains) {
      loadDomain(domain.name, domain.dir);
    }
  }

  void Service::addDomain(std::string domain, const std::filesystem::path& sourceDir) {
    std::filesystem::path dir = paths::translationsDir(domain, sourceDir);
    m_domains.push_back({std::move(domain), std::move(dir)});
    if (m_loaded) {
      loadDomain(m_domains.back().name, m_domains.back().dir);
    }
  }

  void Service::loadDomain(const std::string& domain, const std::filesystem::path& dir) {
    if (!mergeCatalogFile(dir / "en.json", m_fallback, domain)) {
      kLog.warn("no English catalog for {} in {}", domain, dir.string());
    }
    if (m_language != "en") {
      mergeCatalogFile(dir / (m_language + ".json"), m_active, domain);
    }
  }

  void Service::loadShellCatalogs(const std::string& candidate) {
    // English is always loaded as the fallback; for English itself the active catalog stays empty.
    if (!loadCatalog("en", m_fallback)) {
      kLog.warn("could not load English fallback catalog");
    }

    if (candidate.empty() || candidate == "en") {
      m_active.clear();
      m_language = "en";
      m_rtl = false;
      kLog.info("language: en");
      return;
    }

    for (const std::string& lang : detail::catalogLanguageCandidates(candidate)) {
      // lookup() falls back to m_fallback, so English keeps one copy of the catalog.
      if (lang == "en") {
        m_active.clear();
      }
      if (lang == "en" || loadCatalog(lang, m_active)) {
        m_language = lang;
        m_rtl = detail::isRtlLanguage(m_language);
        if (lang == candidate) {
          kLog.info("language: {}", m_language);
        } else {
          kLog.info("language: {} (from {})", m_language, candidate);
        }
        return;
      }
    }

    kLog.warn("no catalog for '{}', falling back to English", candidate);
    m_active.clear();
    m_language = "en";
    m_rtl = false;
  }

  void Service::setLanguage(std::string_view lang) {
    // Compare the resolved request, not the loaded catalog: an empty (auto) or regional tag
    // resolves to a different catalog name and would otherwise reload on every config reload.
    if (m_loaded && resolveRequestedLanguage(lang) == m_requestedLanguage) {
      return;
    }
    init(lang);
  }

  std::string_view Service::lookup(std::string_view dottedKey) const {
    if (auto it = m_active.find(dottedKey); it != m_active.end()) {
      return it->second;
    }
    if (auto it = m_fallback.find(dottedKey); it != m_fallback.end()) {
      return it->second;
    }
    return {};
  }

} // namespace i18n
