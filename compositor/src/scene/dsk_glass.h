#pragma once

#include "core/animation.h"
#include "server/dsk_surface.h"

#include <array>
#include <optional>

namespace andromeda {

  // The glass a client declares over dsk_surface_v1: Liquid [refraction, rim, lens, dense], Blur and Tint.
  struct DskGlass {
    float liquid = 0.0F;
    float rim = 0.0F;
    float lens = 0.0F;
    // glass::Material::dense: how far the density raises the tint.
    float dense = 0.0F;
    // 0 blurs at full size, 1 not at all.
    float clarity = 0.0F;
    // Straight rgba; the compositor draws it over the blur, raised by the density.
    std::array<float, 4> tint{};

    [[nodiscard]] bool seeThrough() const { return liquid > 0.0F || rim > 0.0F; }
    bool operator==(const DskGlass&) const = default;
  };

  // The glass `motion` (the surface's or one shape's) declares, for a client at `state`'s protocol version.
  [[nodiscard]] DskGlass readDskGlass(const DskSurfaceManager::State& state, const SurfaceMotion& motion);

  // How far a glass surface raises its tint over its backdrop (glass::adaptDensity), gliding over glass::kAdaptFadeMs.
  // Light glass (dark ink) answers its darkest backdrop spot, dark glass its brightest.
  class GlassDensity {
  public:
    // A new backdrop sample. True when the density starts a glide: the owner schedules a frame.
    bool sample(const DskGlass& glass, float darkest, float brightest);
    // The tint changed: its tone can move the target. True when a glide starts.
    bool retint(const DskGlass& glass);
    // Back to no sample, as before the first map.
    void reset();
    bool tick(double nowMsec) { return m_density.tick(nowMsec); }
    [[nodiscard]] bool animating() const { return m_density.animating(); }
    [[nodiscard]] float target() const { return m_target; }
    // A backdrop sample has arrived since the last reset.
    [[nodiscard]] bool measured() const { return m_backdrop.has_value(); }
    // The tint alpha drawn over the blur for a tint of alpha `alpha`.
    [[nodiscard]] float amount(float alpha, float dense) const;

  private:
    struct Backdrop {
      float darkest = 0.5F;
      float brightest = 0.5F;
    };
    bool apply(const DskGlass& glass);

    std::optional<Backdrop> m_backdrop;
    AnimatedValue m_density{0.0};
    float m_target = 0.0F;
    bool m_light = false;
  };

} // namespace andromeda
