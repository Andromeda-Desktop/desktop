#include "dbus/network/network_display.h"
#include "dbus/network/network_types.h"

#include <cstring>
#include <print>

namespace {

  bool expectGlyph(const char* actual, const char* expected, const char* message) {
    if (std::strcmp(actual, expected) != 0) {
      std::println(stderr, "network_display_test: {}: expected '{}', got '{}'", message, expected, actual);
      return false;
    }
    return true;
  }

} // namespace

int main() {
  bool ok = true;

  // Cellular bands are the wifi bands shifted onto cell-signal-1..5: tabler has
  // no zero-bar cellular glyph, so the weakest band still draws one bar.
  ok = expectGlyph(network_display::cellularGlyphForSignal(0), "cell-signal-1", "no signal") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(20), "cell-signal-2", "weak signal") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(40), "cell-signal-3", "medium signal") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(70), "cell-signal-4", "strong signal") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(79), "cell-signal-4", "top band lower edge") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(80), "cell-signal-5", "top band") && ok;
  ok = expectGlyph(network_display::cellularGlyphForSignal(100), "cell-signal-5", "full signal") && ok;

  // Cellular kind without a ModemManager signal to band on.
  NetworkState cellular;
  cellular.kind = NetworkConnectivity::Cellular;
  cellular.connected = true;
  ok = expectGlyph(network_display::glyphForState(cellular), "cell-signal-1", "connected cellular fallback") && ok;
  cellular.connected = false;
  ok = expectGlyph(network_display::glyphForState(cellular), "cell-signal-off", "disconnected cellular fallback") && ok;

  // Every Wi-Fi state draws what the approved sheet shows; a connected link with no way out shows the exclamation.
  struct Case {
    const char* name;
    NetworkState state;
    const char* glyph;
    const char* under;
    bool dim;
    bool cycling;
  };
  const auto wifi = [](std::uint8_t signal, InternetReach internet) {
    NetworkState s;
    s.kind = NetworkConnectivity::Wireless;
    s.connected = true;
    s.wirelessEnabled = true;
    s.signalStrength = signal;
    s.internet = internet;
    return s;
  };
  NetworkState connecting;
  connecting.kind = NetworkConnectivity::Wireless;
  connecting.wirelessEnabled = true;
  connecting.resolving = true;
  NetworkState noNetwork;
  noNetwork.wirelessEnabled = true;
  NetworkState off;
  NetworkState airplane;
  airplane.airplane = true;
  NetworkState wired;
  wired.kind = NetworkConnectivity::Wired;
  wired.connected = true;
  wired.internet = InternetReach::Full;
  NetworkState wiredPortal = wired;
  wiredPortal.internet = InternetReach::Portal;
  const Case cases[] = {
      {"strong", wifi(90, InternetReach::Full), "wifi", nullptr, false, false},
      {"medium", wifi(50, InternetReach::Full), "wifi-2", "wifi", false, false},
      {"weak", wifi(10, InternetReach::Full), "wifi-1", "wifi", false, false},
      {"not yet checked", wifi(90, InternetReach::Unknown), "wifi", nullptr, false, false},
      {"no internet", wifi(90, InternetReach::None), "wifi-exclamation", nullptr, false, false},
      {"captive portal", wifi(50, InternetReach::Portal), "wifi-exclamation", nullptr, false, false},
      {"limited", wifi(50, InternetReach::Limited), "wifi-exclamation", nullptr, false, false},
      {"connecting", connecting, "wifi", nullptr, true, true},
      {"no network", noNetwork, "wifi", nullptr, true, false},
      {"wifi off", off, "wifi-off", nullptr, false, false},
      {"airplane", airplane, "plane", nullptr, false, false},
      {"ethernet", wired, "ethernet", nullptr, false, false},
      {"ethernet portal", wiredPortal, "ethernet-exclamation", nullptr, false, false},
  };
  for (const Case& c : cases) {
    const auto look = network_display::glyphLookForState(c.state);
    ok = expectGlyph(look.glyph, c.glyph, c.name) && ok;
    if ((look.under == nullptr) != (c.under == nullptr) || (c.under != nullptr && std::strcmp(look.under, c.under) != 0)
        || look.dim != c.dim || look.cycling != c.cycling) {
      std::println(stderr, "network_display_test: {}: wrong under, dim or cycling", c.name);
      ok = false;
    }
  }
  if (internetReachFromNm(4) != InternetReach::Full || internetReachFromNm(1) != InternetReach::None
      || internetReachFromNm(9) != InternetReach::Unknown) {
    std::println(stderr, "network_display_test: NM connectivity maps to the wrong reach");
    ok = false;
  }

  return ok ? 0 : 1;
}
