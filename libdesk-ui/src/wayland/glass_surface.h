#pragma once

#include "ui/palette.h"
#include "ui/signal.h"
#include "ui/style.h"
#include "wayland/dsk_surface_client.h"

#include <array>
#include <memory>
#include <optional>

struct dsk_surface_manager_v1;
struct wl_surface;

// The dsk_surface_v1 liquid value of a material: {level, rim, lens, dense}.
[[nodiscard]] inline DskSurfaceClient::Value glassLiquid(const glass::Material& material) noexcept {
  return {material.level, material.rim, material.lens, material.dense};
}

// The dsk_surface_v1 blur value (v15) of a material: {clarity}.
[[nodiscard]] inline DskSurfaceClient::Value glassBlur(const glass::Material& material) noexcept {
  return {material.clarity, 0.0, 0.0, 0.0};
}

// Sets a material's liquid and blur on `target` (`surface` or one of its shapes); nullptr turns the glass off. Before
// v15 the compositor takes the blur from the liquid value.
template <typename Target>
void setGlassMaterial(Target& target, const DskSurfaceClient& surface, const glass::Material* material) {
  target.set(DskSurfaceClient::Property::Liquid, material != nullptr ? glassLiquid(*material) : DskSurfaceClient::Value{});
  if (surface.hasBlurProperty()) {
    target.set(DskSurfaceClient::Property::Blur, material != nullptr ? glassBlur(*material) : DskSurfaceClient::Value{});
  }
}

// setGlassMaterial on a compositor curve.
template <typename Target>
void animateGlassMaterial(
    Target& target, const DskSurfaceClient& surface, DskSurfaceClient::Curve curve, const glass::Material& material
) {
  target.animate(DskSurfaceClient::Property::Liquid, curve, glassLiquid(material));
  if (surface.hasBlurProperty()) {
    target.animate(DskSurfaceClient::Property::Blur, curve, glassBlur(material));
  }
}

// The dsk_surface_v1 tint value (v9): the tint the compositor draws over the blur, `role` at `alpha`; 0 with reduce
// transparency. A surface that sends alpha > 0 paints no material fill of its own. `node` picks the palette tone.
[[nodiscard]] inline DskSurfaceClient::Value
glassTintValue(ColorRole role, float alpha, const Node* node = nullptr) noexcept {
  const Color& c = colorForRole(role, node);
  return {c.r, c.g, c.b, Style::reduceTransparency() ? 0.0 : alpha};
}

// Sends a surface's glass::Material to the compositor and sends it again when the slider or the theme changes. For
// surfaces with no DskSurfaceClient of their own; a surface has at most one.
class GlassSurface {
public:
  using Kind = glass::Kind;

  // Binds to `surface` once; later calls with the same surface only resend. Requests apply on the next commit.
  void apply(dsk_surface_manager_v1* manager, wl_surface* surface, Kind kind) {
    if (manager == nullptr || surface == nullptr) {
      return;
    }
    if (m_surface != surface) {
      m_dsk = std::make_unique<DskSurfaceClient>(manager, surface);
      m_surface = surface;
      m_paletteConn = paletteChanged().connect([this] { send(); });
      m_sent.reset();
    }
    m_kind = kind;
    send();
  }

  // Whole-surface opacity, blur included. False before apply() bound a surface.
  bool setOpacity(double opacity) {
    if (m_dsk == nullptr) {
      return false;
    }
    m_dsk->set(DskSurfaceClient::Property::Opacity, {opacity, 0.0, 0.0, 0.0});
    return true;
  }
  // The compositor runs the whole-surface opacity to `opacity` on `curve`; a set or animate replaces it.
  bool animateOpacity(double opacity, DskSurfaceClient::Curve curve) {
    if (m_dsk == nullptr) {
      return false;
    }
    m_dsk->animate(DskSurfaceClient::Property::Opacity, curve, {opacity, 0.0, 0.0, 0.0});
    return true;
  }

  // Any other whole-surface property (scale, translate); false before apply() bound a surface.
  bool set(DskSurfaceClient::Property property, const DskSurfaceClient::Value& value) {
    if (m_dsk == nullptr) {
      return false;
    }
    m_dsk->set(property, value);
    return true;
  }
  bool animate(
      DskSurfaceClient::Property property, DskSurfaceClient::Curve curve, const DskSurfaceClient::Value& to,
      DskSurfaceClient::DoneCallback onDone = {}
  ) {
    if (m_dsk == nullptr) {
      return false;
    }
    m_dsk->animate(property, curve, to, std::move(onDone));
    return true;
  }

  void reset() {
    m_paletteConn = {};
    m_dsk.reset();
    m_surface = nullptr;
  }

private:
  void send() {
    if (m_dsk == nullptr || !m_dsk->hasLiquidProperty()) {
      return;
    }
    // A light/dark fade emits paletteChanged every frame; the values only change with the glass settings.
    const glass::Material material = Style::glassMaterial(m_kind);
    const std::array<DskSurfaceClient::Value, 2> next{glassLiquid(material), glassBlur(material)};
    if (next == m_sent) {
      return;
    }
    m_sent = next;
    setGlassMaterial(*m_dsk, *m_dsk, &material);
  }

  std::unique_ptr<DskSurfaceClient> m_dsk;
  wl_surface* m_surface = nullptr;
  Kind m_kind = Kind::Sheet;
  std::optional<std::array<DskSurfaceClient::Value, 2>> m_sent;
  Signal<>::ScopedConnection m_paletteConn;
};
