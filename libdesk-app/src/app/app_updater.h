#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

class AppLoop;

// The app's self-update through the andromeda-app-updater helper beside its executable (macOS bundles only).
// It checks at start and every 24 h; onReady gets the version of a verified, staged update.
class AppUpdater {
public:
  virtual ~AppUpdater() = default;
  // Hands the stage to the swap helper, which installs it once this process exits and opens the new app. False when
  // no update is ready or the helper did not start; a failed start keeps the update ready.
  virtual bool restart() = 0;
  // The app ends: a ready update installs after the exit, a running check stops.
  virtual void quit() = 0;
  // The verified version while the Restart button shows.
  [[nodiscard]] virtual std::optional<std::string> readyVersion() const = 0;
};

// Null where the platform updates apps itself (Linux packages) or when manifestUrl is empty.
[[nodiscard]] std::unique_ptr<AppUpdater> makeAppUpdater(AppLoop& loop, const std::string& manifestUrl,
                                                         std::function<void(const std::string& version)> onReady);
