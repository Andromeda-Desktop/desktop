#include "system/icon_theme_poll_source.h"

#include "core/files/directory_watcher.h"
#include "system/icon_resolver.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <poll.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace {
  using Clock = std::chrono::steady_clock;

  constexpr std::chrono::seconds kCheckInterval{60};
  constexpr std::chrono::milliseconds kDebounceInterval{100};

  enum WatchRole : std::uint8_t {
    WATCH_BASE_DIR = 1U << 0U,
    WATCH_THEME_ROOT = 1U << 1U,
    WATCH_ICON_DIR = 1U << 2U,
    WATCH_SETTINGS_DIR = 1U << 3U,
    WATCH_SETTINGS_ANCESTOR = 1U << 4U,
  };

  struct DesiredWatch {
    std::uint8_t roles = 0;
    std::unordered_set<std::string> settingsFiles;
    std::unordered_set<std::string> missingChildren;
  };

  struct ActiveWatch {
    DirectoryWatcher::WatchId id = -1;
    std::uint8_t roles = 0;
    std::unordered_set<std::string> settingsFiles;
    std::unordered_set<std::string> missingChildren;
  };

  bool hasRole(std::uint8_t roles, WatchRole role) { return (roles & static_cast<std::uint8_t>(role)) != 0; }

  bool isIconFile(std::string_view name) { return name.ends_with(".svg") || name.ends_with(".png"); }

  int millisecondsUntil(Clock::time_point deadline, Clock::time_point now) {
    if (now >= deadline) {
      return 0;
    }
    return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count());
  }
} // namespace

struct IconThemePollSource::Impl {
  DirectoryWatcher& dirs;
  std::function<void()> changeCallback;
  std::unordered_map<std::string, ActiveWatch> watches; // by directory
  Clock::time_point nextCheck = Clock::now() + kCheckInterval;
  Clock::time_point debounceUntil;
  bool contentPending = false;
  bool metadataPending = false;
  // The 60 s re-check stats every icon dir; it runs only when inotify could not watch them all.
  bool watchesComplete = false;

  explicit Impl(DirectoryWatcher& watcher) : dirs(watcher) { resetWatches(); }
  ~Impl() { clearWatches(); }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  void clearWatches() {
    for (const auto& [path, watch] : watches) {
      dirs.unwatch(watch.id);
    }
    watches.clear();
  }

  void resetWatches() {
    clearWatches();
    watchesComplete = true;

    std::unordered_map<std::string, DesiredWatch> desired;
    const auto paths = IconResolver::themeWatchPaths();
    for (const auto& path : paths.baseDirs) {
      desired[path].roles |= WATCH_BASE_DIR;
    }
    for (const auto& path : paths.themeRoots) {
      desired[path].roles |= WATCH_THEME_ROOT;
    }
    for (const auto& path : paths.iconDirs) {
      desired[path].roles |= WATCH_ICON_DIR;
    }
    for (const auto& path : paths.settingsFiles) {
      const fs::path settingsPath(path);
      std::error_code ec;
      if (fs::is_directory(settingsPath.parent_path(), ec)) {
        auto& watch = desired[settingsPath.parent_path().string()];
        watch.roles |= WATCH_SETTINGS_DIR;
        watch.settingsFiles.emplace(settingsPath.filename().string());
        continue;
      }
      // A missing settings dir (dconf creates ~/.config/dconf on its first write) is watched through its nearest
      // existing ancestor, so the creation resets the watches without a periodic re-check.
      fs::path child = settingsPath.parent_path();
      fs::path ancestor = child.parent_path();
      while (ancestor != child && !fs::is_directory(ancestor, ec)) {
        child = ancestor;
        ancestor = ancestor.parent_path();
      }
      auto& watch = desired[ancestor.string()];
      watch.roles |= WATCH_SETTINGS_ANCESTOR;
      watch.missingChildren.emplace(child.filename().string());
    }

    for (auto& [path, watch] : desired) {
      std::error_code ec;
      if (!fs::is_directory(path, ec)) {
        continue;
      }
      const auto id = dirs.watch(path, [this, path](DirectoryWatcher::Event event, const std::string& name,
                                                    const std::string& newName) {
        handleEvent(path, event, name, newName);
      });
      watchesComplete = watchesComplete && id >= 0;
      if (id >= 0) {
        watches[path] = ActiveWatch{.id = id,
                                    .roles = watch.roles,
                                    .settingsFiles = std::move(watch.settingsFiles),
                                    .missingChildren = std::move(watch.missingChildren)};
      }
    }
  }

  void queueChange(bool content, bool metadata) {
    contentPending = contentPending || content;
    metadataPending = metadataPending || metadata;
    debounceUntil = Clock::now() + kDebounceInterval;
  }

  void handleEvent(const std::string& path, DirectoryWatcher::Event event, const std::string& name,
                   const std::string& newName) {
    using Event = DirectoryWatcher::Event;
    if (event == Event::Overflow) {
      queueChange(true, true);
      return;
    }
    const auto it = watches.find(path);
    if (it == watches.end()) {
      return;
    }
    if (event == Event::Gone) {
      queueChange(false, true);
      return;
    }
    // A removed or moved-away directory reports Gone on its own watch; here only arrivals count as directory changes.
    const auto isDirectory = [&path](const std::string& child) {
      std::error_code ec;
      return fs::is_directory(fs::path(path) / child, ec);
    };
    switch (event) {
    case Event::Written:
    case Event::Deleted:
      handleName(it->second, name, true, false);
      break;
    case Event::Created:
      handleName(it->second, name, false, isDirectory(name));
      break;
    case Event::MovedIn:
      handleName(it->second, name, true, isDirectory(name));
      break;
    case Event::Renamed:
      handleName(it->second, name, true, false);
      handleName(it->second, newName, true, isDirectory(newName));
      break;
    default:
      break;
    }
  }

  void handleName(const ActiveWatch& watch, std::string_view name, bool completedFileChange, bool directoryChange) {
    bool content = false;
    bool metadata = false;
    if (hasRole(watch.roles, WATCH_SETTINGS_DIR)
        && completedFileChange
        && watch.settingsFiles.contains(std::string(name))) {
      metadata = true;
    }
    if (hasRole(watch.roles, WATCH_SETTINGS_ANCESTOR)
        && directoryChange
        && watch.missingChildren.contains(std::string(name))) {
      metadata = true;
    }
    if (hasRole(watch.roles, WATCH_BASE_DIR) && directoryChange) {
      metadata = true;
    }
    if (hasRole(watch.roles, WATCH_THEME_ROOT) && completedFileChange) {
      metadata = metadata || name == "index.theme";
    }
    if (hasRole(watch.roles, WATCH_ICON_DIR)) {
      content = content
          || (completedFileChange && isIconFile(name))
          || (hasRole(watch.roles, WATCH_THEME_ROOT) && directoryChange);
    }
    if (content || metadata) {
      queueChange(content, metadata);
    }
  }

  void applyPendingChange() {
    const bool hadContent = std::exchange(contentPending, false);
    const bool hadMetadata = std::exchange(metadataPending, false);
    bool changed = false;
    if (hadContent) {
      IconResolver::invalidateThemeContent();
      changed = true;
    } else if (hadMetadata) {
      changed = IconResolver::checkThemePlanChanged();
    }
    resetWatches();
    if (changed && changeCallback) {
      changeCallback();
    }
  }
};

IconThemePollSource::IconThemePollSource(DirectoryWatcher& dirs) : m_impl(std::make_unique<Impl>(dirs)) {}

IconThemePollSource::~IconThemePollSource() = default;

void IconThemePollSource::setChangeCallback(std::function<void()> callback) {
  m_impl->changeCallback = std::move(callback);
}

int IconThemePollSource::pollTimeoutMs() const {
  const auto now = Clock::now();
  int timeout = m_impl->watchesComplete ? -1 : millisecondsUntil(m_impl->nextCheck, now);
  if (m_impl->contentPending || m_impl->metadataPending) {
    const int debounce = millisecondsUntil(m_impl->debounceUntil, now);
    timeout = timeout < 0 ? debounce : std::min(timeout, debounce);
  }
  return timeout;
}

void IconThemePollSource::dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) {
  auto now = Clock::now();
  if ((m_impl->contentPending || m_impl->metadataPending) && now >= m_impl->debounceUntil) {
    m_impl->applyPendingChange();
    now = Clock::now();
  }
  if (!m_impl->watchesComplete && now >= m_impl->nextCheck) {
    m_impl->nextCheck = now + kCheckInterval;
    const bool changed = IconResolver::checkThemeChanged();
    if (changed) {
      m_impl->resetWatches();
      if (m_impl->changeCallback) {
        m_impl->changeCallback();
      }
    }
  }
}

void IconThemePollSource::doAddPollFds(std::vector<pollfd>& /*fds*/) {}
