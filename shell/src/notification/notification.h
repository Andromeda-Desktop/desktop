#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

enum class Urgency : uint8_t {
  Low = 0,
  Normal = 1,
  Critical = 2,
};

// org.freedesktop.Notifications close reason codes
enum class CloseReason : uint32_t {
  Expired = 1,
  Dismissed = 2,
  ClosedByCall = 3,
};

enum class NotificationOrigin : uint8_t {
  External = 0, // org.freedesktop.Notifications
  Internal = 1,
  Portal = 2, // org.freedesktop.impl.portal.Notification
};

enum class NotificationDndPolicy : uint8_t {
  Respect = 0,
  ShowToast = 1,
  Bypass = 2,
};

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using WallClock = std::chrono::system_clock;
using WallTimePoint = WallClock::time_point;

struct NotificationImageData {
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::int32_t rowStride = 0;
  bool hasAlpha = true;
  std::int32_t bitsPerSample = 8;
  std::int32_t channels = 4;
  std::vector<std::uint8_t> data;

  bool operator==(const NotificationImageData&) const = default;
};

// Who may replace, close, list or invoke a notification. Host processes share one trust domain (same user);
// a sandboxed app is its app id; a caller the bus cannot name is its connection only. Shell (in-process and the
// portal) is never reachable from the fdo D-Bus API.
struct NotificationOwner {
  enum class Kind : std::uint8_t { Shell, Host, App, Connection };
  Kind kind = Kind::Shell;
  std::string id; // App: the app id; Connection: the unique name

  bool operator==(const NotificationOwner&) const = default;
};

struct Notification {
  uint32_t id;
  NotificationOrigin origin;
  NotificationDndPolicy dndPolicy = NotificationDndPolicy::Respect;
  bool transient = false;
  // Opt-in for internal notifications that must survive being missed. External notifications are
  // already kept; internal ones are toast-only unless they set this.
  bool persistInHistory = false;
  bool hideContent = false; // the screen was shared when it arrived: the banner shows no summary, body or image
  std::string appName;
  std::string summary;
  std::string body;
  NotificationOwner owner;
  std::string sender; // D-Bus destination of the notification's signals; empty broadcasts
  int32_t timeout;
  Urgency urgency;
  std::vector<std::string> actions; // pairs: [key, label, key, label, ...]
  std::optional<std::string> icon;
  std::optional<NotificationImageData> imageData;
  std::optional<std::string> category;
  std::optional<std::string> desktopEntry;
  // x-andromeda-settings-page hint: the default action opens this Settings page.
  std::optional<std::string> settingsPage;
  std::optional<std::string> group;    // hint x-andromeda-group "<source>/<app>"; absent = group by app name
  TimePoint receivedTime;              // add/replace time used for duplicate burst suppression
  std::optional<TimePoint> expiryTime; // absent = never expires
  /// Wall-clock receive time (history UI, persistence). Optional for pre-migration / corrupt rows.
  std::optional<WallTimePoint> receivedWallClock;
  /// Wall-clock expiry when timeout-based; mirrors expiryTime where applicable.
  std::optional<WallTimePoint> expiryWallClock;
};

// Action pairs [key, label, ...]; a trailing key without a label is no action.
[[nodiscard]] inline bool hasAction(const Notification& notification, std::string_view key) {
  for (std::size_t i = 0; i + 1 < notification.actions.size(); i += 2) {
    if (notification.actions[i] == key) {
      return true;
    }
  }
  return false;
}

// The first `max` actions with a label, without "default" (the click on the notification itself).
[[nodiscard]] inline std::vector<std::pair<std::string, std::string>>
namedActions(const Notification& notification, std::size_t max) {
  std::vector<std::pair<std::string, std::string>> out;
  const auto& actions = notification.actions;
  for (std::size_t i = 0; i + 1 < actions.size() && out.size() < max; i += 2) {
    if (actions[i] != "default" && !actions[i + 1].empty()) {
      out.emplace_back(actions[i], actions[i + 1]);
    }
  }
  return out;
}
