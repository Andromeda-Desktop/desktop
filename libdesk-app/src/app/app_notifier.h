#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class DeskApp;

struct AppNotice {
  std::string title; // empty: the app name
  std::string body;
  std::uint8_t urgency = 1; // 0 low, 1 normal, 2 critical
  std::vector<std::string> buttons;
  std::string icon; // empty: the app's icon
  bool progress = false; // stays until replaced or closed, and leaves no history entry
};

// The app's desktop notifications: org.freedesktop.Notifications on Linux (app_notifier_dbus.cpp),
// UNUserNotificationCenter on macOS (app_notifier_macos.mm). A click runs onAction with the xdg-activation token the
// server sent (empty on macOS) and the button (0: the body, n: AppNotice::buttons[n - 1]). A non-empty key replaces or
// closes an earlier notification with the same key; while one Notify for a key is out, the newest notice for it waits
// and replaces it. A notice still being sent holds a DeskApp lease.
class AppNotifier {
public:
  explicit AppNotifier(DeskApp& app);
  ~AppNotifier();
  AppNotifier(const AppNotifier&) = delete;
  AppNotifier& operator=(const AppNotifier&) = delete;

  using ActionFn = std::function<void(const std::string& token, int button)>;
  void notify(const AppNotice& notice, const std::string& key, ActionFn onAction, std::function<void()> onClosed = {});
  void close(const std::string& key);

  struct Impl;

private:
  std::unique_ptr<Impl> m_impl;
};
