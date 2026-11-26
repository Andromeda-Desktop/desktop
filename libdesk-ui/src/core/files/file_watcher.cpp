#include "core/files/file_watcher.h"

#include "core/deferred_call.h"

#include <memory>
#include <string>
#include <utility>

namespace {
  using Event = DirectoryWatcher::Event;
  using Trigger = FileWatcher::WatchTrigger;

  bool matches(Trigger trigger, Event event, const std::string& file, const std::string& name,
               const std::string& newName) {
    switch (event) {
    case Event::Overflow:
    case Event::Gone:
      return true;
    case Event::Renamed:
      return newName == file || (trigger == Trigger::Modified && name == file);
    case Event::Written:
    case Event::MovedIn:
      return name == file;
    case Event::Changed:
    case Event::Created:
    case Event::Deleted:
      return trigger == Trigger::Modified && name == file;
    }
    return false;
  }

  bool touches(Event event, const std::string& file, const std::string& name, const std::string& newName) {
    return event == Event::Overflow || event == Event::Gone || name == file || newName == file;
  }

  std::filesystem::path linkTarget(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_symlink(path, ec)) {
      return {};
    }
    auto target = std::filesystem::canonical(path, ec);
    return ec ? std::filesystem::path{} : target;
  }
} // namespace

struct FileWatcher::Watch {
  Callback callback;
  WatchTrigger trigger;
  std::filesystem::path path;
  std::filesystem::path target; // canonical target while `path` is a symlink
  WatchId targetId = -1;
  bool queued = false;
  bool fire = false;
  bool relink = false;
};

FileWatcher::~FileWatcher() {
  for (const auto& [id, watch] : m_watches) {
    m_dirs.unwatch(id);
    m_dirs.unwatch(watch->targetId);
  }
}

FileWatcher::WatchId FileWatcher::subscribe(const std::shared_ptr<Watch>& watch, const std::filesystem::path& file,
                                            bool isLink) {
  // m_watches alone owns a Watch, so it is the alive token: after unwatch or ~FileWatcher a queued call does nothing.
  return m_dirs.watch(
      file.parent_path(),
      [this, weak = std::weak_ptr<Watch>(watch), isLink, name = file.filename().string()](
          Event event, const std::string& changed, const std::string& newName) {
        const auto state = weak.lock();
        if (!state) {
          return;
        }
        const bool fire = matches(state->trigger, event, name, changed, newName);
        const bool relink = !isLink && touches(event, name, changed, newName);
        if (!fire && !relink) {
          return;
        }
        state->fire |= fire;
        state->relink |= relink;
        if (state->queued) {
          return;
        }
        state->queued = true;
        // Runs outside the DirectoryWatcher dispatch, so it may watch and unwatch.
        DeferredCall::callLaterIfAlive(weak, [this, weak] {
          if (const auto live = weak.lock()) {
            live->queued = false;
            if (std::exchange(live->relink, false)) {
              follow(live);
            }
            if (std::exchange(live->fire, false)) {
              live->callback();
            }
          }
        });
      },
      DirectoryWatcher::Follow::Path
  );
}

void FileWatcher::follow(const std::shared_ptr<Watch>& watch) {
  auto target = linkTarget(watch->path);
  if (target == watch->target) {
    return;
  }
  m_dirs.unwatch(watch->targetId);
  watch->target = std::move(target);
  watch->targetId = watch->target.empty() ? -1 : subscribe(watch, watch->target, true);
}

FileWatcher::WatchId FileWatcher::watch(const std::filesystem::path& filePath, Callback callback,
                                        WatchTrigger trigger) {
  auto watch = std::make_shared<Watch>(Watch{.callback = std::move(callback), .trigger = trigger, .path = filePath});
  const WatchId id = subscribe(watch, filePath, false);
  if (id >= 0) {
    m_watches.emplace(id, watch);
    // Dotfile managers link the file; editors then write the target, not the link's directory.
    follow(watch);
  }
  return id;
}

void FileWatcher::unwatch(WatchId id) {
  m_dirs.unwatch(id);
  if (const auto it = m_watches.find(id); it != m_watches.end()) {
    m_dirs.unwatch(it->second->targetId);
    m_watches.erase(it);
  }
}
