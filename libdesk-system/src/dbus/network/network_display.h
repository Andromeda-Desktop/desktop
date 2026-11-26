#pragma once

#include <cstdint>

struct NetworkState;

namespace network_display {

  // How the bar draws a network state: the glyph, a dim full glyph under it for the unlit arcs, dim ink, and arcs that
  // cycle while connecting.
  struct GlyphLook {
    const char* glyph = "wifi";
    const char* under = nullptr;
    bool dim = false;
    bool cycling = false;
  };
  [[nodiscard]] GlyphLook glyphLookForState(const NetworkState& state) noexcept;
  // The glyph of glyphLookForState, for one-glyph surfaces.
  [[nodiscard]] const char* glyphForState(const NetworkState& state) noexcept;
  // Glyphs a connecting Wi-Fi cycles through, weakest first.
  inline constexpr const char* kWifiCycle[] = {"wifi-1", "wifi-2", "wifi"};
  [[nodiscard]] const char* vpnGlyph() noexcept;
  // A VPN that is not up yet; drawn dim.
  [[nodiscard]] const char* vpnActivatingGlyph() noexcept;
  [[nodiscard]] const char* wifiGlyphForState(const NetworkState& state) noexcept;
  [[nodiscard]] const char* wifiGlyphForSignal(std::uint8_t signal) noexcept;
  // Signal band 0 (weakest) .. 4 (strongest) — the bands the wifi glyph draws.
  // Signal-ordered UI sorts on this rather than the raw percent, which jitters
  // on every scan update.
  [[nodiscard]] int wifiSignalBand(std::uint8_t signal) noexcept;

  // Radio band of an operating frequency: "2.4 GHz", "5 GHz", "6 GHz" or "60 GHz".
  // nullptr when the frequency is 0 or falls outside those bands. 802.11y 3.6 GHz
  // and S1G 900 MHz have no label here.
  [[nodiscard]] const char* wifiFrequencyBandLabel(std::uint32_t frequencyMhz) noexcept;

  // Cellular (ModemManager) glyphs, banded the same way as wifi.
  [[nodiscard]] const char* cellularGlyphForSignal(std::uint8_t signal) noexcept;
  [[nodiscard]] const char* cellularOffGlyph() noexcept;

} // namespace network_display
