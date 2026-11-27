#pragma once

#include "ui/palette.h"
#include "ui/signal.h"
#include "ui/style.h"
#include "wayland/dsk_surface_client.h"
#include "wayland/surface.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>


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

// Sends the island glass to `target` (`surface` or one of its shapes); none while `on` is false or with reduce
// transparency.
template <typename Target>
void sendIslandGlass(Target& target, const DskSurfaceClient& surface, bool on) {
  const glass::Material material = Style::islandGlass();
  setGlassMaterial(target, surface, on && !Style::reduceTransparency() ? &material : nullptr);
}

// The island glass tint value: the material tint where the compositor draws it (`compositorTint`), else 0.
[[nodiscard]] inline DskSurfaceClient::Value islandGlassTint(bool compositorTint, const Node* node) noexcept {
  return glassTintValue(materialRole(), compositorTint ? Style::islandGlass().tint : 0.0F, node);
}

// Sets the island tone on `node`, the root of a surface that carries the island glass.
template <typename N> void applyIslandTone(N& node) {
  node.setPaletteTone(glassTone(glass::Kind::Shape));
  node.setContentShadow(glassContentShadow());
}

// Sets the tone of a surface root drawn with no glass body (a bare bar): its ink follows the backdrop under it.
template <typename N> void applyBackdropTone(N& node, PaletteTone tone) {
  node.setPaletteTone(tone);
  node.setContentShadow(false);
}

// Sets the island tone on the node in `root` now and after each glass change, then runs `then` after each change. A
// node put in the slot later takes the tone at the next change; the slot must outlive the connection.
template <typename Ptr>
[[nodiscard]] Signal<>::ScopedConnection bindIslandTone(const Ptr& root, std::function<void()> then = {}) {
  if (root != nullptr) {
    applyIslandTone(*root);
  }
  return Style::glassChanged().connect([&root, then = std::move(then)] {
    if (root != nullptr) {
      applyIslandTone(*root);
    }
    if (then) {
      then();
    }
  });
}

// Sends a surface's glass::Material on the surface's own dsk object and sends it again when the slider or the theme
// changes.
class GlassSurface {
public:
  using Kind = glass::Kind;

  // Binds to `surface` once; later calls with the same surface only resend. Requests apply on the next commit.
  void apply(WaylandSurface& surface, Kind kind) {
    if (m_surface != &surface) {
      m_dsk = surface.dsk();
      if (m_dsk == nullptr) {
        return;
      }
      m_surface = &surface;
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
    m_dsk = nullptr;
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

  DskSurfaceClient* m_dsk = nullptr; // m_surface's own
  WaylandSurface* m_surface = nullptr;
  Kind m_kind = Kind::Sheet;
  std::optional<std::array<DskSurfaceClient::Value, 2>> m_sent;
  Signal<>::ScopedConnection m_paletteConn;
};
