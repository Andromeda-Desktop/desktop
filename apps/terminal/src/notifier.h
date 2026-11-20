#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "osc99.h"

class SessionBus;

namespace term {

  // Desktop notifications: org.freedesktop.Notifications on Linux (notifier_dbus.cpp), UNUserNotificationCenter on
  // macOS (notifier_macos.mm; bus is unused). A click runs onAction with the xdg-activation token the server sent
  // (empty on macOS) and the button (0: the body, n: Notice::buttons[n - 1]). A non-empty key replaces or closes an
  // earlier notification with the same key.
  class Notifier {
  public:
    explicit Notifier(SessionBus* bus);
    ~Notifier();
    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;

    using ActionFn = std::function<void(const std::string& token, int button)>;
    void notify(const Notice& notice, const std::string& key, ActionFn onAction, std::function<void()> onClosed = {});
    void close(const std::string& key);

    struct Impl;

  private:
    std::unique_ptr<Impl> m_impl;
  };

} // namespace term
