// Runs under dbus-run-session (meson.build): the shell's SettingsPortal and an app's Appearance on one private bus.
#include "app/appearance.h"
#include "dbus/portal/settings_portal.h"
#include "dbus/session_bus.h"
#include "protocols/pure_black.h"
#include "render/scene/node.h"
#include "i18n/i18n_service.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <chrono>
#include <cmath>
#include <memory>
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

  // Rulebook §4.3: the dark view sits on black, the dark card keeps its L* distance from view, light never changes.
  void expectPureBlack(const char* what) {
    const Palette dark = tokenPalette(false);
    const auto lstar = [](const Color& c) { return pure_black::lightness(c.r, c.g, c.b); };
    const double cardLstar = lstar(dark.card) - lstar(dark.view);
    const Palette& live = paletteForTone(PaletteTone::Dark);
    if (!(live.view == rgba(0.0F, 0.0F, 0.0F, dark.view.a)) || std::abs(lstar(live.card) - cardLstar) > 0.5
        || !(paletteForTone(PaletteTone::Light).view == tokenPalette(true).view)) {
      std::println(stderr, "appearance_portal_test: {} did not apply", what);
      ++g_failures;
    }
  }
} // namespace

int main() {
  SessionBus shellBus;
  auto portal = std::make_unique<SettingsPortal>(shellBus);
  portal->setScales(1.0, 1.0, 1.5);
  portal->publish();

  SessionBus appBus;
  int changes = 0;
  Appearance appearance(appBus, [&changes] { ++changes; });
  expectCorner(shellBus, appBus, 1.5F, "an app that starts reads the shell's corner scale");

  const int before = changes;
  portal->setScales(1.0, 1.0, 0.5);
  portal->publish();
  expectCorner(shellBus, appBus, 0.5F, "a live change reaches a running app");
  if (changes == before) {
    std::println(stderr, "appearance_portal_test: a corner scale change did not rebuild the windows");
    ++g_failures;
  }

  portal->setScales(1.0, 1.0, 3.0);
  portal->publish();
  expectCorner(shellBus, appBus, 2.0F, "a value above the range clamps to 2");

  const int beforeLanguage = changes;
  portal->setLanguage("de");
  portal->publish();
  if (!pumpUntil(shellBus, appBus, [] { return i18n::Service::instance().requestedLanguage() == "de"; })
      || changes == beforeLanguage) {
    std::println(
        stderr, "appearance_portal_test: the shell language did not reach the app (requested '{}')",
        i18n::Service::instance().requestedLanguage()
    );
    ++g_failures;
  }

  // One published batch of several keys is one generation: the windows rebuild once and take every value.
  const int beforeBatch = changes;
  portal->setScales(1.0, 1.0, 0.75);
  portal->setHighContrast(true);
  portal->setPureBlack(true);
  portal->setLanguage("fr");
  portal->publish();
  if (!pumpUntil(shellBus, appBus, [] { return i18n::Service::instance().requestedLanguage() == "fr"; })) {
    std::println(stderr, "appearance_portal_test: the batch did not reach the app");
    ++g_failures;
  }
  // A later batch arrives after every signal of this one, so the count below is final.
  portal->setLanguage("it");
  portal->publish();
  if (!pumpUntil(shellBus, appBus, [] { return i18n::Service::instance().requestedLanguage() == "it"; })) {
    std::println(stderr, "appearance_portal_test: the marker batch did not reach the app");
    ++g_failures;
  }
  if (changes != beforeBatch + 2) {
    std::println(stderr, "appearance_portal_test: two batches rebuilt the windows {} times, want 2", changes - beforeBatch);
    ++g_failures;
  }
  if (Style::cornerRadiusScale() != 0.75F || !(palette.label2 == palette.label1)) {
    std::println(stderr, "appearance_portal_test: the batch's corner scale or high contrast did not apply");
    ++g_failures;
  }
  expectPureBlack("the batch's pure black");

  // A restarted shell counts its generations from 1 again and has set only the scheme so far: the app takes the new
  // scheme and keeps the values the new shell has not set yet.
  portal.reset();
  SessionBus restartBus;
  SettingsPortal restarted(restartBus);
  restarted.setDark(false);
  restarted.publish();
  if (!pumpUntil(restartBus, appBus, [] { return isResolvedLightTheme(); })) {
    std::println(stderr, "appearance_portal_test: the restarted shell's scheme did not reach the app");
    ++g_failures;
  }
  if (Style::cornerRadiusScale() != 0.75F || i18n::Service::instance().requestedLanguage() != "it") {
    std::println(stderr, "appearance_portal_test: the restarted shell reset values it has not set");
    ++g_failures;
  }
  expectPureBlack("pure black after a shell restart");
  return g_failures == 0 ? 0 : 1;
}
