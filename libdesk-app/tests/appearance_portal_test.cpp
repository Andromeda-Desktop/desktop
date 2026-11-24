// Runs under dbus-run-session (meson.build): the shell's SettingsPortal and an app's Appearance on one private bus.
#include "app/appearance.h"
#include "dbus/portal/settings_portal.h"
#include "dbus/session_bus.h"
#include "ui/style.h"

#include <chrono>
#include <print>
#include <thread>

namespace {
  int g_failures = 0;

  template <typename F> bool pumpUntil(SessionBus& shell, SessionBus& app, F done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
      if (std::chrono::steady_clock::now() > until) {
        return false;
      }
      shell.processPendingEvents();
      app.processPendingEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
  }

  void expectCorner(SessionBus& shell, SessionBus& app, float want, const char* what) {
    if (!pumpUntil(shell, app, [want] { return Style::cornerRadiusScale() == want; })) {
      std::println(stderr, "appearance_portal_test: {}: corner scale {}, want {}", what, Style::cornerRadiusScale(), want);
      ++g_failures;
    }
  }
} // namespace

int main() {
  SessionBus shellBus;
  SettingsPortal portal(shellBus);
  portal.setScales(1.0, 1.0, 1.5);

  SessionBus appBus;
  int changes = 0;
  Appearance appearance(appBus, [&changes] { ++changes; });
  expectCorner(shellBus, appBus, 1.5F, "an app that starts reads the shell's corner scale");

  const int before = changes;
  portal.setScales(1.0, 1.0, 0.5);
  expectCorner(shellBus, appBus, 0.5F, "a live change reaches a running app");
  if (changes == before) {
    std::println(stderr, "appearance_portal_test: a corner scale change did not rebuild the windows");
    ++g_failures;
  }

  portal.setScales(1.0, 1.0, 3.0);
  expectCorner(shellBus, appBus, 2.0F, "a value above the range clamps to 2");
  return g_failures == 0 ? 0 : 1;
}
