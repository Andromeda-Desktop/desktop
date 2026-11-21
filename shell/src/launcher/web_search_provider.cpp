#include "launcher/web_search_provider.h"

#include "compositors/compositor_platform.h"
#include "i18n/i18n.h"
#include "net/url_open.h"
#include "shell/panel/panel_manager.h"

#include <gio/gio.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>

namespace {

  std::string percentEncode(std::string_view text) {
    std::string out;
    for (const unsigned char c : text) {
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'
          || c == '.' || c == '~') {
        out.push_back(static_cast<char>(c));
      } else {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%%%02X", c);
        out += buf;
      }
    }
    return out;
  }

  std::optional<nlohmann::json> readJson(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
      return std::nullopt;
    }
    auto json = nlohmann::json::parse(in, nullptr, false);
    if (json.is_discarded()) {
      return std::nullopt;
    }
    return std::optional<nlohmann::json>(std::in_place, std::move(json));
  }

  // Profile directory (under the XDG config dir) of a Chromium-family browser, by desktop id prefix.
  std::optional<std::string> chromiumConfigDir(std::string_view desktopId) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 9> kDirs{{
        {"vivaldi-snapshot", "vivaldi-snapshot"},
        {"vivaldi", "vivaldi"},
        {"google-chrome-beta", "google-chrome-beta"},
        {"google-chrome-unstable", "google-chrome-unstable"},
        {"google-chrome", "google-chrome"},
        {"chromium", "chromium"},
        {"brave-browser", "BraveSoftware/Brave-Browser"},
        {"microsoft-edge", "microsoft-edge"},
        {"opera", "opera"},
    }};
    for (const auto& [prefix, dir] : kDirs) {
      if (desktopId.starts_with(prefix)) {
        return std::string(dir);
      }
    }
    return std::nullopt;
  }

  // The default browser's own search engine URL template, with {searchTerms} still in it.
  std::optional<std::string> browserSearchTemplate() {
    GAppInfo* app = g_app_info_get_default_for_uri_scheme("https");
    if (app == nullptr) {
      return std::nullopt;
    }
    const char* id = g_app_info_get_id(app);
    const auto dir = chromiumConfigDir(id != nullptr ? id : "");
    g_object_unref(app);
    if (!dir) {
      return std::nullopt;
    }
    const std::filesystem::path root = std::filesystem::path(g_get_user_config_dir()) / *dir;
    std::string profile = "Default";
    if (const auto state = readJson(root / "Local State")) {
      const auto lastUsed = state->value(nlohmann::json::json_pointer("/profile/last_used"), std::string{});
      if (!lastUsed.empty()) {
        profile = lastUsed;
      }
    }
    const auto prefs = readJson(root / profile / "Preferences");
    if (!prefs) {
      return std::nullopt;
    }
    auto url = prefs->value(
        nlohmann::json::json_pointer("/default_search_provider_data/mirrored_template_url_data/url"), std::string{}
    );
    if (url.find("{searchTerms}") == std::string::npos) {
      return std::nullopt;
    }
    return url;
  }

  // Chromium template syntax: {searchTerms}, {google:baseURL}, {inputEncoding}; every other {param} is optional.
  std::string expandTemplate(const std::string& tmpl, std::string_view encodedQuery) {
    std::string out;
    for (std::size_t i = 0; i < tmpl.size();) {
      const std::size_t close = tmpl[i] == '{' ? tmpl.find('}', i) : std::string::npos;
      if (close == std::string::npos) {
        out.push_back(tmpl[i++]);
        continue;
      }
      const std::string_view name(tmpl.data() + i + 1, close - i - 1);
      if (name == "searchTerms") {
        out += encodedQuery;
      } else if (name == "google:baseURL") {
        out += "https://www.google.com/";
      } else if (name == "inputEncoding") {
        out += "UTF-8";
      }
      i = close + 1;
    }
    return out;
  }

} // namespace

std::string WebSearchProvider::displayName() const { return i18n::tr("launcher.providers.web.title"); }

std::vector<LauncherResult> WebSearchProvider::query(std::string_view text) const {
  if (text.empty()) {
    return {};
  }
  LauncherResult result;
  result.id = std::string(text);
  result.title = i18n::tr("launcher.pill.search-web", "query", std::string(text));
  result.glyphName = std::string(defaultGlyphName());
  return {std::move(result)};
}

bool WebSearchProvider::activate(const LauncherResult& result) {
  const std::string query = percentEncode(result.id);
  const auto tmpl = browserSearchTemplate();
  std::string url = tmpl ? expandTemplate(*tmpl, query) : "https://duckduckgo.com/?q=" + query;
  // A token from the launcher surface lets the browser raise its window, also on another workspace.
  if (m_platform == nullptr || !m_platform->hasXdgActivation()) {
    return net::openInBrowser(url);
  }
  wl_surface* surface = nullptr;
  if (auto context = PanelManager::instance().popupParentContextForPanel("launcher"); context.has_value()) {
    surface = WaylandSurface::wlOf(context->surface);
  }
  m_platform->requestActivationTokenAsync(surface, {}, [url = std::move(url)](std::string token) {
    (void)net::openInBrowser(url, token);
  });
  return true;
}
