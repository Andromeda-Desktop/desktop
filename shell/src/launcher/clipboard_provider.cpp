#include "launcher/clipboard_provider.h"

#include "clipboard/clipboard_service.h"
#include "i18n/i18n.h"
#include "net/url_open.h"
#include "time/time_format.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>

namespace {

  constexpr std::string_view kIdPrefix = "clip-";

  std::string firstLine(std::string_view text) {
    const auto trimmed = StringUtils::trim(text);
    const auto end = trimmed.find('\n');
    return std::string(end == std::string_view::npos ? trimmed : trimmed.substr(0, end));
  }

  // Groups in the order the launcher draws them.
  std::string dayGroup(const ClipboardEntry& entry) {
    if (entry.pinned) {
      return i18n::tr("launcher.clipboard.pinned");
    }
    const auto age = std::chrono::system_clock::now() - entry.capturedAt;
    if (age < std::chrono::hours(24)) {
      return i18n::tr("launcher.clipboard.today");
    }
    if (age < std::chrono::hours(48)) {
      return i18n::tr("launcher.clipboard.yesterday");
    }
    return i18n::tr("launcher.clipboard.earlier");
  }

  std::string kindGlyph(const ClipboardEntry& entry, std::string_view text) {
    if (entry.isImage()) {
      return "photo";
    }
    if (entry.isFiles()) {
      return "files";
    }
    if (text.starts_with("http://") || text.starts_with("https://")) {
      return "link";
    }
    if (text.size() == 7 && text.front() == '#') {
      return "palette";
    }
    return "file-text";
  }

} // namespace

std::string ClipboardProvider::displayName() const { return i18n::tr("launcher.providers.clipboard.title"); }

namespace {
  // Image rows that get a thumbnail: the newest ones, about one screen of rows.
  constexpr std::size_t kMaxThumbs = 8;
} // namespace

std::vector<LauncherResult> ClipboardProvider::query(std::string_view text) const {
  std::vector<LauncherResult> results;
  if (m_clipboard == nullptr) {
    return results;
  }
  const std::string needle = StringUtils::toLower(StringUtils::trim(text));
  const auto& history = m_clipboard->history();
  results.reserve(history.size());
  warmThumbnails();
  // Pinned entries first, then newest first; history() keeps newest at the front.
  for (const bool pinnedPass : {true, false}) {
    for (const ClipboardEntry& entry : history) {
      if (entry.pinned != pinnedPass) {
        continue;
      }
      std::string line;
      if (entry.isImage()) {
        line = i18n::tr("launcher.clipboard.image");
      } else if (entry.isFiles()) {
        const auto files = ClipboardService::parseFilesPreview(entry.textPreview);
        const std::string name = files.paths.empty() ? std::string() : std::filesystem::path(files.paths.front()).filename().string();
        line = files.count > 1 ? i18n::tr("clipboard.entry.files", "name", name, "count", files.count - 1) : name;
      } else {
        line = firstLine(entry.textPreview);
      }
      if (!needle.empty() && !StringUtils::toLower(entry.textPreview).contains(needle)) {
        continue;
      }
      LauncherResult r;
      r.id = std::string(kIdPrefix) + entry.storageId;
      r.title = line;
      r.subtitle = formatTimeAgo(entry.capturedAt);
      r.glyphName = kindGlyph(entry, entry.textPreview);
      r.category = dayGroup(entry);
      r.presentation = entry.isImage() ? "clipboard-image" : entry.isFiles() ? "clipboard-files" : "clipboard-text";
      r.pinned = entry.pinned;
      if (const auto thumb = m_thumbs.find(entry.storageId); entry.isImage() && thumb != m_thumbs.end()) {
        r.iconPath = thumb->second;
      }
      results.push_back(std::move(r));
    }
  }
  return results;
}

void ClipboardProvider::warmThumbnails() const {
  if (m_clipboard == nullptr) {
    return;
  }
  const auto& history = m_clipboard->history();
  // A thumbnail is a whole image as a data URI: drop those whose entry left the history.
  std::erase_if(m_thumbs, [&history](const auto& thumb) {
    return std::ranges::none_of(history, [&thumb](const ClipboardEntry& entry) { return entry.storageId == thumb.first; });
  });
  std::size_t count = 0;
  for (const bool pinnedPass : {true, false}) {
    for (const ClipboardEntry& entry : history) {
      if (entry.pinned != pinnedPass || !entry.isImage() || count == kMaxThumbs) {
        continue;
      }
      ++count;
      if (!m_thumbs.contains(entry.storageId) && !std::ranges::contains(m_pendingThumbs, entry.storageId)) {
        m_pendingThumbs.push_back(entry.storageId);
      }
    }
  }
  if (!m_pendingThumbs.empty() && !m_thumbTimer.active()) {
    m_thumbTimer.start(std::chrono::milliseconds(0), [this] { loadNextThumbnail(); });
  }
}

void ClipboardProvider::loadNextThumbnail() const {
  if (m_pendingThumbs.empty() || m_clipboard == nullptr) {
    return;
  }
  const std::string id = m_pendingThumbs.front();
  m_pendingThumbs.erase(m_pendingThumbs.begin());
  // Decrypt and base64 run on the SharedWorker; the next thumbnail starts when this one lands.
  m_clipboard->imageDataUriAsync(id, [this, id, alive = std::weak_ptr<int>(m_alive)](std::optional<std::string> uri) {
    if (alive.expired()) {
      return;
    }
    if (uri) {
      m_thumbs[id] = std::move(*uri);
    }
    if (!m_pendingThumbs.empty()) {
      m_thumbTimer.start(std::chrono::milliseconds(0), [this] { loadNextThumbnail(); });
    } else if (m_onResultsChanged) {
      m_onResultsChanged();
    }
  });
}

std::size_t ClipboardProvider::indexOf(const LauncherResult& result) const {
  if (m_clipboard == nullptr || !result.id.starts_with(kIdPrefix)) {
    return static_cast<std::size_t>(-1);
  }
  return indexOfStorageId(std::string_view(result.id).substr(kIdPrefix.size()));
}

std::size_t ClipboardProvider::indexOfStorageId(std::string_view storageId) const {
  const auto& history = m_clipboard->history();
  for (std::size_t i = 0; i < history.size(); ++i) {
    if (history[i].storageId == storageId) {
      return i;
    }
  }
  return static_cast<std::size_t>(-1);
}

bool ClipboardProvider::activate(const LauncherResult& result) {
  const std::size_t index = indexOf(result);
  if (index == static_cast<std::size_t>(-1)) {
    return false;
  }
  const auto copy = [this](const ClipboardEntry& loaded) {
    const ClipboardEntry entry = loaded;
    if (!entry.pinned) {
      if (const std::size_t at = indexOfStorageId(entry.storageId); at != static_cast<std::size_t>(-1)) {
        (void)m_clipboard->promoteEntry(at);
      }
    }
    return m_clipboard->copyEntry(entry);
  };
  const ClipboardEntry& entry = m_clipboard->history()[index];
  if (entry.payloadLoaded) {
    return !entry.data.empty() && copy(entry);
  }
  // The payload decrypts on the storage worker; the launcher closes through the done callback, as for async providers.
  m_clipboard->withEntryPayload(
      entry.storageId, [this, copy, id = result.id, alive = std::weak_ptr<int>(m_alive)](const ClipboardEntry* loaded) {
        if (!alive.expired() && loaded != nullptr && copy(*loaded) && m_onActivationDone) {
          m_onActivationDone(id, true);
        }
      }
  );
  return false;
}

bool ClipboardProvider::togglePinned(const LauncherResult& result) {
  const std::size_t index = indexOf(result);
  return index != static_cast<std::size_t>(-1) && m_clipboard->setEntryPinned(index, !m_clipboard->history()[index].pinned);
}

bool ClipboardProvider::remove(const LauncherResult& result) {
  const std::size_t index = indexOf(result);
  return index != static_cast<std::size_t>(-1) && m_clipboard->removeHistoryEntry(index);
}

void ClipboardProvider::openImage(const LauncherResult& result) {
  const std::size_t index = indexOf(result);
  if (index == static_cast<std::size_t>(-1) || !m_clipboard->history()[index].isImage()) {
    return;
  }
  m_clipboard->exportEntryAsync(
      m_clipboard->history()[index].storageId,
      [this, id = result.id, alive = std::weak_ptr<int>(m_alive)](std::optional<std::string> path) {
        if (!alive.expired() && path.has_value() && net::openFile(*path) && m_onActivationDone) {
          m_onActivationDone(id, false);
        }
      }
  );
}

std::string ClipboardProvider::text(const LauncherResult& result) const {
  const std::size_t index = indexOf(result);
  if (index == static_cast<std::size_t>(-1)) {
    return {};
  }
  const ClipboardEntry& entry = m_clipboard->history()[index];
  if (!entry.isFiles()) {
    return entry.textPreview;
  }
  std::string paths;
  for (const std::string_view path : ClipboardService::parseFilesPreview(entry.textPreview).paths) {
    paths += (paths.empty() ? "" : "\n") + std::string(path);
  }
  return paths;
}

void ClipboardProvider::withFilePaths(
    const LauncherResult& result, std::function<bool(const std::vector<std::filesystem::path>&)> use
) {
  const std::size_t index = indexOf(result);
  if (index == static_cast<std::size_t>(-1) || !m_clipboard->history()[index].isFiles()) {
    return;
  }
  m_clipboard->withEntryPayload(
      m_clipboard->history()[index].storageId,
      [this, use = std::move(use), id = result.id, alive = std::weak_ptr<int>(m_alive)](const ClipboardEntry* loaded) {
        if (alive.expired() || loaded == nullptr) {
          return;
        }
        const auto paths = ClipboardService::filePaths(loaded->data.span());
        if (!paths.empty() && use(paths) && m_onActivationDone) {
          m_onActivationDone(id, false);
        }
      }
  );
}

void ClipboardProvider::copyPaths(const LauncherResult& result) {
  withFilePaths(result, [this](const std::vector<std::filesystem::path>& paths) {
    std::string text;
    for (const auto& path : paths) {
      text += (text.empty() ? "" : "\n") + path.string();
    }
    return m_clipboard->copyText(std::move(text));
  });
}

void ClipboardProvider::reveal(const LauncherResult& result) {
  withFilePaths(result, [this](const std::vector<std::filesystem::path>& paths) {
    m_fileManager.showItem(paths.front());
    return true;
  });
}

void ClipboardProvider::requestThumbnail(const LauncherResult& result) {
  if (!result.id.starts_with(kIdPrefix)) {
    return;
  }
  const std::string id = result.id.substr(kIdPrefix.size());
  if (m_thumbs.contains(id) || std::ranges::contains(m_pendingThumbs, id)) {
    return;
  }
  m_pendingThumbs.insert(m_pendingThumbs.begin(), id);
  if (!m_thumbTimer.active()) {
    m_thumbTimer.start(std::chrono::milliseconds(0), [this] { loadNextThumbnail(); });
  }
}

std::size_t ClipboardProvider::byteSize(const LauncherResult& result) const {
  const std::size_t index = indexOf(result);
  return index == static_cast<std::size_t>(-1) ? 0 : m_clipboard->history()[index].byteSize;
}
