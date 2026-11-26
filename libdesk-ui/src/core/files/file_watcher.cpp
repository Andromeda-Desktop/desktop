#include "core/files/file_watcher.h"

#include "core/deferred_call.h"

#include <memory>
#include <string>

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
      return newName == file;
    case Event::Written:
    case Event::MovedIn:
      return name == file;
    case Event::Changed:
    case Event::Created:
      return trigger == Trigger::Modified && name == file;
    case Event::Deleted:
      return false;
    }
    return false;
  }

  struct Watch {
    FileWatcher::Callback callback;
    bool queued = false;
  };
} // namespace

FileWatcher::WatchId FileWatcher::watch(const std::filesystem::path& filePath, Callback callback, WatchTrigger trigger) {
  auto watch = std::make_shared<Watch>(Watch{.callback = std::move(callback)});
  // The subscriber owns `watch`: after unwatch a queued call finds it gone and does nothing.
  return m_dirs.watch(filePath.parent_path(),
                      [watch, trigger, file = filePath.filename().string()](Event event, const std::string& name,
                                                                            const std::string& newName) {
                        if (watch->queued || !matches(trigger, event, file, name, newName)) {
                          return;
                        }
                        watch->queued = true;
                        DeferredCall::callLater([weak = std::weak_ptr<Watch>(watch)] {
                          if (const auto live = weak.lock()) {
                            live->queued = false;
                            live->callback();
                          }
                        });
                      },
                      DirectoryWatcher::Follow::Path);
}
