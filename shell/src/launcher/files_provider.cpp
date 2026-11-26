#include "launcher/files_provider.h"

#include "clipboard/clipboard_service.h"
#include "core/blocking_pool.h"
#include "core/freedesktop_thumbnails.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"
#include "launcher/quick_look_client.h"
#include "net/uri.h"
#include "net/url_open.h"
#include "util/string_utils.h"

#include <glib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>

namespace {

  constexpr Logger kLog("files");
  // docs/search.md debounce for async providers: longer than key repeat (25/s = 40 ms), short enough to feel instant.
  constexpr auto kTypingDebounce = std::chrono::milliseconds(60);

  bool isImage(std::string_view path) {
    constexpr std::array<std::string_view, 9> kImages = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp", ".svg", ".jxl", ".avif"};
    std::string lower(path.substr(path.size() > 6 ? path.size() - 6 : 0));
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::ranges::any_of(kImages, [&](std::string_view ext) { return lower.ends_with(ext); });
  }

  // The folder as the user reads it: the home folder as "~".
  std::string displayFolder(const std::string& path) {
    std::string folder = std::filesystem::path(path).parent_path().string();
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
      const std::string_view h(home);
      if (folder == h) {
        return "~";
      }
      if (folder.starts_with(h) && folder.size() > h.size() && folder[h.size()] == '/') {
        return "~" + folder.substr(h.size());
      }
    }
    return folder;
  }

  // One row for a path: name, "~/folder", a file or folder glyph, and the picture when it is one (thumbnails come later).
  LauncherResult fileResult(const std::string& path, bool folder) {
    LauncherResult r;
    r.id = path;
    r.title = std::filesystem::path(path).filename().string();
    r.subtitle = displayFolder(path);
    r.glyphName = folder ? "folder" : "file";
    r.presentation = folder ? "folder" : "file";
    if (!folder && isImage(path)) {
      r.iconPath = path;
      r.presentation = "file-image";
    }
    return r;
  }

  // The shared GTK/Qt recent-files list (Files > Recents), local paths, newest use (else change) first.
  std::vector<std::string> readRecentPaths(const std::string& file) {
    std::vector<std::string> paths;
    GBookmarkFile* bookmarks = g_bookmark_file_new();
    if (g_bookmark_file_load_from_file(bookmarks, file.c_str(), nullptr)) {
      gsize count = 0;
      gchar** uris = g_bookmark_file_get_uris(bookmarks, &count);
      std::vector<std::pair<gint64, std::string>> byUse;
      for (gsize i = 0; i < count; ++i) {
        GDateTime* when = g_bookmark_file_get_visited_date_time(bookmarks, uris[i], nullptr);
        if (when == nullptr) {
          when = g_bookmark_file_get_modified_date_time(bookmarks, uris[i], nullptr);
        }
        if (auto path = uri::pathFromFileUri(uris[i])) {
          byUse.emplace_back(when != nullptr ? g_date_time_to_unix(when) : 0, std::move(*path));
        }
      }
      g_strfreev(uris);
      std::ranges::stable_sort(byUse, std::greater<>{}, &std::pair<gint64, std::string>::first);
      paths.reserve(byUse.size());
      for (auto& [when, path] : byUse) {
        paths.push_back(std::move(path));
      }
    }
    g_bookmark_file_free(bookmarks);
    return paths;
  }

} // namespace

FilesProvider::FilesProvider(ClipboardService* clipboard, SessionBus* bus)
    : m_clipboard(clipboard), m_bus(bus), m_fileManager(bus) {}

FilesProvider::~FilesProvider() { disconnect(); }

std::string FilesProvider::displayName() const { return i18n::tr("launcher.files.title"); }

void FilesProvider::disconnect() const {
  m_client.close();
  m_pending.clear();
  m_requested.reset();
  m_loading = false;
  m_debounce.stop();
}

void FilesProvider::withThumbnails(std::vector<LauncherResult>& results) const {
  std::vector<std::string> missing;
  for (LauncherResult& r : results) {
    if (r.presentation != "file") {
      continue;
    }
    const auto [it, added] = m_thumbnails.try_emplace(r.id);
    if (added) {
      missing.push_back(r.id);
    } else if (it->second.has_value() && !it->second->empty()) {
      r.iconPath = *it->second;
      r.presentation = "file-image";
    }
  }
  if (missing.empty()) {
    return;
  }
  using Found = std::vector<std::pair<std::string, std::string>>;
  BlockingPool::instance().run<Found>(
      m_alive,
      [missing = std::move(missing)] {
        Found found;
        for (const std::string& path : missing) {
          const auto thumb = freedesktop_thumbnails::lookup(path, kThumbnailPx);
          found.emplace_back(path, thumb ? thumb->string() : std::string());
        }
        return found;
      },
      [this](Found found) {
        kLog.debug("thumbnails: {} of {} files have one", std::ranges::count_if(found, [](const auto& f) {
          return !f.second.empty();
        }), found.size());
        for (auto& [path, thumb] : found) {
          m_thumbnails[path] = std::move(thumb);
        }
        withThumbnails(m_results);
        if (m_onResultsChanged) {
          m_onResultsChanged();
        }
      }
  );
}

void FilesProvider::reset() {
  m_debounce.stop();
  m_thumbnails.clear();
  m_requested.reset();
  m_answered.reset();
  m_results.clear();
  m_loading = false;
  m_recentRequest.cancel();
}

std::vector<LauncherResult> FilesProvider::query(std::string_view text) const { return request(text, false); }

std::vector<LauncherResult> FilesProvider::queryPrefixed(std::string_view text) const { return request(text, true); }

std::vector<LauncherResult> FilesProvider::request(std::string_view text, bool scoped) const {
  // The broker matches answers to the text it asked, so the raw text is the key; the daemon gets it trimmed.
  const Request key{std::string(text), scoped ? std::optional(m_scopeKind) : std::nullopt};
  std::string needle = StringUtils::trim(text);
  std::erase(needle, '\n');
  m_recentRequest.cancel();
  if (scoped && needle.empty() && m_scopeKind.empty()) {
    m_answered = key.first;
    m_answeredScope = key.second;
    m_requested.reset();
    m_debounce.stop();
    loadRecentFiles();
    return m_results;
  }
  if (needle.size() < kMinQuery && (!scoped || m_scopeKind.empty() || !needle.empty())) {
    m_answered = key.first;
    m_answeredScope = key.second;
    m_requested.reset();
    m_results.clear();
    m_loading = false;
    m_debounce.stop();
    return {};
  }
  if (m_answered == key.first && m_answeredScope == key.second) {
    // Back to an answered query: an answer still on its way is for another text now.
    m_requested.reset();
    m_loading = false;
    m_debounce.stop();
    return m_results;
  }
  // Until the new answer arrives, the last one stays, so the section does not blink out on each keystroke.
  if (m_requested == key) {
    return m_results;
  }
  const std::string kind = scoped ? m_scopeKind : std::string();
  const int limit = scoped ? kScopedLimit : kLimit;
  m_requested = key;
  m_loading = true;
  m_debounce.start(needle.empty() ? std::chrono::milliseconds(0) : kTypingDebounce, [this, key, limit, kind] {
    send(key, limit, kind);
  });
  return m_results;
}

void FilesProvider::loadRecentFiles() const {
  m_loading = true;
  using Recent = std::vector<std::pair<std::string, bool>>;
  BlockingPool::instance().run<Recent>(
      m_alive,
      [file = (std::filesystem::path(g_get_user_data_dir()) / "recently-used.xbel").string()] {
        Recent recent;
        std::error_code ec;
        if (!std::filesystem::exists(file, ec)) {
          return recent;
        }
        for (const std::string& path : readRecentPaths(file)) {
          if (recent.size() >= static_cast<std::size_t>(kScopedLimit)) {
            break;
          }
          const auto status = std::filesystem::status(path, ec);
          if (!ec && std::filesystem::exists(status)) {
            recent.emplace_back(path, std::filesystem::is_directory(status));
          }
        }
        return recent;
      },
      [this, ticket = m_recentRequest.begin()](Recent recent) {
        if (!m_recentRequest.current(ticket)) {
          return;
        }
        m_results.clear();
        for (const auto& [path, folder] : recent) {
          LauncherResult r = fileResult(path, folder);
          r.category = i18n::tr("launcher.pill.recent");
          m_results.push_back(std::move(r));
        }
        m_loading = false;
        withThumbnails(m_results);
        if (m_onResultsChanged) {
          m_onResultsChanged();
        }
      }
  );
}

void FilesProvider::send(const Request& request, int limit, const std::string& kind) const {
  // Without the daemon there is no Files section; the next query tries to connect again.
  if (!m_client.query(limit, request.first, kind)) {
    disconnect();
    m_answered = request.first;
    m_answeredScope = request.second;
    m_results.clear();
    if (m_onResultsChanged) {
      m_onResultsChanged();
    }
    return;
  }
  m_pending.push_back(request);
}

void FilesProvider::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_client.fd() >= 0) {
    fds.push_back({.fd = m_client.fd(), .events = m_client.pollEvents(), .revents = 0});
  }
}

void FilesProvider::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (m_client.fd() < 0 || startIdx >= fds.size() || fds[startIdx].fd != m_client.fd()) {
    return;
  }
  const bool open = m_client.dispatch(fds[startIdx].revents, [this](std::vector<std::string> rows) {
    if (m_pending.empty()) {
      return;
    }
    const Request request = std::move(m_pending.front());
    m_pending.pop_front();
    if (request == m_requested) {
      onAnswer(request, rows);
    }
  });
  if (!open) {
    // The daemon went away: the open query is answered with nothing, so the section stops loading.
    const std::optional<Request> requested = m_requested;
    disconnect();
    if (requested.has_value()) {
      onAnswer(*requested, {});
    }
  }
}

void FilesProvider::onAnswer(const Request& request, const std::vector<std::string>& rows) const {
  const auto& [text, scope] = request;
  const bool scoped = scope.has_value();
  m_results.clear();
  std::vector<std::int64_t> mtimes;
  for (const std::string_view row : rows) {
    // path \t mtime_ns \t size \t f|d \t n|c (name or text match)
    const std::size_t tab = row.find('\t');
    if (tab == std::string_view::npos || row.size() < 4 || row[row.size() - 2] != '\t') {
      continue;
    }
    const std::string path(row.substr(0, tab));
    const bool folder = row.substr(row.size() - 4, 2) == "\td";
    mtimes.push_back(std::atoll(std::string(row.substr(tab + 1)).c_str()));
    LauncherResult r = fileResult(path, folder);
    r.category = displayName();
    m_results.push_back(std::move(r));
  }
  kLog.debug("{} files for \"{}\"{}{}", m_results.size(), text, scoped ? " in the Files scope" : "",
             scoped && !scope->empty() ? " kind " + *scope : std::string());
  if (scoped) {
    std::vector<std::size_t> order(m_results.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
      order[i] = i;
    }
    std::ranges::stable_sort(order, std::greater{}, [&](std::size_t i) { return mtimes[i]; });
    std::vector<LauncherResult> sorted;
    sorted.reserve(order.size());
    for (const std::size_t i : order) {
      sorted.push_back(std::move(m_results[i]));
    }
    m_results = std::move(sorted);
  } else if (!m_results.empty()) {
    LauncherResult all;
    all.id = std::string(kShowAllId);
    all.title = i18n::tr("launcher.files.show-all");
    all.glyphName = "list";
    all.presentation = "action";
    all.category = displayName();
    // Last row of the Files section, whatever the other rows score.
    all.score = std::numeric_limits<double>::lowest();
    m_results.push_back(std::move(all));
  }
  withThumbnails(m_results);
  m_answered = text;
  m_answeredScope = scope;
  m_requested.reset();
  m_loading = false;
  if (m_onResultsChanged) {
    m_onResultsChanged();
  }
}

bool FilesProvider::activate(const LauncherResult& result) {
  if (!net::openInBrowser(uri::fileUri(result.id))) {
    kLog.warn("opening {} failed", result.id);
  }
  return true;
}

void FilesProvider::reveal(const LauncherResult& result) const { m_fileManager.showItem(result.id); }

void FilesProvider::quickLook(const LauncherResult& result) const {
  if (m_bus == nullptr) {
    return;
  }
  if (!m_quickLook) {
    m_quickLook = std::make_unique<QuickLookClient>(*m_bus);
  }
  m_quickLook->toggle(result.id);
}

void FilesProvider::copyPath(const LauncherResult& result) const {
  if (m_clipboard != nullptr) {
    (void)m_clipboard->copyText(result.id);
  }
}
