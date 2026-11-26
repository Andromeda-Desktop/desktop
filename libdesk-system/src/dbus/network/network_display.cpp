#include "dbus/network/network_display.h"

#include "dbus/network/network_types.h"

#include <string_view>

namespace network_display {

  namespace {
    // Connected, but NetworkManager found no way out (none, captive portal, limited). Unknown means "not checked".
    bool noInternet(const NetworkState& state) noexcept {
      return state.connected
          && (state.internet == InternetReach::None || state.internet == InternetReach::Portal
              || state.internet == InternetReach::Limited);
    }
  } // namespace

  GlyphLook glyphLookForState(const NetworkState& state) noexcept {
    if (state.kind == NetworkConnectivity::Wired) {
      if (!state.connected) {
        return {.glyph = "ethernet-off"};
      }
      return {.glyph = noInternet(state) ? "ethernet-exclamation" : "ethernet"};
    }
    if (state.kind == NetworkConnectivity::Cellular) {
      return {.glyph = state.connected ? "cell-signal-1" : cellularOffGlyph()};
    }
    if (state.airplane) {
      return {.glyph = "plane"};
    }
    if (!state.wirelessEnabled) {
      return {.glyph = "wifi-off"};
    }
    if (state.kind == NetworkConnectivity::Wireless && state.connected) {
      if (noInternet(state)) {
        return {.glyph = "wifi-exclamation"};
      }
      const char* glyph = wifiGlyphForSignal(state.signalStrength);
      return {.glyph = glyph, .under = std::string_view(glyph) == "wifi" ? nullptr : "wifi"};
    }
    if (state.resolving) {
      return {.glyph = "wifi", .dim = true, .cycling = true};
    }
    return {.glyph = "wifi", .dim = true};
  }

  const char* glyphForState(const NetworkState& state) noexcept { return glyphLookForState(state).glyph; }

  const char* vpnGlyph() noexcept { return "shield-check"; }

  const char* vpnActivatingGlyph() noexcept { return "shield"; }

  const char* wifiGlyphForState(const NetworkState& state) noexcept {
    if (!state.wirelessEnabled) {
      return "wifi-off";
    }
    if (state.kind == NetworkConnectivity::Wireless && state.connected) {
      return noInternet(state) ? "wifi-exclamation" : wifiGlyphForSignal(state.signalStrength);
    }
    return "wifi";
  }

  int wifiSignalBand(std::uint8_t signal) noexcept {
    if (signal >= 80) {
      return 4;
    }
    if (signal >= 60) {
      return 3;
    }
    if (signal >= 35) {
      return 2;
    }
    if (signal >= 15) {
      return 1;
    }
    return 0;
  }

  const char* wifiGlyphForSignal(std::uint8_t signal) noexcept {
    switch (wifiSignalBand(signal)) {
    case 4:
      return "wifi";
    case 3:
    case 2:
      return "wifi-2";
    default:
      return "wifi-1";
    }
  }

  const char* wifiFrequencyBandLabel(std::uint32_t frequencyMhz) noexcept {
    // Broad envelopes around each band rather than exact channel centers — the input
    // is whatever operating frequency the backend reports. Band membership follows
    // cfg80211: 802.11j 4.9 GHz channels and the U-NII-4/ITS block are 5 GHz, and
    // 5925 MHz is the 5/6 GHz boundary.
    if (frequencyMhz >= 2400 && frequencyMhz <= 2500) {
      return "2.4 GHz";
    }
    if (frequencyMhz >= 4900 && frequencyMhz <= 5924) {
      return "5 GHz";
    }
    if (frequencyMhz >= 5925 && frequencyMhz <= 7125) {
      return "6 GHz";
    }
    if (frequencyMhz >= 57000 && frequencyMhz <= 71000) {
      return "60 GHz";
    }
    return nullptr;
  }

  const char* cellularGlyphForSignal(std::uint8_t signal) noexcept {
    // cell-signal-* rather than antenna-bars-*: the antenna-bars outlines in the
    // vendored tabler.ttf are mangled and render as dots, while cell-signal
    // renders correctly.
    switch (wifiSignalBand(signal)) {
    case 4:
      return "cell-signal-5";
    case 3:
      return "cell-signal-4";
    case 2:
      return "cell-signal-3";
    case 1:
      return "cell-signal-2";
    default:
      return "cell-signal-1";
    }
  }

  const char* cellularOffGlyph() noexcept { return "cell-signal-off"; }

} // namespace network_display
