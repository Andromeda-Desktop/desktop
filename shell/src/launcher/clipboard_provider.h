#pragma once

#include "core/timer_manager.h"
#include "dbus/file_manager1.h"
#include "launcher/launcher_provider.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class ClipboardService;
class SessionBus;

// Clipboard history as a launcher mode (DESIGN.md "Launcher media modes"): newest first, pinned on top.
class ClipboardProvider : public LauncherProvider {
public:
  ClipboardProvider(ClipboardService* clipboard, SessionBus* bus) : m_clipboard(clipboard), m_fileManager(bus) {}

  [[nodiscard]] std::string_view defaultPrefix() const override { return "clip"; }
  [[nodiscard]] std::string_view id() const override { return "Clipboard"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "clipboard"; }
  [[nodiscard]] bool supportsAutoPaste() const override { return true; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  void setResultsChangedCallback(std::function<void()> callback) override { m_onResultsChanged = std::move(callback); }

  // An unloaded payload decrypts first: activate returns false and the done callback closes the launcher.
  bool activate(const LauncherResult& result) override;
  void setActivationDoneCallback(std::function<void(std::string, bool)> callback) override {
    m_onActivationDone = std::move(callback);
  }
  // Pin or unpin, and delete, by result id; the launcher's preview buttons call these.
  bool togglePinned(const LauncherResult& result);
  bool remove(const LauncherResult& result);
  // Opens an image entry in the default image viewer, from a plain copy in the state dir. These three finish through
  // the activation done callback (nothing copied) once the payload is read.
  void openImage(const LauncherResult& result);
  // A copied-files entry: copies its paths as text, one per line; shows the first one in its folder.
  void copyPaths(const LauncherResult& result);
  void reveal(const LauncherResult& result);
  // The stored text preview of a text entry (the row title is only its first line).
  [[nodiscard]] std::string text(const LauncherResult& result) const;
  // Loads the result's image data URI next (front of the queue); the results refresh when it lands.
  void requestThumbnail(const LauncherResult& result);
  // Loads the thumbnails of the newest image entries in the background, so the next open lists them in its first
  // frame. Call it when the history changes.
  void warmThumbnails() const;
  // Payload size in bytes, for the preview details.
  [[nodiscard]] std::size_t byteSize(const LauncherResult& result) const;

private:
  [[nodiscard]] std::size_t indexOf(const LauncherResult& result) const;
  [[nodiscard]] std::size_t indexOfStorageId(std::string_view storageId) const;
  void loadNextThumbnail() const;

  // Runs `use` with the entry's file paths once its payload is read; finishes the activation when `use` succeeds.
  void withFilePaths(const LauncherResult& result, std::function<bool(const std::vector<std::filesystem::path>&)> use);

  ClipboardService* m_clipboard = nullptr;
  FileManager1 m_fileManager;
  std::function<void()> m_onResultsChanged;
  std::function<void(std::string, bool)> m_onActivationDone;
  // Row thumbnails as data URIs (the texture cache decodes them off the main thread), by storage id. They load one
  // per timer tick when the history changes: a payload read decrypts it from disk.
  mutable std::unordered_map<std::string, std::string> m_thumbs;
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
  mutable std::vector<std::string> m_pendingThumbs;
  mutable Timer m_thumbTimer;
};
